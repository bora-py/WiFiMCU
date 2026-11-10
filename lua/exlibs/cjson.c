/**
 * cjson.c - Lightweight JSON parser and serializer for WiFiMCU (Lua 5.1 / RoTable)
 * Clean embedded implementation with no external dependencies.
 */

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lrotable.h"
#include "user_config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#ifdef USE_CJSON_MODULE

/* --- JSON ENCODER --- */
static void json_encode_value(lua_State *L, luaL_Buffer *b, int index, int depth);

static void json_encode_string(luaL_Buffer *b, const char *s, size_t len) {
    luaL_addchar(b, '"');
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  luaL_addstring(b, "\\\""); break;
            case '\\': luaL_addstring(b, "\\\\"); break;
            case '\b': luaL_addstring(b, "\\b");  break;
            case '\f': luaL_addstring(b, "\\f");  break;
            case '\n': luaL_addstring(b, "\\n");  break;
            case '\r': luaL_addstring(b, "\\r");  break;
            case '\t': luaL_addstring(b, "\\t");  break;
            default:
                if (c < 32) {
                    char buf[8];
                    sprintf(buf, "\\u%04x", c);
                    luaL_addstring(b, buf);
                } else {
                    luaL_addchar(b, c);
                }
                break;
        }
    }
    luaL_addchar(b, '"');
}

static int is_array(lua_State *L, int index, int *max_key) {
    int max = 0;
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) == LUA_TNUMBER) {
            int k = (int)lua_tonumber(L, -2);
            if (k <= 0) {
                lua_pop(L, 2);
                return 0;
            }
            if (k > max) max = k;
            count++;
        } else {
            lua_pop(L, 2);
            return 0;
        }
        lua_pop(L, 1);
    }
    *max_key = max;
    return (count == max);
}

static void json_encode_table(lua_State *L, luaL_Buffer *b, int index, int depth) {
    if (depth > 16) {
        luaL_error(L, "cjson: nested table structure too deep");
        return;
    }
    int max_key = 0;
    if (is_array(L, index, &max_key)) {
        luaL_addchar(b, '[');
        for (int i = 1; i <= max_key; i++) {
            if (i > 1) luaL_addchar(b, ',');
            lua_rawgeti(L, index, i);
            json_encode_value(L, b, lua_gettop(L), depth + 1);
            lua_pop(L, 1);
        }
        luaL_addchar(b, ']');
    } else {
        luaL_addchar(b, '{');
        int first = 1;
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            if (!first) luaL_addchar(b, ',');
            first = 0;
            if (lua_type(L, -2) == LUA_TSTRING) {
                size_t klen;
                const char *k = lua_tolstring(L, -2, &klen);
                json_encode_string(b, k, klen);
            } else {
                size_t klen;
                lua_pushvalue(L, -2);
                const char *k = lua_tolstring(L, -1, &klen);
                json_encode_string(b, k ? k : "", klen);
                lua_pop(L, 1);
            }
            luaL_addchar(b, ':');
            json_encode_value(L, b, lua_gettop(L), depth + 1);
            lua_pop(L, 1);
        }
        luaL_addchar(b, '}');
    }
}

static void json_encode_value(lua_State *L, luaL_Buffer *b, int index, int depth) {
    int t = lua_type(L, index);
    switch (t) {
        case LUA_TNIL:
            luaL_addstring(b, "null");
            break;
        case LUA_TBOOLEAN:
            luaL_addstring(b, lua_toboolean(L, index) ? "true" : "false");
            break;
        case LUA_TNUMBER: {
            lua_Number n = lua_tonumber(L, index);
            char numbuf[32];
            if (n == (lua_Integer)n) {
                sprintf(numbuf, "%ld", (long)(lua_Integer)n);
            } else {
                sprintf(numbuf, "%.4f", (double)n);
                // trim trailing zeroes after dot
                char *p = numbuf + strlen(numbuf) - 1;
                while (p > numbuf && *p == '0') { *p = '\0'; p--; }
                if (*p == '.') *p = '\0';
            }
            luaL_addstring(b, numbuf);
            break;
        }
        case LUA_TSTRING: {
            size_t len;
            const char *s = lua_tolstring(L, index, &len);
            json_encode_string(b, s, len);
            break;
        }
        case LUA_TTABLE:
            json_encode_table(L, b, index, depth);
            break;
        default:
            luaL_addstring(b, "null");
            break;
    }
}

static int lcjson_encode(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    json_encode_value(L, &b, 1, 0);
    luaL_pushresult(&b);
    return 1;
}

/* --- JSON DECODER --- */
typedef struct {
    const char *ptr;
    const char *end;
} JsonParser;

static void skip_whitespace(JsonParser *p) {
    while (p->ptr < p->end && isspace((unsigned char)*p->ptr)) {
        p->ptr++;
    }
}

static int parse_value(lua_State *L, JsonParser *p, int depth);

static int parse_string(lua_State *L, JsonParser *p) {
    if (p->ptr >= p->end || *p->ptr != '"') return 0;
    p->ptr++; // skip opening quote
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    while (p->ptr < p->end) {
        char c = *p->ptr++;
        if (c == '"') {
            luaL_pushresult(&b);
            return 1;
        }
        if (c == '\\') {
            if (p->ptr >= p->end) return 0;
            char esc = *p->ptr++;
            switch (esc) {
                case '"':  luaL_addchar(&b, '"');  break;
                case '\\': luaL_addchar(&b, '\\'); break;
                case '/':  luaL_addchar(&b, '/');  break;
                case 'b':  luaL_addchar(&b, '\b'); break;
                case 'f':  luaL_addchar(&b, '\f'); break;
                case 'n':  luaL_addchar(&b, '\n'); break;
                case 'r':  luaL_addchar(&b, '\r'); break;
                case 't':  luaL_addchar(&b, '\t'); break;
                case 'u': {
                    if (p->ptr + 4 > p->end) return 0;
                    char hex[5] = { p->ptr[0], p->ptr[1], p->ptr[2], p->ptr[3], 0 };
                    p->ptr += 4;
                    unsigned long u = strtoul(hex, NULL, 16);
                    if (u < 128) luaL_addchar(&b, (char)u);
                    else luaL_addchar(&b, '?');
                    break;
                }
                default: luaL_addchar(&b, esc); break;
            }
        } else {
            luaL_addchar(&b, c);
        }
    }
    return 0;
}

