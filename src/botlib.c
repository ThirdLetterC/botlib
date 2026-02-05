/* Copyright (c) 2023, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* Adding these for portablity */
#define _BSD_SOURCE
#if defined(__linux__)
#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <sqlite3.h>

#include "botlib.h"
#include "parson.h"
#include "sds.h"

/* Thread local and atomic state. */
_Thread_local sqlite3 *DbHandle = nullptr; /* Per-thread sqlite handle. */

/* The bot global state. */
struct {
  int debug;       /* If true enables debugging info (--debug option).
                      This gets incremented by one at every successive
                      --debug, so that very verbose stuff are only
                      enabled with a few --debug calls. */
  int verbose;     // If true enables verbose info.
  char *dbfile;    // Change with --dbfile.
  char **triggers; // Strings triggering processing.
  sds apikey;      // Telegram API key for the bot.
  sds username;    // Bot username from getMe call.
  TBRequestCallback req_callback; // Callback handling requests.
  TBCronCallback cron_callback;
} Bot;

/* Global stats. Sometimes we access such stats from threads without caring
 * about race conditions, since they in practice are very unlikely to happen
 * in most archs with this data types, and even so we don't care.
 * This stuff is reported by the bot when the $$ info command is used. */
struct {
  time_t start_time; /* Unix time the bot was started. */
  uint64_t queries;  /* Number of queries received. */
} botStats;

/* ============================================================================
 * Utils
 * ========================================================================= */

/* Glob-style pattern matching. Return 1 on match, 0 otherwise. */
int strmatch(const char *pattern, int patternLen, const char *string,
             int stringLen, int nocase) {
  while (patternLen && stringLen) {
    switch (pattern[0]) {
    case '*':
      while (patternLen && pattern[1] == '*') {
        pattern++;
        patternLen--;
      }
      if (patternLen == 1)
        return 1; /* match */
      while (stringLen) {
        if (strmatch(pattern + 1, patternLen - 1, string, stringLen, nocase))
          return 1; /* match */
        string++;
        stringLen--;
      }
      return 0; /* no match */
      break;
    case '?':
      string++;
      stringLen--;
      break;
    case '[': {
      int not, match;

      pattern++;
      patternLen--;
      not = pattern[0] == '^';
      if (not) {
        pattern++;
        patternLen--;
      }
      match = 0;
      while (1) {
        if (pattern[0] == '\\' && patternLen >= 2) {
          pattern++;
          patternLen--;
          if (pattern[0] == string[0])
            match = 1;
        } else if (pattern[0] == ']') {
          break;
        } else if (patternLen == 0) {
          pattern--;
          patternLen++;
          break;
        } else if (patternLen >= 3 && pattern[1] == '-') {
          int start = pattern[0];
          int end = pattern[2];
          int c = string[0];
          if (start > end) {
            int t = start;
            start = end;
            end = t;
          }
          if (nocase) {
            start = tolower(start);
            end = tolower(end);
            c = tolower(c);
          }
          pattern += 2;
          patternLen -= 2;
          if (c >= start && c <= end)
            match = 1;
        } else {
          if (!nocase) {
            if (pattern[0] == string[0])
              match = 1;
          } else {
            if (tolower((int)pattern[0]) == tolower((int)string[0]))
              match = 1;
          }
        }
        pattern++;
        patternLen--;
      }
      if (not)
        match = !match;
      if (!match)
        return 0; /* no match */
      string++;
      stringLen--;
      break;
    }
    case '\\':
      if (patternLen >= 2) {
        pattern++;
        patternLen--;
      }
      [[fallthrough]];
    default:
      if (!nocase) {
        if (pattern[0] != string[0])
          return 0; /* no match */
      } else {
        if (tolower((int)pattern[0]) != tolower((int)string[0]))
          return 0; /* no match */
      }
      string++;
      stringLen--;
      break;
    }
    pattern++;
    patternLen--;
    if (stringLen == 0) {
      while (*pattern == '*') {
        pattern++;
        patternLen--;
      }
      break;
    }
  }
  if (patternLen == 0 && stringLen == 0)
    return 1;
  return 0;
}

/* ============================================================================
 * Allocator wrapper: we want to exit on OOM instead of trying to recover.
 * ========================================================================= */

