/* ============================================================================
 * SQLite abstraction
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "botlib.h"
#include "sds.h"
#include "sqlite_wrap.h"

static constexpr bool SHOW_QUERY_ERRORS = true;

/* This is the low level function that we use to model all the higher level
 * functions.
 *
 * Queries can contain ?s ?b ?i and ?d special specifiers that are bound to
 * the SQL query, and must be present later as additional arguments after
 * the 'sql' argument.
 *
 *  ?s      -- TEXT field: char* argument.
 *  ?b      -- Blob field: char* argument followed by size_t argument.
 *  ?i      -- INT field : int64_t argument.
 *  ?d      -- REAL field: double argument.
 *
 * The function returns the return code of the last SQLite query that
 * failed on error. On success it returns what sqlite3_step() returns.
 * If the function returns SQLITE_ROW, that is, if the query is
 * returning data, the function returns, by reference, a sqlRow object
 * that the caller can use to get the current and next rows.
 *
 * The user needs to later free this sqlRow object with sqlEnd() (but this
 * is done automatically if all the rows are consumed with sqlNextRow()).
 * Note that is valid to call sqlEnd() even if the query didn't return
 * SQLITE_ROW, since in such case row->stmt is set to nullptr.
 */
[[nodiscard]] int sqlGenericQuery(sqlite3 *dbhandle, sqlRow *row,
                                  const char *sql, va_list ap) {
  int rc = SQLITE_ERROR;
  sqlite3_stmt *stmt = nullptr;
  sds query = sdsempty();
  if (row)
    row->stmt = nullptr; /* On error sqlNextRow() should return false. */

  /* We need to build the query, substituting the following three
   * classes of patterns with just "?", remembering the order and
   * type, and later using the sql3 binding API in order to prepare
   * the query:
   *
   * ?s string
   * ?b blob (varargs must have char ptr and size_t len)
   * ?i int64_t
   * ?d double
   */
  char spec[SQL_MAX_SPEC];
  size_t numspec = 0;
  const char *p = sql;
  while (p[0]) {
    if (p[0] == '?') {
      if (p[1] == 's' || p[1] == 'i' || p[1] == 'd' || p[1] == 'b') {
        if (numspec == SQL_MAX_SPEC)
          goto error;
        spec[numspec++] = p[1];
      } else {
        goto error;
      }
      query = sdscatlen(query, "?", 1);
      p++; /* Skip the specifier. */
    } else {
      query = sdscatlen(query, p, 1);
    }
    p++;
  }

  /* Prepare the query and bind the query arguments. */
  rc = sqlite3_prepare_v2(dbhandle, query, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    if (SHOW_QUERY_ERRORS)
      printf("%p: Query error: %s: %s\n", (void *)dbhandle, query,
             sqlite3_errmsg(dbhandle));
    goto error;
  }

  for (size_t j = 0; j < numspec; j++) {
    const int bind_index = (int)(j + 1);
    switch (spec[j]) {
    case 'b': {
      char *blobptr = va_arg(ap, char *);
      size_t bloblen = va_arg(ap, size_t);
      rc = sqlite3_bind_blob64(stmt, bind_index, blobptr, bloblen, nullptr);
    } break;
    case 's':
      rc = sqlite3_bind_text(stmt, bind_index, va_arg(ap, char *), -1, nullptr);
      break;
    case 'i':
      rc = sqlite3_bind_int64(stmt, bind_index, va_arg(ap, int64_t));
      break;
    case 'd':
      rc = sqlite3_bind_double(stmt, bind_index, va_arg(ap, double));
      break;
    }
    if (rc != SQLITE_OK)
      goto error;
  }

  /* Execute. */
  rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    if (row) {
      row->stmt = stmt;
      row->cols = 0;
      row->col = nullptr;
      stmt = nullptr; /* Don't free it on cleanup. */
    }
  }

error:
  if (stmt)
    sqlite3_finalize(stmt);
  sdsfree(query);
  return rc;
}

/* This function should be called only if you don't get all the rows
 * till the end. It is safe to call anyway. */
void sqlEnd(sqlRow *row) {
  if (row->stmt == nullptr)
    return;
  xfree(row->col);
  sqlite3_finalize(row->stmt);
  row->col = nullptr;
  row->stmt = nullptr;
}

/* After sqlGenericQuery() returns SQLITE_ROW, you can call this function
 * with the 'row' object pointer in order to get the rows composing the
 * result set. It returns true if the next row is available, otherwise false
 * is returned (and the row object is freed). If you stop the iteration
 * before all the elements are used, you need to call sqlEnd(). */
[[nodiscard]] bool sqlNextRow(sqlRow *row) {
  if (row->stmt == nullptr)
    return false;

  if (row->col != nullptr) {
    if (sqlite3_step(row->stmt) != SQLITE_ROW) {
      sqlEnd(row);
      return false;
    }
  }

  xfree(row->col);
  row->cols = sqlite3_data_count(row->stmt);
  row->col = xmalloc(row->cols * sizeof(sqlCol));
  for (int j = 0; j < row->cols; j++) {
    row->col[j].type = sqlite3_column_type(row->stmt, j);
    if (row->col[j].type == SQLITE_INTEGER) {
      row->col[j].i = sqlite3_column_int64(row->stmt, j);
    } else if (row->col[j].type == SQLITE_FLOAT) {
      row->col[j].d = sqlite3_column_double(row->stmt, j);
    } else if (row->col[j].type == SQLITE_TEXT) {
      row->col[j].s = (char *)sqlite3_column_text(row->stmt, j);
      row->col[j].i = sqlite3_column_bytes(row->stmt, j);
    } else if (row->col[j].type == SQLITE_BLOB) {
      row->col[j].s = sqlite3_column_blob(row->stmt, j);
      row->col[j].i = sqlite3_column_bytes(row->stmt, j);
    } else {
      /* SQLITE_NULL. */
      row->col[j].s = nullptr;
      row->col[j].i = 0;
      row->col[j].d = 0;
    }
  }
  return true;
}