static int parse_number(lua_State *L, JsonParser *p) {
    const char *start = p->ptr;
    if (p->ptr < p->end && (*p->ptr == '-' || *p->ptr == '+')) p->ptr++;
    while (p->ptr < p->end && isdigit((unsigned char)*p->ptr)) p->ptr++;
    int is_float = 0;
    if (p->ptr < p->end && *p->ptr == '.') {
        is_float = 1;
        p->ptr++;
        while (p->ptr < p->end && isdigit((unsigned char)*p->ptr)) p->ptr++;
    }
    if (p->ptr < p->end && (*p->ptr == 'e' || *p->ptr == 'E')) {
        is_float = 1;
        p->ptr++;
        if (p->ptr < p->end && (*p->ptr == '-' || *p->ptr == '+')) p->ptr++;
        while (p->ptr < p->end && isdigit((unsigned char)*p->ptr)) p->ptr++;
    }
    size_t len = p->ptr - start;
    char numbuf[64];
    if (len >= sizeof(numbuf)) len = sizeof(numbuf) - 1;
    strncpy(numbuf, start, len);
    numbuf[len] = '\0';
    if (is_float) {
        lua_pushnumber(L, (lua_Number)atof(numbuf));
    } else {
        lua_pushinteger(L, (lua_Integer)atol(numbuf));
    }
    return 1;
}

static int parse_array(lua_State *L, JsonParser *p, int depth) {
    p->ptr++; // skip '['
    lua_newtable(L);
    int idx = 1;
    skip_whitespace(p);
    if (p->ptr < p->end && *p->ptr == ']') {
        p->ptr++;
        return 1;
    }
    while (p->ptr < p->end) {
        if (!parse_value(L, p, depth + 1)) return 0;
        lua_rawseti(L, -2, idx++);
        skip_whitespace(p);
        if (p->ptr < p->end && *p->ptr == ']') {
            p->ptr++;
            return 1;
        }
        if (p->ptr < p->end && *p->ptr == ',') {
            p->ptr++;
        } else {
            return 0;
        }
    }
    return 0;
}

static int parse_object(lua_State *L, JsonParser *p, int depth) {
    p->ptr++; // skip '{'
    lua_newtable(L);
    skip_whitespace(p);
    if (p->ptr < p->end && *p->ptr == '}') {
        p->ptr++;
        return 1;
    }
    while (p->ptr < p->end) {
        skip_whitespace(p);
        if (!parse_string(L, p)) return 0; // leaves key on top
        skip_whitespace(p);
        if (p->ptr >= p->end || *p->ptr != ':') return 0;
        p->ptr++; // skip ':'
        if (!parse_value(L, p, depth + 1)) return 0; // leaves val on top
        lua_settable(L, -3);
        skip_whitespace(p);
        if (p->ptr < p->end && *p->ptr == '}') {
            p->ptr++;
            return 1;
        }
        if (p->ptr < p->end && *p->ptr == ',') {
            p->ptr++;
        } else {
            return 0;
        }
    }
    return 0;
}

static int parse_value(lua_State *L, JsonParser *p, int depth) {
    if (depth > 16) return 0;
    skip_whitespace(p);
    if (p->ptr >= p->end) return 0;
    char c = *p->ptr;
    if (c == '"') return parse_string(L, p);
    if (c == '{') return parse_object(L, p, depth);
    if (c == '[') return parse_array(L, p, depth);
    if (c == '-' || isdigit((unsigned char)c)) return parse_number(L, p);
    if (p->end - p->ptr >= 4 && strncmp(p->ptr, "true", 4) == 0) {
        p->ptr += 4;
        lua_pushboolean(L, 1);
        return 1;
    }
    if (p->end - p->ptr >= 5 && strncmp(p->ptr, "false", 5) == 0) {
        p->ptr += 5;
        lua_pushboolean(L, 0);
        return 1;
    }
    if (p->end - p->ptr >= 4 && strncmp(p->ptr, "null", 4) == 0) {
        p->ptr += 4;
        lua_pushnil(L);
        return 1;
    }
    return 0;
}

static int lcjson_decode(lua_State *L) {
    size_t len;
    const char *str = luaL_checklstring(L, 1, &len);
    JsonParser p = { str, str + len };
    if (!parse_value(L, &p, 0)) {
        return luaL_error(L, "cjson: malformed JSON string");
    }
    return 1;
}

#define MIN_OPT_LEVEL 2
#include "lrodefs.h"

const LUA_REG_TYPE cjson_map[] = {
    { LSTRKEY("encode"), LFUNCVAL(lcjson_encode) },
    { LSTRKEY("decode"), LFUNCVAL(lcjson_decode) },
    { LNILKEY, LNILVAL }
};

LUALIB_API int luaopen_cjson(lua_State *L) {
#if LUA_OPTIMIZE_MEMORY > 0
    return 0;
#else
    luaL_register(L, "cjson", cjson_map);
    return 1;
#endif
}

#endif /* USE_CJSON_MODULE */