void *xmalloc(size_t size) {
  if (size == 0)
    size = 1;
  auto p = calloc(1, size);
  if (p == nullptr) {
    printf("Out of memory: calloc(%zu, 1)", size);
    exit(1);
  }
  return p;
}

void *xrealloc(void *ptr, size_t size) {
  if (size == 0)
    size = 1;
  auto p = realloc(ptr, size);
  if (p == nullptr) {
    printf("Out of memory: realloc(%zu)", size);
    exit(1);
  }
  return p;
}

void xfree(void *ptr) { free(ptr); }

/* ============================================================================
 * HTTP interface abstraction
 * ==========================================================================*/

/* The callback concatenating data arriving from CURL http requests into
 * a target SDS string. */
size_t makeHTTPGETCallWriterSDS(char *ptr, [[maybe_unused]] size_t size,
                                size_t nmemb, void *userdata) {
  sds *body = (sds *)userdata;
  *body = sdscatlen(*body, ptr, nmemb);
  return nmemb;
}

/* The callback writing the CURL reply to a file. */
size_t makeHTTPGETCallWriterFILE(char *ptr, [[maybe_unused]] size_t size,
                                 size_t nmemb, void *userdata) {
  FILE **fp = (FILE **)userdata;
  return fwrite(ptr, 1, nmemb, *fp);
}

/* Request the specified URL in a blocking way, returns the content (or
 * error string) as an SDS string. If 'resptr' is not nullptr, the integer
 * will be set, by reference, to 1 or 0 to indicate success or error.
 * The returned SDS string must be freed by the caller both in case of
 * error and success. */
[[nodiscard]] sds makeHTTPGETCall(const char *url, int *resptr) {
  if (Bot.debug)
    printf("HTTP GET %s\n", url);
  CURL *curl;
  CURLcode res;
  sds body = sdsempty();
  if (resptr)
    *resptr = 0;

  curl = curl_easy_init();
  if (curl == nullptr) {
    body = sdscat(body, "curl_easy_init failed");
    return body;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, makeHTTPGETCallWriterSDS);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15);

  /* Perform the request, res will get the return code */
  res = curl_easy_perform(curl);
  if (resptr)
    *resptr = res == CURLE_OK ? 1 : 0;

  /* Check for errors */
  if (res != CURLE_OK) {
    const char *errstr = curl_easy_strerror(res);
    body = sdscat(body, errstr);
  } else {
    /* Return 0 if the request worked but returned a 500 code. */
    long code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if ((code == 500 || code == 400) && resptr)
      *resptr = 0;
  }

  /* always cleanup */
  curl_easy_cleanup(curl);
  return body;
}

/* Like makeHTTPGETCall(), but the list of options will be concatenated to
 * the URL as a query string, and URL encoded as needed.
 * The option list array should contain optnum*2 strings, alternating
 * option names and values. */
[[nodiscard]] sds makeHTTPGETCallOpt(const char *url, int *resptr,
                                     char **optlist, int optnum) {
  sds fullurl = sdsnew(url);
  sds body = sdsempty();
  if (resptr)
    *resptr = 0;
  if (optnum)
    fullurl = sdscatlen(fullurl, "?", 1);
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    body = sdscat(body, "curl_easy_init failed");
    goto cleanup;
  }
  for (int j = 0; j < optnum; j++) {
    if (j > 0)
      fullurl = sdscatlen(fullurl, "&", 1);
    fullurl = sdscat(fullurl, optlist[j * 2]);
    fullurl = sdscatlen(fullurl, "=", 1);
    char *escaped = curl_easy_escape(curl, optlist[j * 2 + 1],
                                     strlen(optlist[j * 2 + 1]));
    if (escaped == nullptr) {
      body = sdscat(body, "curl_easy_escape failed");
      goto cleanup;
    }
    fullurl = sdscat(fullurl, escaped);
    curl_free(escaped);
  }
  sdsfree(body);
  body = makeHTTPGETCall(fullurl, resptr);

cleanup:
  if (curl)
    curl_easy_cleanup(curl);
  sdsfree(fullurl);
  return body;
}

/* Make an HTTP request to the Telegram bot API, where 'req' is the specified
 * action name. This is a low level API that is used by other bot APIs
 * in order to do higher level work. 'resptr' works the same as in
 * makeHTTPGETCall(). */
