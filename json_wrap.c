/* ============================================================================
 * JSON selector implementation using Parson.
 * ========================================================================= */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parson.h"

enum {
    JSEL_INVALID = 0,
    JSEL_OBJ = 1,      /* "." */
    JSEL_ARRAY = 2,    /* "[" */
    JSEL_TYPECHECK = 3 /* ":" */
};

static const size_t JSEL_MAX_TOKEN = 256;

static bool json_type_matches(const JSON_Value *value, char typecode) {
    const JSON_Value_Type t = json_value_get_type(value);
    return (typecode == 's' && t == JSONString) ||
           (typecode == 'n' && t == JSONNumber) ||
           (typecode == 'o' && t == JSONObject) ||
           (typecode == 'a' && t == JSONArray) ||
           (typecode == 'b' && t == JSONBoolean) ||
           (typecode == '!' && t == JSONNull);
}

/**
 * @brief Select a nested JSON value using a mini-language similar to the
 *        original cJSON_Select helper.
 *
 * Supports:
 *  - ".field" object lookup
 *  - "[index]" array indexing
 *  - ":<type>" terminal type checks (s/n/o/a/b/!)
 *  - "*" placeholder replaced via variadic arguments (int for arrays,
 *    const char* for objects)
 *
 * @return Pointer to the matching JSON value or nullptr on failure.
 */
[[nodiscard]] JSON_Value *json_select(JSON_Value *value, const char *fmt, ...) {
    if (value == nullptr || fmt == nullptr) {
        return nullptr;
    }

    int next = JSEL_INVALID;
    char token[JSEL_MAX_TOKEN + 1];
    size_t tlen = 0;
    va_list ap;

    va_start(ap, fmt);
    const char *p = fmt;
    JSON_Value *current = value;

    while (1) {
        if (tlen && (*p == '\0' || strchr(".[]:", *p))) {
            token[tlen] = '\0';
            if (next == JSEL_ARRAY) {
                if (json_value_get_type(current) != JSONArray) goto notfound;
                char *endptr = nullptr;
                const long idx = strtol(token, &endptr, 10);
                if (endptr == token || *endptr != '\0' || idx < 0) goto notfound;
                JSON_Array *array = json_value_get_array(current);
                if (array == nullptr) goto notfound;
                const size_t usize = (size_t) idx;
                if (usize >= json_array_get_count(array)) goto notfound;
                current = json_array_get_value(array, usize);
                if (current == nullptr) goto notfound;
            } else if (next == JSEL_OBJ) {
                if (json_value_get_type(current) != JSONObject) goto notfound;
                JSON_Object *obj = json_value_get_object(current);
                if (obj == nullptr) goto notfound;
                current = json_object_get_value(obj, token);
                if (current == nullptr) goto notfound;
            } else if (next == JSEL_TYPECHECK) {
                if (!json_type_matches(current, token[0])) goto notfound;
            } else {
                goto notfound;
            }
        } else if (next != JSEL_INVALID) {
            if (*p != '*') {
                if (tlen >= JSEL_MAX_TOKEN) goto notfound;
                token[tlen++] = *p++;
                continue;
            }

            if (next == JSEL_ARRAY) {
                const int idx = va_arg(ap, int);
                char buf[64];
                const int len = snprintf(buf, sizeof(buf), "%d", idx);
                if (len < 0 || len >= (int) sizeof(buf) ||
                    (size_t) len > JSEL_MAX_TOKEN - tlen) goto notfound;
                memcpy(token + tlen, buf, (size_t) len);
                tlen += (size_t) len;
            } else if (next == JSEL_OBJ) {
                const char *s = va_arg(ap, char *);
                if (s == nullptr) goto notfound;
                const size_t len = strlen(s);
                if (tlen + len > JSEL_MAX_TOKEN) goto notfound;
                memcpy(token + tlen, s, len);
                tlen += len;
            } else {
                goto notfound;
            }
            ++p;
            continue;
        }

        if (*p == ']') ++p;
        if (*p == '\0') break;
        if (*p == '.') next = JSEL_OBJ;
        else if (*p == '[') next = JSEL_ARRAY;
        else if (*p == ':') next = JSEL_TYPECHECK;
        else goto notfound;
        tlen = 0;
        ++p;
    }

cleanup:
    va_end(ap);
    return current;

notfound:
    current = nullptr;
    goto cleanup;
}