/* Wrapper for sqlGenericQuery() returning the last inserted ID or 0
 * on error. */
[[nodiscard]] int64_t sqlInsert(sqlite3 *dbhandle, const char *sql, ...) {
  int64_t lastid = 0;
  va_list ap;
  va_start(ap, sql);
  int rc = sqlGenericQuery(dbhandle, nullptr, sql, ap);
  if (rc == SQLITE_DONE)
    lastid = sqlite3_last_insert_rowid(dbhandle);
  va_end(ap);
  return lastid;
}

/* Wrapper for sqlGenericQuery() returning true if the query resulted in
 * SQLITE_DONE, otherwise false. This is good for UPDATE and DELETE
 * statements. */
[[nodiscard]] bool sqlQuery(sqlite3 *dbhandle, const char *sql, ...) {
  bool retval = false;
  va_list ap;
  va_start(ap, sql);
  int rc = sqlGenericQuery(dbhandle, nullptr, sql, ap);
  retval = (rc == SQLITE_DONE);
  va_end(ap);
  return retval;
}

/* Wrapper for sqlGenericQuery() using varialbe number of args.
 * This is what you want when doing SELECT queries. */
[[nodiscard]] int sqlSelect(sqlite3 *dbhandle, sqlRow *row, const char *sql,
                            ...) {
  va_list ap;
  va_start(ap, sql);
  int rc = sqlGenericQuery(dbhandle, row, sql, ap);
  va_end(ap);
  return rc;
}

/* Wrapper for sqlGenericQuery() using variable number of args.
 * This is what you want when doing SELECT queries that return a
 * single row. This function will care to also call sqlNextRow() for
 * you in case the return value is SQLITE_ROW. */
[[nodiscard]] int sqlSelectOneRow(sqlite3 *dbhandle, sqlRow *row,
                                  const char *sql, ...) {
  va_list ap;
  va_start(ap, sql);
  int rc = sqlGenericQuery(dbhandle, row, sql, ap);
  if (rc == SQLITE_ROW) {
    if (!sqlNextRow(row)) {
      rc = SQLITE_ERROR;
    }
  }
  va_end(ap);
  return rc;
}

/* Wrapper for sqlGenericQuery() to do a SELECT and return directly
 * the integer of the first row, or zero on error. */
[[nodiscard]] int64_t sqlSelectInt(sqlite3 *dbhandle, const char *sql, ...) {
  sqlRow row;
  int64_t i = 0;
  va_list ap;
  va_start(ap, sql);
  int rc = sqlGenericQuery(dbhandle, &row, sql, ap);
  if (rc == SQLITE_ROW) {
    if (sqlNextRow(&row)) {
      i = row.col[0].i;
      sqlEnd(&row);
    } else {
      rc = SQLITE_ERROR;
    }
  }
  va_end(ap);
  return i;
}

/* ==========================================================================
 * Key value store abstraction. This implements a trivial KV store on top
 * of SQLite. It only has SET, GET, DEL and support for a maximum time to live.
 * ======================================================================== */

/* Set the key to the specified value and expire time. An expire of zero
 * means the key should not be expired at all. Return true on success, or
 * false on error. */
[[nodiscard]] bool kvSetLen(sqlite3 *dbhandle, const char *key,
                            const char *value, size_t vlen, int64_t expire) {
  if (expire)
    expire += time(nullptr);
  if (!sqlInsert(dbhandle, "INSERT INTO KeyValue VALUES(?i,?s,?b)", expire, key,
                 value, vlen)) {
    if (!sqlQuery(dbhandle,
                  "UPDATE KeyValue SET expire=?i,value=?b WHERE key=?s", expire,
                  value, vlen, key)) {
      return false;
    }
  }
  return true;
}

/* Wrapper where the value len is obtained via strlen().*/
[[nodiscard]] bool kvSet(sqlite3 *dbhandle, const char *key, const char *value,
                         int64_t expire) {
  return kvSetLen(dbhandle, key, value, strlen(value), expire);
}

/* Get the specified key and return it as an SDS string. If the value is
 * expired or does not exist nullptr is returned. */
[[nodiscard]] sds kvGet(sqlite3 *dbhandle, const char *key) {
  sds value = nullptr;
  sqlRow row;
  const int rc = sqlSelect(
      dbhandle, &row, "SELECT expire,value FROM KeyValue WHERE key=?s", key);
  if (rc == SQLITE_ROW && sqlNextRow(&row)) {
    int64_t expire = row.col[0].i;
    if (expire && expire < time(nullptr)) {
      [[maybe_unused]] bool deleted =
          sqlQuery(dbhandle, "DELETE FROM KeyValue WHERE key=?s", key);
    } else {
      value = sdsnewlen(row.col[1].s, row.col[1].i);
    }
  }
  sqlEnd(&row);
  return value;
}

/* Delete the key if it exists. */
[[nodiscard]] bool kvDel(sqlite3 *dbhandle, const char *key) {
  return sqlQuery(dbhandle, "DELETE FROM KeyValue WHERE key=?s", key);
}