[[nodiscard]] sds makeGETBotRequest(const char *action, int *resptr,
                                    char **optlist, int numopt) {
  sds url = sdsnew("https://api.telegram.org/bot");
  url = sdscat(url, Bot.apikey);
  url = sdscatlen(url, "/", 1);
  url = sdscat(url, action);
  sds body = makeHTTPGETCallOpt(url, resptr, optlist, numopt);
  sdsfree(url);
  return body;
}

/* Send an image using the sendPhoto endpoint. Return 1 on success, 0
 * on error. */
[[nodiscard]] int botSendImage(int64_t target, char *filename) {
  CURLcode res;
  int retval = 0;
  sds strtarget = sdsfromlonglong(target);
  CURL *curl = curl_easy_init();
  curl_mime *mime = nullptr;
  sds body = nullptr;

  if (curl == nullptr)
    goto cleanup;
  mime = curl_mime_init(curl);
  if (mime == nullptr)
    goto cleanup;

  curl_mimepart *part = curl_mime_addpart(mime);
  if (part == nullptr || curl_mime_name(part, "chat_id") != CURLE_OK ||
      curl_mime_data(part, strtarget, CURL_ZERO_TERMINATED) != CURLE_OK) {
    goto cleanup;
  }

  part = curl_mime_addpart(mime);
  if (part == nullptr || curl_mime_name(part, "photo") != CURLE_OK ||
      curl_mime_filedata(part, filename) != CURLE_OK) {
    goto cleanup;
  }

  char url[1024];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto",
           Bot.apikey);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, makeHTTPGETCallWriterSDS);
  body = sdsempty(); // Accumulate the reply here.
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15);

  /* Perform the request, res will get the return code */
  res = curl_easy_perform(curl);

  /* Check for errors */
  if (res == CURLE_OK) {
    retval = 1;
    /* Return 0 if the request worked but returned a 500 code. */
    long code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (code == 500 || code == 400)
      retval = 0;
  } else {
    retval = 0;
  }

  if (retval == 0 && body != nullptr)
    printf("sendImage() error from Telegram API: %s\n", body);

cleanup:
  sdsfree(body);
  if (mime != nullptr)
    curl_mime_free(mime);
  if (curl != nullptr)
    curl_easy_cleanup(curl);
  sdsfree(strtarget);
  return retval;
}

/* =============================================================================
 * Higher level Telegram bot API.
 * ===========================================================================*/

/* Return the bot username. */
[[nodiscard]] char *botGetUsername() {
  int res;

  if (Bot.username)
    return Bot.username;
  sds body = makeGETBotRequest("getMe", &res, nullptr, 0);
  if (res == 0) {
    sdsfree(body);
    return nullptr;
  }

  JSON_Value *json = json_parse_string(body);
  if (json == nullptr) {
    sdsfree(body);
    return nullptr;
  }
  JSON_Value *username = json_select(json, ".result.username:s");
  const char *uname = username ? json_value_get_string(username) : nullptr;
  if (uname != nullptr)
    Bot.username = sdsnew(uname);
  sdsfree(body);
  json_value_free(json);
  return Bot.username;
}

/* Send a message to the specified channel, optionally as a reply to a
 * specific message (if reply_to is non zero).
 * Return 1 on success, 0 on error. */
[[nodiscard]] int botSendMessageAndGetInfo(int64_t target, sds text,
                                           int64_t reply_to, int64_t *chat_id,
                                           int64_t *message_id) {
  char *options[10];
  int optlen = 4;
  options[0] = "chat_id";
  options[1] = sdsfromlonglong(target);
  options[2] = "text";
  options[3] = text;
  options[4] = "parse_mode";
  options[5] = "Markdown";
  options[6] = "disable_web_page_preview";
  options[7] = "true";
  if (reply_to) {
    optlen++;
    options[8] = "reply_to_message_id";
    options[9] = sdsfromlonglong(reply_to);
  } else {
    options[9] = nullptr; /* So we can sdsfree it later without problems. */
  }

  int res;
  sds body = makeGETBotRequest("sendMessage", &res, options, optlen);

  if (chat_id || message_id) {
    JSON_Value *json = json_parse_string(body);
    if (json != nullptr) {
      JSON_Value *res = json_select(json, ".result.message_id:n");
      if (res != nullptr && message_id)
        *message_id = (int64_t)json_value_get_number(res);
      res = json_select(json, ".result.chat.id:n");
      if (res != nullptr && chat_id)
        *chat_id = (int64_t)json_value_get_number(res);
      json_value_free(json);
    }
  }

  sdsfree(body);
  sdsfree(options[1]);
  sdsfree(options[9]);
  return res;
}

/* Like botSendMessageWithInfo() but without returning by reference
 * the chat and message IDs that are only useful if you want to
 * edit the message later.
 * Return 1 on success, 0 on error. */
[[nodiscard]] int botSendMessage(int64_t target, sds text, int64_t reply_to) {
  return botSendMessageAndGetInfo(target, text, reply_to, nullptr, nullptr);
}

/* Send a message to the specified channel, optionally as a reply to a
 * specific message (if reply_to is non zero).
 * Return 1 on success, 0 on error. */
[[nodiscard]] int botEditMessageText(int64_t chat_id, int message_id,
                                     sds text) {
  char *options[10];
  int optlen = 5;
  options[0] = "chat_id";
  options[1] = sdsfromlonglong(chat_id);
  options[2] = "message_id";
  options[3] = sdsfromlonglong(message_id);
  options[4] = "text";
  options[5] = text;
  options[6] = "parse_mode";
  options[7] = "Markdown";
  options[8] = "disable_web_page_preview";
  options[9] = "true";

  int res;
  sds body = makeGETBotRequest("editMessageText", &res, options, optlen);
  sdsfree(body);
  sdsfree(options[1]);
  sdsfree(options[3]);
  return res;
}

/* This function should be called from the bot implementation callback.
 * If the bot request has a file (the user can see that by inspecting
 * the br->file_type field), then this function will attempt to download
 * the file from Telegram and store it in the current working directory
 * with the name 'br->file_id'.
 *
 * On success 1 is returned, otherwise 0.
 * When the function returns successfully, the caller can access
 * a file named 'br->file_id'. */
[[nodiscard]] int botGetFile(BotRequest *br, const char *target_filename) {
  /* 1. Get the file information and path. */
  char *options[2];
  options[0] = "file_id";
  options[1] = br->file_id;

  int res;
  sds body = makeGETBotRequest("getFile", &res, options, 1);
  if (res == 0) {
    sdsfree(body);
    return 0; // Error.
  }

  JSON_Value *json = json_parse_string(body);
  sds file_path = nullptr;
  if (json != nullptr) {
    JSON_Value *result = json_select(json, ".result.file_path:s");
    const char *path = result ? json_value_get_string(result) : nullptr;
    if (path != nullptr)
      file_path = sdsnew(path);
    json_value_free(json);
  }
  sdsfree(body);
  if (file_path == nullptr)
    return 0; // Error.

  /* 2. Get the file content. */
  CURL *curl = curl_easy_init();
  const char *output_filename = target_filename ? target_filename : br->file_id;
  FILE *fp = nullptr;
  int retval = 0;
  if (!curl)
    goto cleanup; // Error.

  /* We need to open a file for writing. We will be
   * using the curl callback in order to append to the
   * file. */
  fp = fopen(output_filename, "wb");
  if (fp == nullptr)
    goto cleanup; // We can't continue without the target file.

  char url[1024];
  snprintf(url, sizeof(url), "https://api.telegram.org/file/bot%s/%s",
           Bot.apikey, file_path);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, makeHTTPGETCallWriterFILE);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fp);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15);

  /* Perform the request and cleanup. */
  retval = curl_easy_perform(curl) == CURLE_OK ? 1 : 0;

cleanup:
  if (curl)
    curl_easy_cleanup(curl);
  if (fp)
    fclose(fp);
  /* Best effort removal of incomplete file. */
  if (retval == 0 && fp != nullptr)
    unlink(output_filename);
  sdsfree(file_path);
  return retval;
}

/* Free the bot request and associated data. */
void freeBotRequest(BotRequest *br) {
  sdsfreesplitres(br->argv, br->argc);
  sdsfree(br->request);
  sdsfree(br->file_id);
  sdsfree(br->from_username);
  if (br->mentions) {
    for (int j = 0; j < br->num_mentions; j++)
      sdsfree(br->mentions[j]);
    free(br->mentions);
  }
  free(br);
}

/* Create a bot request object and return it to the caller. */
BotRequest *createBotRequest() {
  BotRequest *br = calloc(1, sizeof(*br));
  if (br == nullptr) {
    printf("Out of memory: BotRequest\n");
    exit(1);
  }
  br->type = TB_TYPE_UNKNOWN;
  br->file_type = TB_FILE_TYPE_NONE;
  return br;
}

/* =============================================================================
 * Database abstraction
 * ===========================================================================*/

/* Create the SQLite tables if needed (if createdb is true), and return
 * the SQLite database handle. Return nullptr on error. */
sqlite3 *dbInit(const char *createdb_query) {
  sqlite3 *db;
  int rt = sqlite3_open(Bot.dbfile, &db);
  if (rt != SQLITE_OK) {
    fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
    return nullptr;
  }

  if (createdb_query) {
    char *errmsg;
    int rc = sqlite3_exec(db, createdb_query, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
      fprintf(stderr, "SQL error [%d]: %s\n", rc, errmsg);
      sqlite3_free(errmsg);
      sqlite3_close(db);
      return nullptr;
    }
  }
  return db;
}

/* Should be called every time a thread exits, so that if the thread has
 * an SQLite thread-local handle, it gets closed. */
void dbClose() {
  if (DbHandle)
    sqlite3_close(DbHandle);
  DbHandle = nullptr;
}

/* =============================================================================
 * Bot requests handling
 * ========================================================================== */

/* Request handling thread entry point. */
void *botHandleRequest(void *arg) {
  DbHandle = dbInit(nullptr);
  BotRequest *br = arg;
  if (DbHandle == nullptr) {
    freeBotRequest(br);
    return nullptr;
  }

  /* Parse the request as a command composed of arguments. */
  br->argv = sdssplitargs(br->request, &br->argc);
  Bot.req_callback(DbHandle, br);
  freeBotRequest(br);
  dbClose();
  return nullptr;
}

/* Get the updates from the Telegram API, process them, and return the
 * ID of the highest processed update.
 *
 * The offset is the last ID already processed, the timeout is the number
 * of seconds to wait in long polling in case no request is immediately
 * available. */
int64_t botProcessUpdates(int64_t offset, int timeout) {
  char *options[6];
  int res;

  options[0] = "offset";
  options[1] = sdsfromlonglong(offset + 1);
  options[2] = "timeout";
  options[3] = sdsfromlonglong(timeout);
  options[4] = "allowed_updates";
  options[5] = "message";
  sds body = makeGETBotRequest("getUpdates", &res, options, 3);
  sdsfree(options[1]);
  sdsfree(options[3]);

  /* If two --debug options are provided, log the whole Telegram
   * reply here. */
  if (Bot.debug >= 2)
    printf("RECEIVED FROM TELEGRAM API:\n%s\n", body);

  JSON_Value *json = json_parse_string(body);
  if (json == nullptr)
    goto fmterr;
  JSON_Value *result = json_select(json, ".result:a");
  if (result == nullptr)
    goto fmterr;
  JSON_Array *updates = json_value_get_array(result);
  if (updates == nullptr)
    goto fmterr;
  const size_t update_count = json_array_get_count(updates);
  for (size_t update_index = 0; update_index < update_count; ++update_index) {
    JSON_Value *update = json_array_get_value(updates, update_index);
    if (update == nullptr)
      continue;
    JSON_Value *update_id = json_select(update, ".update_id:n");
    if (update_id == nullptr)
      continue;
    int64_t thisoff = (int64_t)json_value_get_number(update_id);
    if (thisoff > offset)
      offset = thisoff;

    JSON_Value *msg = json_select(update, ".message");
    if (msg == nullptr)
      msg = json_select(update, ".channel_post");
    if (msg == nullptr)
      continue;

    JSON_Value *chatid = json_select(msg, ".chat.id:n");
    if (chatid == nullptr)
      continue;
    int64_t target = (int64_t)json_value_get_number(chatid);

    JSON_Value *fromid = json_select(msg, ".from.id:n");
    int64_t from = fromid ? (int64_t)json_value_get_number(fromid) : 0;

    JSON_Value *fromuser = json_select(msg, ".from.username:s");
    const char *from_username =
        fromuser ? json_value_get_string(fromuser) : "unknown";

    JSON_Value *msgid = json_select(msg, ".message_id:n");
    int64_t message_id = msgid ? (int64_t)json_value_get_number(msgid) : 0;

    JSON_Value *chattype = json_select(msg, ".chat.type:s");
    const char *ct = chattype ? json_value_get_string(chattype) : nullptr;
    int type = TB_TYPE_UNKNOWN;
    if (ct != nullptr) {
      if (!strcmp(ct, "private"))
        type = TB_TYPE_PRIVATE;
      else if (!strcmp(ct, "group"))
        type = TB_TYPE_GROUP;
      else if (!strcmp(ct, "supergroup"))
        type = TB_TYPE_SUPERGROUP;
      else if (!strcmp(ct, "channel"))
        type = TB_TYPE_CHANNEL;
    }

    JSON_Value *date = json_select(msg, ".date:n");
    if (date == nullptr)
      continue;
    time_t timestamp = (time_t)json_value_get_number(date);
    JSON_Value *text = json_select(msg, ".text:s");
    const char *textstr = text ? json_value_get_string(text) : nullptr;
    /* Text may be nullptr even if the message is valid but
     * is a voice message, image, ... .*/

    if (Bot.verbose)
      printf(".text (from: %lld, target: %lld): %s\n", (long long)from,
             (long long)target, textstr ? textstr : "<no text field>");

    /* Sanity check the request before starting the thread:
     * validate that is a request that is really targeting our bot
     * list of "triggers". */
    if (textstr && type != TB_TYPE_PRIVATE && Bot.triggers) {
      int j;
      for (j = 0; Bot.triggers[j]; j++) {
        if (strmatch(Bot.triggers[j], strlen(Bot.triggers[j]), textstr,
                     strlen(textstr), 1)) {
          break;
        }
      }
      if (Bot.triggers[j] == nullptr)
        continue; // No match.
    }
    if (time(nullptr) - timestamp > 60 * 5)
      continue; // Ignore stale messages

    /* At this point we are sure we are going to pass the request
     * to our callback. Prepare the request object. */
    sds request = sdsnew(textstr ? textstr : "");
    BotRequest *br = createBotRequest();
    br->request = request;
    br->from_username = sdsnew(from_username);

    /* Check for files. */
    JSON_Value *voice = json_select(msg, ".voice.file_id:s");
    if (voice) {
      const char *voice_id = json_value_get_string(voice);
      if (voice_id != nullptr) {
        br->file_type = TB_FILE_TYPE_VOICE_OGG;
        br->file_id = sdsnew(voice_id);
        JSON_Value *size = json_select(msg, ".voice.file_size:n");
        br->file_size = size ? json_value_get_number(size) : 0;
      }
    }

    /* Parse entities, filling the mentions array. */
    JSON_Value *entities_val = json_select(msg, ".entities:a");
    if (entities_val != nullptr) {
      JSON_Array *entities = json_value_get_array(entities_val);
      const size_t entity_count = json_array_get_count(entities);
      for (size_t entity_index = 0; entity_index < entity_count;
           ++entity_index) {
        JSON_Value *entity = json_array_get_value(entities, entity_index);
        if (entity == nullptr)
          continue;
        JSON_Value *et = json_select(entity, ".type:s");
        JSON_Value *offset = json_select(entity, ".offset:n");
        JSON_Value *length = json_select(entity, ".length:n");
        const char *etype = et ? json_value_get_string(et) : nullptr;
        if (etype && offset && length && !strcmp(etype, "mention")) {
          unsigned long off = (unsigned long)json_value_get_number(offset);
          unsigned long len = (unsigned long)json_value_get_number(length);
          /* Don't trust Telegram offsets inside our stirng. */
          if (off + len <= sdslen(br->request)) {
            sds mention = sdsnewlen(br->request + off, len);
            br->num_mentions++;
            br->mentions =
                xrealloc(br->mentions, (size_t)br->num_mentions * sizeof(sds));
            br->mentions[br->num_mentions - 1] = mention;
            /* Is the user addressing the bot? Set the flag. */
            if (Bot.username && !strcmp(Bot.username, mention + 1))
              br->bot_mentioned = true;
          }
        }
      }
    }

    br->type = type;
    br->from = from;
    br->target = target;
    br->msg_id = message_id;

    /* Spawn a thread that will handle the request. */
    botStats.queries++;
    pthread_t tid;
    if (pthread_create(&tid, nullptr, botHandleRequest, br) != 0) {
      freeBotRequest(br);
      continue;
    }
    if (pthread_detach(tid) != 0) {
      printf("warning: failed to detach worker thread\n");
    }
    if (Bot.verbose)
      printf("Starting thread to serve: \"%s\"\n", br->request);

    /* It's up to the callback to free the bot request with
     * freeBotRequest(). */
  }

fmterr:
  if (json != nullptr)
    json_value_free(json);
  sdsfree(body);
  return offset;
}

/* =============================================================================
 * Bot main loop
 * ===========================================================================*/

/* This is the bot main loop: we get messages using getUpdates in blocking
 * mode, but with a timeout. Then we serve requests as needed, and every
 * time we unblock, we check for completed requests (by the thread that
 * handles Yahoo Finance API calls). */
void botMain() {
  int64_t nextid = -100; /* Start getting the last 100 messages. */
  int previd;

  [[maybe_unused]] char *username = botGetUsername(); // Cached in Bot.username.
  while (1) {
    previd = nextid;
    nextid = botProcessUpdates(nextid, 1);
    /* We don't want to saturate all the CPU in a busy loop in case
     * the above call fails and returns immediately (for networking
     * errors for instance), so wait a bit at every cycle, but only
     * if we didn't made any progresses with the ID. */
    if (nextid == previd)
      usleep(100000);
    if (Bot.cron_callback)
      Bot.cron_callback(DbHandle);
  }
}

/* Check if a file named 'apikey.txt' exists, if so load the Telegram bot
 * API key from there. If the function is able to read the API key from
 * the file, as a side effect the global SDS string Bot.apikey is populated. */
void readApiKeyFromFile() {
  FILE *fp = fopen("apikey.txt", "r");
  if (fp == nullptr)
    return;
  char buf[1024];
  if (fgets(buf, sizeof(buf), fp) == nullptr) {
    fclose(fp);
    return;
  }
  buf[sizeof(buf) - 1] = '\0';
  fclose(fp);
  sdsfree(Bot.apikey);
  Bot.apikey = sdsnew(buf);
  Bot.apikey = sdstrim(Bot.apikey, " \t\r\n");
}

void resetBotStats() {
  botStats.start_time = time(nullptr);
  botStats.queries = 0;
}

[[nodiscard]] int startBot(const char *createdb_query, int argc, char **argv,
                           int flags, TBRequestCallback req_callback,
                           TBCronCallback cron_callback, char **triggers) {
  srand(time(nullptr));

  Bot.debug = 0;
  Bot.verbose = 0;
  Bot.dbfile = "./mybot.sqlite";
  Bot.triggers = triggers;
  Bot.apikey = nullptr;
  Bot.req_callback = req_callback;
  Bot.cron_callback = cron_callback;

  /* Parse options. */
  for (int j = 1; j < argc; j++) {
    int morearg = argc - j - 1;
    if (!strcmp(argv[j], "--debug")) {
      Bot.debug++;
      Bot.verbose = 1;
    } else if (!strcmp(argv[j], "--verbose")) {
      Bot.verbose = 1;
    } else if (!strcmp(argv[j], "--apikey") && morearg) {
      sdsfree(Bot.apikey);
      Bot.apikey = sdsnew(argv[++j]);
    } else if (!strcmp(argv[j], "--dbfile") && morearg) {
      Bot.dbfile = argv[++j];
    } else if (!(flags & TB_FLAGS_IGNORE_BAD_ARG)) {
      printf("Usage: %s [--apikey <apikey>] [--debug] [--verbose] "
             "[--dbfile <filename>]"
             "\n",
             argv[0]);
      exit(1);
    }
  }

  /* Initializations. Note that we don't redefine the SQLite allocator,
   * since SQLite errors are always handled by Stonky anyway. */
  curl_global_init(CURL_GLOBAL_DEFAULT);
  if (Bot.apikey == nullptr)
    readApiKeyFromFile();
  if (Bot.apikey == nullptr) {
    printf("Provide a bot API key via --apikey or storing a file named "
           "apikey.txt in the bot working directory.\n");
    exit(1);
  }
  resetBotStats();
  DbHandle = dbInit(createdb_query);
  if (DbHandle == nullptr)
    exit(1);
  json_set_allocation_functions(xmalloc, xfree);

  /* Enter the infinite loop handling the bot. */
  botMain();
  return 0;
}
