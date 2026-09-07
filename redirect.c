// Gianluca Mazzini @2026- Version 2.04

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <sqlite3.h>

#ifndef DB_PATH
#define DB_PATH "/home/tools/mcp/work/misc/redirect.db"
#endif

#define PORTAL_HOST "www.chaos.cc"
#define FALLBACK_URL "https://www.google.com"
#define DEFAULT_SMSAUTH_URL "https://www.mazzini.org/smsauth"
#define DEFAULT_SMSAUTH_CHANNEL "redirect"
#define SESSION_COOKIE "chaos_session"
#define LOGIN_COOKIE "chaos_login"
#define SESSION_IDLE 3600
#define AUTH_RETENTION 900
#define SHORTCUT_LEN 8
#define DEFAULT_STATS 20
#define SEARCH_SIZE 2048
#define MAX_BODY 16384
#define MAX_URL 4096
#define MAX_VALUE 2048
#define HTTP_TIMEOUT_MS 5000L

#define AUTH_PENDING 0
#define AUTH_OK 1
#define AUTH_KO 2

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} HttpBuffer;

static const char *db_path(void) {
    const char *p;

    p = getenv("REDIRECT_DB");
    if (p != NULL && *p != '\0') return p;
    return DB_PATH;
}

static const char *portal_host(void) {
    const char *p;

    p = getenv("REDIRECT_PORTAL_HOST");
    if (p != NULL && *p != '\0') return p;
    return PORTAL_HOST;
}

static int parse_uint(const char *s, int *value) {
    char *end;
    long v;

    if (s == NULL || *s == '\0') return 0;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < 0 || v > 2147483647L) return 0;
    *value = (int)v;
    return 1;
}

static int valid_header_value(const char *s) {
    if (s == NULL || *s == '\0') return 0;
    return strchr(s, '\r') == NULL && strchr(s, '\n') == NULL;
}

static int valid_web_url(const char *s) {
    size_t n;

    if (!valid_header_value(s)) return 0;
    n = strlen(s);
    if (n >= MAX_URL) return 0;
    return strncmp(s, "https://", 8) == 0 || strncmp(s, "http://", 7) == 0;
}

static int normalize_cell(const char *in, char *out, size_t outsz) {
    char tmp[32];
    size_t i, n;

    if (in == NULL || outsz < 2) return 0;
    n = 0;
    for (i = 0; in[i] != '\0'; i++) {
        if (isdigit((unsigned char)in[i])) {
            if (n + 1 >= sizeof(tmp)) return 0;
            tmp[n++] = in[i];
        }
    }
    tmp[n] = '\0';
    if (n == 12 && tmp[0] == '3' && tmp[1] == '9') {
        memmove(tmp, tmp + 2, n - 1);
        n -= 2;
    }
    if (n < 7 || n > 15 || n + 1 > outsz) return 0;
    memcpy(out, tmp, n + 1);
    return 1;
}

static int valid_shortcut(const char *s) {
    size_t i, n;

    if (s == NULL) return 0;
    n = strlen(s);
    if (n < 1 || n > 63) return 0;
    if (s[0] == '-' || s[n - 1] == '-') return 0;
    if (strcasecmp(s, "www") == 0) return 0;
    for (i = 0; i < n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'Z') ||
            (s[i] >= 'a' && s[i] <= 'z') || s[i] == '-')) return 0;
    }
    return 1;
}

static int make_origin(const char *shortcut, char *out, size_t outsz) {
    int n;

    if (!valid_shortcut(shortcut)) return 0;
    n = snprintf(out, outsz, "%s.chaos.cc", shortcut);
    return n > 0 && (size_t)n < outsz;
}

static void hex_encode(const unsigned char *in, size_t n, char *out) {
    static const char h[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 15];
    }
    out[n * 2] = '\0';
}

static int random_hex(size_t bytes, char *out) {
    unsigned char buf[64];

    if (bytes > sizeof(buf)) return 0;
    if (RAND_bytes(buf, (int)bytes) != 1) return 0;
    hex_encode(buf, bytes, out);
    return 1;
}

static int random_shortcut(char out[SHORTCUT_LEN + 1]) {
    static const char chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char b;
    size_t i;

    for (i = 0; i < SHORTCUT_LEN; i++) {
        do {
            if (RAND_bytes(&b, 1) != 1) return 0;
        } while (b >= 248);
        out[i] = chars[b % 62];
    }
    out[SHORTCUT_LEN] = '\0';
    return 1;
}

static int ct_equal(const char *a, const char *b) {
    size_t i, la, lb, n;
    unsigned char d;

    if (a == NULL || b == NULL) return 0;
    la = strlen(a);
    lb = strlen(b);
    n = la > lb ? la : lb;
    d = (unsigned char)(la ^ lb);
    for (i = 0; i < n; i++) {
        d |= (unsigned char)((i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0));
    }
    return d == 0;
}

static int hmac_hex(const char *secret, const char *body, char out[65]) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int n;

    n = 0;
    if (HMAC(EVP_sha256(), secret, (int)strlen(secret),
        (const unsigned char *)body, strlen(body), digest, &n) == NULL) return 0;
    if (n != 32) return 0;
    hex_encode(digest, n, out);
    return 1;
}

static int db_exec(sqlite3 *db, const char *sql) {
    char *error;
    int rc;

    error = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "redirect: %s\n", error != NULL ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        return 0;
    }
    return 1;
}

static int set_persist(sqlite3 *db) {
    sqlite3_stmt *st;
    int rc, ok;

    st = NULL;
    rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode=PERSIST", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    rc = sqlite3_step(st);
    ok = rc == SQLITE_ROW && sqlite3_column_text(st, 0) != NULL &&
        strcasecmp((const char *)sqlite3_column_text(st, 0), "persist") == 0;
    sqlite3_finalize(st);
    return ok;
}

static int column_exists(sqlite3 *db, const char *table, const char *column) {
    sqlite3_stmt *st;
    char sql[256];
    const unsigned char *name;
    int rc, found;

    if (snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table) >= (int)sizeof(sql)) return 0;
    st = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    found = 0;
    for (;;) {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) break;
        name = sqlite3_column_text(st, 1);
        if (name != NULL && strcmp((const char *)name, column) == 0) {
            found = 1;
            break;
        }
    }
    sqlite3_finalize(st);
    return found;
}

static int setting_get(sqlite3 *db, const char *name, char *out, size_t outsz) {
    sqlite3_stmt *st;
    const unsigned char *v;
    int rc;

    st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE name=?1", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        v = sqlite3_column_text(st, 0);
        if (v != NULL && strlen((const char *)v) + 1 <= outsz) {
            strcpy(out, (const char *)v);
            sqlite3_finalize(st);
            return 1;
        }
    }
    sqlite3_finalize(st);
    return 0;
}

static int setting_set(sqlite3 *db, const char *name, const char *value) {
    sqlite3_stmt *st;
    int rc;

    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO settings(name,value) VALUES(?1,?2) "
        "ON CONFLICT(name) DO UPDATE SET value=excluded.value", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

static int init_db(sqlite3 *db) {
    const char *schema;

    schema =
        "CREATE TABLE IF NOT EXISTS redirect("
        "origin TEXT PRIMARY KEY COLLATE NOCASE,"
        "destination TEXT NOT NULL,"
        "hit INTEGER NOT NULL DEFAULT 0,"
        "description TEXT NOT NULL DEFAULT '',"
        "cell TEXT NOT NULL DEFAULT ''"
        ");";
    if (!db_exec(db, schema)) return 0;
    if (!column_exists(db, "redirect", "last_hit") &&
        !db_exec(db, "ALTER TABLE redirect ADD COLUMN last_hit INTEGER NOT NULL DEFAULT 0")) return 0;
    if (!column_exists(db, "redirect", "created") &&
        !db_exec(db, "ALTER TABLE redirect ADD COLUMN created INTEGER NOT NULL DEFAULT 0")) return 0;
    if (!column_exists(db, "redirect", "updated") &&
        !db_exec(db, "ALTER TABLE redirect ADD COLUMN updated INTEGER NOT NULL DEFAULT 0")) return 0;

    schema =
        "CREATE TABLE IF NOT EXISTS users("
        "cell TEXT PRIMARY KEY,"
        "advanced INTEGER NOT NULL DEFAULT 0,"
        "created INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS sessions("
        "token TEXT PRIMARY KEY,"
        "cell TEXT NOT NULL,"
        "csrf TEXT NOT NULL,"
        "created INTEGER NOT NULL,"
        "last_seen INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS auth_pending("
        "token TEXT PRIMARY KEY,"
        "challenge TEXT NOT NULL UNIQUE,"
        "cell TEXT NOT NULL,"
        "code TEXT NOT NULL,"
        "number TEXT NOT NULL DEFAULT '',"
        "created INTEGER NOT NULL,"
        "expires INTEGER NOT NULL,"
        "state INTEGER NOT NULL DEFAULT 0,"
        "reason TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS settings("
        "name TEXT PRIMARY KEY,"
        "value TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS redirect_hit ON redirect(hit DESC);"
        "CREATE INDEX IF NOT EXISTS redirect_cell ON redirect(cell);"
        "CREATE INDEX IF NOT EXISTS redirect_last_hit ON redirect(last_hit DESC);"
        "CREATE INDEX IF NOT EXISTS sessions_cell ON sessions(cell);"
        "CREATE INDEX IF NOT EXISTS sessions_last_seen ON sessions(last_seen);"
        "CREATE INDEX IF NOT EXISTS auth_pending_cell ON auth_pending(cell,state,expires);"
        "CREATE INDEX IF NOT EXISTS auth_pending_expires ON auth_pending(expires);"
        "INSERT OR IGNORE INTO users(cell,advanced,created) "
        "SELECT DISTINCT cell,0,0 FROM redirect WHERE cell<>'';";
    if (!db_exec(db, schema)) return 0;
    if (!db_exec(db, "INSERT OR IGNORE INTO settings(name,value) VALUES('smsauth_url','" DEFAULT_SMSAUTH_URL "');"
        "INSERT OR IGNORE INTO settings(name,value) VALUES('smsauth_channel','" DEFAULT_SMSAUTH_CHANNEL "');")) return 0;
    printf("init: ok\n");
    return 1;
}

static int user_ensure(sqlite3 *db, const char *cell) {
    sqlite3_stmt *st;
    time_t now;
    int rc;

    now = time(NULL);
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO users(cell,advanced,created) VALUES(?1,0,?2)", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, cell, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

static int user_advanced(sqlite3 *db, const char *cell) {
    sqlite3_stmt *st;
    int rc, advanced;

    st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT advanced FROM users WHERE cell=?1", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, cell, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    advanced = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return advanced != 0;
}

static void runtime_cleanup(sqlite3 *db) {
    sqlite3_stmt *st;
    time_t now;

    now = time(NULL);
    st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE last_seen<?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)(now - SESSION_IDLE));
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    st = NULL;
    if (sqlite3_prepare_v2(db,
        "UPDATE auth_pending SET state=?1,reason='timeout' WHERE state=?2 AND expires<=?3", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, AUTH_KO);
        sqlite3_bind_int(st, 2, AUTH_PENDING);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM auth_pending WHERE created<?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)(now - AUTH_RETENTION));
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

static int url_decode(const char *src, size_t n, char *dst, size_t dstsz) {
    size_t i, j;
    int a, b;

    j = 0;
    for (i = 0; i < n; i++) {
        if (j + 1 >= dstsz) return 0;
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && i + 2 < n) {
            a = isdigit((unsigned char)src[i + 1]) ? src[i + 1] - '0' : tolower((unsigned char)src[i + 1]) - 'a' + 10;
            b = isdigit((unsigned char)src[i + 2]) ? src[i + 2] - '0' : tolower((unsigned char)src[i + 2]) - 'a' + 10;
            if (a < 0 || a > 15 || b < 0 || b > 15) return 0;
            dst[j++] = (char)((a << 4) | b);
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return 1;
}

static int form_get(const char *body, const char *name, char *out, size_t outsz) {
    const char *p, *eq, *amp;
    size_t name_len, key_len, value_len;

    name_len = strlen(name);
    p = body;
    for (;;) {
        if (*p == '\0') break;
        amp = strchr(p, '&');
        if (amp == NULL) amp = p + strlen(p);
        eq = memchr(p, '=', (size_t)(amp - p));
        if (eq != NULL) {
            key_len = (size_t)(eq - p);
            if (key_len == name_len && strncmp(p, name, name_len) == 0) {
                value_len = (size_t)(amp - eq - 1);
                return url_decode(eq + 1, value_len, out, outsz);
            }
        }
        if (*amp == '\0') break;
        p = amp + 1;
    }
    return 0;
}

static int json_get(const char *body, const char *name, char *out, size_t outsz) {
    char pattern[128];
    const char *p;
    size_t j;

    if (strlen(name) + 3 >= sizeof(pattern)) return 0;
    snprintf(pattern, sizeof(pattern), "\"%s\"", name);
    p = strstr(body, pattern);
    if (p == NULL) return 0;
    p += strlen(pattern);
    for (; isspace((unsigned char)*p); p++);
    if (*p++ != ':') return 0;
    for (; isspace((unsigned char)*p); p++);
    if (*p++ != '"') return 0;
    j = 0;
    for (; *p != '\0' && *p != '"'; p++) {
        if (j + 1 >= outsz) return 0;
        if (*p == '\\') {
            p++;
            if (*p == '\0') return 0;
            if (*p == 'n') out[j++] = '\n';
            else if (*p == 'r') out[j++] = '\r';
            else if (*p == 't') out[j++] = '\t';
            else if (*p == 'b') out[j++] = '\b';
            else if (*p == 'f') out[j++] = '\f';
            else if (*p == '"' || *p == '\\' || *p == '/') out[j++] = *p;
            else return 0;
        } else {
            out[j++] = *p;
        }
    }
    if (*p != '"') return 0;
    out[j] = '\0';
    return 1;
}

static int json_get_int(const char *body, const char *name, int *value) {
    char pattern[128];
    const char *p;
    char *end;
    long v;

    if (strlen(name) + 3 >= sizeof(pattern)) return 0;
    snprintf(pattern, sizeof(pattern), "\"%s\"", name);
    p = strstr(body, pattern);
    if (p == NULL) return 0;
    p += strlen(pattern);
    for (; isspace((unsigned char)*p); p++);
    if (*p++ != ':') return 0;
    for (; isspace((unsigned char)*p); p++);
    errno = 0;
    v = strtol(p, &end, 10);
    if (errno != 0 || end == p || v < 0 || v > 2147483647L) return 0;
    *value = (int)v;
    return 1;
}

static int json_escape(const char *src, char *dst, size_t dstsz) {
    size_t i, j;
    unsigned char c;

    j = 0;
    for (i = 0; src[i] != '\0'; i++) {
        c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dstsz) return 0;
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c < 32) {
            if (j + 6 >= dstsz) return 0;
            snprintf(dst + j, dstsz - j, "\\u%04x", c);
            j += 6;
        } else {
            if (j + 1 >= dstsz) return 0;
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
    return 1;
}

static int html_escape(const char *src, char *dst, size_t dstsz) {
    size_t i, j, n;
    const char *rep;

    j = 0;
    for (i = 0; src[i] != '\0'; i++) {
        rep = NULL;
        if (src[i] == '&') rep = "&amp;";
        else if (src[i] == '<') rep = "&lt;";
        else if (src[i] == '>') rep = "&gt;";
        else if (src[i] == '"') rep = "&quot;";
        else if (src[i] == '\'') rep = "&#39;";
        if (rep != NULL) {
            n = strlen(rep);
            if (j + n >= dstsz) return 0;
            memcpy(dst + j, rep, n);
            j += n;
        } else {
            if (j + 1 >= dstsz) return 0;
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return 1;
}

static int read_body(char *body, size_t body_size) {
    const char *s;
    long n;
    size_t got, total;

    s = getenv("CONTENT_LENGTH");
    if (s != NULL && *s != '\0') {
        n = strtol(s, NULL, 10);
        if (n < 0 || (size_t)n >= body_size || n > MAX_BODY) return -1;
        got = fread(body, 1, (size_t)n, stdin);
        if (got != (size_t)n) return -1;
        body[got] = '\0';
        return got > 0 ? 1 : 0;
    }
    total = 0;
    for (;;) {
        if (total + 1 >= body_size || total >= MAX_BODY) return -1;
        got = fread(body + total, 1, body_size - total - 1, stdin);
        total += got;
        if (got == 0) break;
    }
    body[total] = '\0';
    return total > 0 ? 1 : 0;
}

static int cookie_get(const char *name, char *out, size_t outsz) {
    const char *cookies, *p, *end, *eq;
    size_t name_len, n;

    cookies = getenv("HTTP_COOKIE");
    if (cookies == NULL) return 0;
    name_len = strlen(name);
    p = cookies;
    for (;;) {
        for (; *p == ' ' || *p == ';'; p++);
        if (*p == '\0') break;
        end = strchr(p, ';');
        if (end == NULL) end = p + strlen(p);
        eq = memchr(p, '=', (size_t)(end - p));
        if (eq != NULL && (size_t)(eq - p) == name_len && strncmp(p, name, name_len) == 0) {
            n = (size_t)(end - eq - 1);
            if (n + 1 > outsz) return 0;
            memcpy(out, eq + 1, n);
            out[n] = '\0';
            return 1;
        }
        if (*end == '\0') break;
        p = end + 1;
    }
    return 0;
}

static void html_headers(void) {
    printf("Content-Type: text/html; charset=utf-8\r\n");
    printf("Cache-Control: no-store\r\n");
    printf("X-Content-Type-Options: nosniff\r\n");
    printf("X-Frame-Options: DENY\r\n");
    printf("Referrer-Policy: no-referrer\r\n");
}

static void json_headers(void) {
    printf("Content-Type: application/json; charset=utf-8\r\n");
    printf("Cache-Control: no-store\r\n");
    printf("X-Content-Type-Options: nosniff\r\n");
}

static void cookie_set(const char *name, const char *value, int max_age) {
    printf("Set-Cookie: %s=%s; Path=/; Secure; HttpOnly; SameSite=Strict", name, value);
    if (max_age >= 0) printf("; Max-Age=%d", max_age);
    printf("\r\n");
}

static void cookie_clear(const char *name) {
    printf("Set-Cookie: %s=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0\r\n", name);
}


static void format_epoch(sqlite3_int64 epoch, char *out, size_t outsz) {
    time_t t;
    struct tm *tmv;

    if (epoch <= 0) {
        snprintf(out, outsz, "mai");
        return;
    }
    t = (time_t)epoch;
    tmv = localtime(&t);
    if (tmv == NULL || strftime(out, outsz, "%Y-%m-%d %H:%M", tmv) == 0) snprintf(out, outsz, "%lld", (long long)epoch);
}

static int session_get(sqlite3 *db, char *cell, size_t cellsz, char *csrf, size_t csrfsz, int *advanced) {
    sqlite3_stmt *st;
    char token[129];
    const unsigned char *v;
    time_t now;
    int rc, ok;

    if (!cookie_get(SESSION_COOKIE, token, sizeof(token))) return 0;
    if (strlen(token) != 64) return 0;
    runtime_cleanup(db);
    now = time(NULL);
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT cell,csrf FROM sessions WHERE token=?1 AND last_seen>?2", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)(now - SESSION_IDLE));
    rc = sqlite3_step(st);
    ok = 0;
    if (rc == SQLITE_ROW) {
        v = sqlite3_column_text(st, 0);
        if (v != NULL && strlen((const char *)v) + 1 <= cellsz) strcpy(cell, (const char *)v);
        else cell[0] = '\0';
        v = sqlite3_column_text(st, 1);
        if (v != NULL && strlen((const char *)v) + 1 <= csrfsz) strcpy(csrf, (const char *)v);
        else csrf[0] = '\0';
        if (cell[0] != '\0' && csrf[0] != '\0') ok = 1;
    }
    sqlite3_finalize(st);
    if (!ok) return 0;

    st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE sessions SET last_seen=?1 WHERE token=?2", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
        sqlite3_bind_text(st, 2, token, -1, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    *advanced = user_advanced(db, cell);
    return 1;
}

static int session_create(sqlite3 *db, const char *cell, char token[65], char csrf[65]) {
    sqlite3_stmt *st;
    time_t now;
    int rc, tries;

    if (!user_ensure(db, cell)) return 0;
    now = time(NULL);
    for (tries = 0; tries < 10; tries++) {
        if (!random_hex(32, token) || !random_hex(32, csrf)) return 0;
        st = NULL;
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO sessions(token,cell,csrf,created,last_seen) VALUES(?1,?2,?3,?4,?4)", -1, &st, NULL);
        if (rc != SQLITE_OK) return 0;
        sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, cell, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, csrf, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc == SQLITE_DONE) return 1;
        if (rc != SQLITE_CONSTRAINT) return 0;
    }
    return 0;
}

static void session_delete(sqlite3 *db) {
    sqlite3_stmt *st;
    char token[129];

    if (!cookie_get(SESSION_COOKIE, token, sizeof(token))) return;
    st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE token=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

static int csrf_ok(const char *body, const char *csrf) {
    char supplied[129];

    supplied[0] = '\0';
    if (!form_get(body, "csrf", supplied, sizeof(supplied))) return 0;
    return ct_equal(supplied, csrf);
}

static size_t http_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    HttpBuffer *buf;
    size_t n, need, cap;
    char *p;

    buf = (HttpBuffer *)userdata;
    n = size * nmemb;
    need = buf->size + n + 1;
    if (need > buf->capacity) {
        cap = buf->capacity == 0 ? 1024 : buf->capacity;
        for (; cap < need; cap *= 2);
        if (cap > 65536) return 0;
        p = realloc(buf->data, cap);
        if (p == NULL) return 0;
        buf->data = p;
        buf->capacity = cap;
    }
    memcpy(buf->data + buf->size, ptr, n);
    buf->size += n;
    buf->data[buf->size] = '\0';
    return n;
}

static int smsauth_start(sqlite3 *db, const char *cell,
    char challenge[33], char code[6], char number[256], int *ttl) {
    CURL *curl;
    CURLcode crc;
    struct curl_slist *headers;
    HttpBuffer response;
    char url[MAX_URL], channel[65], api_key[256];
    char echannel[130], ecell[64], ekey[520], body[1024], result[32];
    const char *cainfo;
    long status;
    int ok;

    url[0] = '\0';
    channel[0] = '\0';
    api_key[0] = '\0';
    if (!setting_get(db, "smsauth_url", url, sizeof(url))) return 0;
    if (!setting_get(db, "smsauth_channel", channel, sizeof(channel))) return 0;
    if (!setting_get(db, "smsauth_api_key", api_key, sizeof(api_key))) return 0;
    if (strncmp(url, "https://", 8) != 0 || api_key[0] == '\0') return 0;
    if (!json_escape(channel, echannel, sizeof(echannel)) ||
        !json_escape(cell, ecell, sizeof(ecell)) ||
        !json_escape(api_key, ekey, sizeof(ekey))) return 0;
    if (snprintf(body, sizeof(body),
        "{\"action\":\"start\",\"channel\":\"%s\",\"cell\":\"%s\",\"key\":\"%s\"}",
        echannel, ecell, ekey) >= (int)sizeof(body)) return 0;

    response.data = NULL;
    response.size = 0;
    response.capacity = 0;
    curl = curl_easy_init();
    if (curl == NULL) return 0;
    headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "redirect/2.04");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, HTTP_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, HTTP_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    cainfo = getenv("REDIRECT_CAINFO");
    if (cainfo != NULL && *cainfo != '\0') curl_easy_setopt(curl, CURLOPT_CAINFO, cainfo);
    crc = curl_easy_perform(curl);
    status = 0;
    if (crc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    ok = 0;
    if (crc == CURLE_OK && status == 200 && response.data != NULL) {
        result[0] = '\0';
        challenge[0] = '\0';
        code[0] = '\0';
        number[0] = '\0';
        *ttl = 0;
        if (json_get(response.data, "result", result, sizeof(result)) && strcmp(result, "PENDING") == 0 &&
            json_get(response.data, "challenge", challenge, 33) && strlen(challenge) == 32 &&
            json_get(response.data, "code", code, 6) && strlen(code) == 5 &&
            json_get(response.data, "number", number, 256) &&
            json_get_int(response.data, "expires", ttl) && *ttl >= 10 && *ttl <= 600) ok = 1;
    }
    free(response.data);
    return ok;
}

static int pending_existing(sqlite3 *db, const char *cell, char token[65], char code[6], char number[256], int *remaining) {
    sqlite3_stmt *st;
    time_t now;
    sqlite3_int64 expires;
    const unsigned char *v;
    int rc, ok;

    runtime_cleanup(db);
    now = time(NULL);
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT token,code,number,expires FROM auth_pending "
        "WHERE cell=?1 AND state=0 AND expires>?2 ORDER BY created DESC LIMIT 1", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, cell, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
    rc = sqlite3_step(st);
    ok = 0;
    if (rc == SQLITE_ROW) {
        v = sqlite3_column_text(st, 0);
        if (v != NULL && strlen((const char *)v) == 64) strcpy(token, (const char *)v);
        else token[0] = '\0';
        v = sqlite3_column_text(st, 1);
        if (v != NULL && strlen((const char *)v) == 5) strcpy(code, (const char *)v);
        else code[0] = '\0';
        v = sqlite3_column_text(st, 2);
        if (v != NULL && strlen((const char *)v) < 256) strcpy(number, (const char *)v);
        else number[0] = '\0';
        expires = sqlite3_column_int64(st, 3);
        *remaining = (int)(expires - (sqlite3_int64)now);
        if (token[0] != '\0' && code[0] != '\0' && *remaining > 0) ok = 1;
    }
    sqlite3_finalize(st);
    return ok;
}

static int pending_create(sqlite3 *db, const char *cell, const char *challenge,
    const char *code, const char *number, int ttl, char token[65]) {
    sqlite3_stmt *st;
    time_t now;
    int rc, tries;

    now = time(NULL);
    for (tries = 0; tries < 10; tries++) {
        if (!random_hex(32, token)) return 0;
        st = NULL;
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO auth_pending(token,challenge,cell,code,number,created,expires,state,reason) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,0,'')", -1, &st, NULL);
        if (rc != SQLITE_OK) return 0;
        sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, challenge, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, cell, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, code, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 5, number, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 6, (sqlite3_int64)now);
        sqlite3_bind_int64(st, 7, (sqlite3_int64)(now + ttl));
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc == SQLITE_DONE) return 1;
        if (rc != SQLITE_CONSTRAINT) return 0;
    }
    return 0;
}

static void page_begin(const char *title) {
    char etitle[256];

    if (!html_escape(title, etitle, sizeof(etitle))) strcpy(etitle, "chaos.cc");
    printf("<!doctype html><html lang=\"it\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    printf("<title>%s</title><style>", etitle);
    printf("body{font-family:Arial,sans-serif;margin:0;background:#f5f7fb;color:#1f2937}main{max-width:1180px;margin:30px auto;padding:0 18px}");
    printf(".card{background:#fff;border:1px solid #dbe3ef;border-radius:14px;padding:20px;margin-bottom:18px;box-shadow:0 8px 24px rgba(0,0,0,.05)}");
    printf("h1,h2{margin-top:0}input{box-sizing:border-box;padding:9px 10px;border:1px solid #cbd5e1;border-radius:8px}input[type=text],input[type=url]{width:100%%}");
    printf("button,.button{border:0;border-radius:8px;padding:9px 14px;background:#2563eb;color:white;cursor:pointer;text-decoration:none}.danger{background:#b91c1c}.small{font-size:12px;color:#64748b}a{color:#2563eb}.check{display:flex;gap:9px;align-items:flex-start;line-height:1.45}.check input{margin-top:3px}.legal h2{margin-top:28px;padding-top:12px;border-top:1px solid #e5e7eb}.legal li{margin-bottom:7px}");
    printf("table{width:100%%;border-collapse:collapse}th,td{padding:9px;border-bottom:1px solid #e5e7eb;text-align:left;vertical-align:top}th{white-space:nowrap}.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px}.stat{background:#eef2ff;padding:14px;border-radius:10px}.stat b{display:block;font-size:24px}.msg{padding:12px;border-radius:8px;margin-bottom:14px;background:#ecfdf5}.err{background:#fef2f2;color:#991b1b}.row{display:grid;grid-template-columns:1fr 1fr;gap:12px}.top{display:flex;justify-content:space-between;gap:12px;align-items:center}.actions{display:flex;gap:6px;flex-wrap:wrap}@media(max-width:760px){.row{grid-template-columns:1fr}table{font-size:13px}th,td{padding:6px}}</style></head><body><main>");
}

static void page_end(void) {
    printf("</main></body></html>\n");
}

static void privacy_page(void) {
    html_headers();
    printf("\r\n");
    page_begin("Privacy e condizioni - chaos.cc");
    printf("<div class=\"card legal\"><h1>Privacy e condizioni d'uso</h1><p class=\"small\">Ultimo aggiornamento: 7 settembre 2026</p>");
    printf("<p>Questa informativa descrive il trattamento dei dati personali connesso a <b>chaos.cc</b>, servizio di ridirezione con URL corti, e le condizioni essenziali per il suo utilizzo.</p>");
    printf("<h2>1. Titolare del trattamento</h2><p>Il titolare del trattamento e' <b>Gianluca Mazzini</b>, contattabile all'indirizzo <a href=\"mailto:gianluca@mazzini.org\">gianluca@mazzini.org</a>. Il servizio e' gestito a titolo personale, non commerciale e senza corrispettivo economico.</p>");
    printf("<h2>2. Servizio</h2><p>chaos.cc consente agli utenti autenticati di creare e gestire URL corti nel dominio <b>chaos.cc</b> che ridirigono verso URL di destinazione scelti dall'utente.</p>");
    printf("<h2>3. Dati trattati</h2><ul><li>numero di cellulare dell'utente, utilizzato per autenticazione e associazione dei link;</li><li>dati tecnici temporanei necessari all'autenticazione e alla sessione;</li><li>shortcut, URL di destinazione e descrizione inseriti dall'utente;</li><li>contatore aggregato dei redirect e data/ora dell'ultimo utilizzo del singolo link.</li></ul>");
    printf("<p>L'applicazione non conserva per i visitatori dei redirect indirizzo IP, user agent, referrer o la cronologia dei singoli click. I componenti di rete e hosting possono comunque trattare transitoriamente i dati tecnici necessari alla comunicazione e alla sicurezza dell'infrastruttura.</p>");
    printf("<h2>4. Finalita' e base giuridica</h2><ul><li>autenticare l'utente ed erogare il servizio richiesto e le relative condizioni d'uso, ai sensi dell'art. 6(1)(b) GDPR ove applicabile;</li><li>garantire sicurezza, prevenire abusi e mantenere il funzionamento tecnico del servizio, sulla base del legittimo interesse del titolare ai sensi dell'art. 6(1)(f) GDPR;</li><li>adempiere a eventuali obblighi di legge o richieste dell'autorita', ai sensi dell'art. 6(1)(c) GDPR.</li></ul>");
    printf("<p>Il numero di cellulare non e' utilizzato per marketing o profilazione. Non sono effettuate decisioni automatizzate o profilazione.</p>");
    printf("<h2>5. Autenticazione e cookie</h2><p>L'accesso viene verificato mediante OTP inviato dall'utente via SMS a uno dei numeri indicati dal sistema. Dopo la verifica viene creato un cookie tecnico di sessione Secure, HttpOnly e SameSite=Strict, necessario esclusivamente al funzionamento del pannello. Non sono utilizzati cookie di profilazione o analytics.</p>");
    printf("<h2>6. Conservazione</h2><ul><li>le richieste di autenticazione sono temporanee e vengono eliminate entro circa 15 minuti;</li><li>la sessione scade dopo 1 ora di inattivita';</li><li>il numero di cellulare resta associato all'utente finche' utilizza il servizio o ne richiede la cancellazione, salvo obblighi di legge;</li><li>shortcut, destinazione, descrizione, contatore aggregato e ultimo utilizzo sono conservati fino alla cancellazione del link o alla cessazione del servizio.</li></ul>");
    printf("<h2>7. Destinatari e trasferimenti</h2><p>I dati possono essere trattati dai fornitori tecnici strettamente necessari al funzionamento del servizio, inclusi hosting, infrastruttura di rete e telecomunicazioni/SMS. I dati non sono venduti. Eventuali trasferimenti fuori dallo Spazio Economico Europeo avvengono, ove presenti, nel rispetto delle garanzie previste dalla normativa applicabile.</p>");
    printf("<h2>8. Natura del conferimento</h2><p>Il numero di cellulare e' necessario per autenticarsi e gestire i redirect. Il mancato conferimento impedisce l'accesso al pannello. I dati relativi ai link sono conferiti volontariamente dall'utente per utilizzare il servizio.</p>");
    printf("<h2>9. Diritti dell'interessato</h2><p>Nei casi previsti dal GDPR, l'interessato puo' chiedere accesso, rettifica, cancellazione, limitazione, portabilita' e puo' opporsi al trattamento. Le richieste possono essere inviate a <a href=\"mailto:gianluca@mazzini.org\">gianluca@mazzini.org</a>. E' inoltre possibile proporre reclamo al Garante per la protezione dei dati personali.</p>");
    printf("<h2>10. Responsabilita' dell'utente</h2><p>L'utente che crea o modifica un redirect e' responsabile, nei limiti consentiti dalla legge, dello shortcut, dell'URL di destinazione, della descrizione e dei contenuti o servizi raggiungibili tramite il link. L'utente deve avere titolo a pubblicare e condividere tali informazioni.</p>");
    printf("<p>E' vietato utilizzare il servizio per contenuti o attivita' illecite, phishing, malware, frodi, violazioni di diritti altrui o per inserire senza adeguato titolo dati personali di terzi, credenziali, token, segreti o informazioni riservate. Se l'utente inserisce dati personali di terzi nell'URL o nella descrizione, e' sua responsabilita' disporre di una valida base giuridica per tale trattamento.</p>");
    printf("<p>Il titolare puo' sospendere o rimuovere redirect manifestamente illeciti, abusivi, pericolosi o contrari alle presenti condizioni. Il servizio e' fornito senza garanzia di disponibilita' continuativa.</p>");
    printf("<h2>11. Modifiche</h2><p>L'informativa e le condizioni possono essere aggiornate nel tempo. La versione vigente e' pubblicata su questa pagina con la relativa data di aggiornamento.</p>");
    printf("<p><a class=\"button\" href=\"/\">Torna al servizio</a></p></div>");
    page_end();
}

static void login_page(const char *message) {
    char emsg[512];

    html_headers();
    printf("\r\n");
    page_begin("chaos.cc");
    printf("<div class=\"card\"><h1>chaos.cc</h1><p><b>Servizio di ridirezione con URL corti.</b> Consente di creare e gestire shortcut personali nel dominio chaos.cc dopo autenticazione via SMS.</p>");
    if (message != NULL && *message != '\0') {
        if (!html_escape(message, emsg, sizeof(emsg))) strcpy(emsg, "Errore");
        printf("<div class=\"msg err\">%s</div>", emsg);
    }
    printf("<form method=\"post\"><input type=\"hidden\" name=\"action\" value=\"login\"><label>Numero di cellulare</label><br><br><input type=\"text\" name=\"cell\" inputmode=\"numeric\" required><br><br>");
    printf("<label class=\"check\"><input type=\"checkbox\" name=\"notice\" value=\"1\" required><span>Ho letto l'<a href=\"/?privacy=1\" target=\"_blank\" rel=\"noopener\">informativa privacy</a> e accetto le condizioni d'uso, inclusa la responsabilita' per i link inseriti.</span></label><br>");
    printf("<button type=\"submit\">Autentica</button></form></div>");
    page_end();
}

static void wait_page(const char *token, const char *cell, const char *code, const char *number, int ttl) {
    char ecell[64], ecode[32], enumber[512];

    html_escape(cell, ecell, sizeof(ecell));
    html_escape(code, ecode, sizeof(ecode));
    html_escape(number, enumber, sizeof(enumber));
    html_headers();
    cookie_set(LOGIN_COOKIE, token, ttl + 30);
    printf("\r\n");
    page_begin("Autenticazione SMS");
    printf("<div class=\"card\"><h1>Autenticazione SMS</h1><div id=\"state\">Utente <b>%s</b><br><br>Invia via SMS il codice <b style=\"font-size:28px\">%s</b>", ecell, ecode);
    if (number[0] != '\0') printf(strchr(number, ',') != NULL ? " a uno dei numeri <b>%s</b>" : " al numero <b>%s</b>", enumber);
    printf(" entro <b id=\"count\">%d</b> secondi.</div></div>", ttl);
    printf("<script>let n=%d;const c=document.getElementById('count');const s=document.getElementById('state');", ttl);
    printf("async function poll(){try{const r=await fetch('/',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action=auth_check',cache:'no-store'});const j=await r.json();if(j.result==='OK'){location='/';return;}if(j.result==='KO'){s.innerHTML='<b>Autenticazione fallita.</b>';return;}}catch(e){}if(n>0)setTimeout(poll,1000);}setInterval(()=>{if(n>0){n--;c.textContent=n;}},1000);setTimeout(poll,700);</script>");
    page_end();
}

static int callback_handle(sqlite3 *db, const char *body) {
    sqlite3_stmt *st;
    const char *signature;
    char secret[256], expected[65], challenge[64], channel[65], configured[65];
    char result[32], cell_raw[64], cell[32], reason[128];
    int rc, state;

    signature = getenv("HTTP_X_SMSAUTH_SIGNATURE");
    if (signature == NULL || strlen(signature) != 64) return 0;
    secret[0] = '\0';
    configured[0] = '\0';
    if (!setting_get(db, "smsauth_callback_secret", secret, sizeof(secret)) || secret[0] == '\0') {
        printf("Status: 403 Forbidden\r\nContent-Type: text/plain\r\n\r\nForbidden\n");
        return 1;
    }
    if (!hmac_hex(secret, body, expected) || !ct_equal(signature, expected)) {
        printf("Status: 403 Forbidden\r\nContent-Type: text/plain\r\n\r\nForbidden\n");
        return 1;
    }
    challenge[0] = '\0';
    channel[0] = '\0';
    result[0] = '\0';
    cell_raw[0] = '\0';
    reason[0] = '\0';
    setting_get(db, "smsauth_channel", configured, sizeof(configured));
    if (!json_get(body, "challenge", challenge, sizeof(challenge)) || strlen(challenge) != 32 ||
        !json_get(body, "channel", channel, sizeof(channel)) || strcmp(channel, configured) != 0 ||
        !json_get(body, "result", result, sizeof(result)) ||
        !json_get(body, "cell", cell_raw, sizeof(cell_raw)) || !normalize_cell(cell_raw, cell, sizeof(cell))) {
        printf("Status: 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nBad Request\n");
        return 1;
    }
    json_get(body, "reason", reason, sizeof(reason));
    state = strcmp(result, "OK") == 0 ? AUTH_OK : AUTH_KO;
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "UPDATE auth_pending SET state=?1,reason=?2 WHERE challenge=?3 AND cell=?4 AND state=0", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        printf("Status: 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nDatabase Error\n");
        return 1;
    }
    sqlite3_bind_int(st, 1, state);
    sqlite3_bind_text(st, 2, reason, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, challenge, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, cell, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        printf("Status: 500 Internal Server Error\r\nContent-Type: text/plain\r\n\r\nDatabase Error\n");
        return 1;
    }
    printf("Status: 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\n\r\nOK\n");
    return 1;
}

static void auth_check(sqlite3 *db) {
    sqlite3_stmt *st;
    char login_token[129], session_token[65], csrf[65], cell[32], reason[128], ereason[300];
    time_t now;
    sqlite3_int64 expires;
    int rc, state;

    if (!cookie_get(LOGIN_COOKIE, login_token, sizeof(login_token)) || strlen(login_token) != 64) {
        json_headers();
        printf("\r\n{\"result\":\"KO\",\"reason\":\"login_missing\"}\n");
        return;
    }
    runtime_cleanup(db);
    now = time(NULL);
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT cell,expires,state,reason FROM auth_pending WHERE token=?1", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        json_headers();
        printf("\r\n{\"result\":\"KO\",\"reason\":\"database_error\"}\n");
        return;
    }
    sqlite3_bind_text(st, 1, login_token, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        json_headers();
        cookie_clear(LOGIN_COOKIE);
        printf("\r\n{\"result\":\"KO\",\"reason\":\"not_found\"}\n");
        return;
    }
    strncpy(cell, (const char *)sqlite3_column_text(st, 0), sizeof(cell) - 1);
    cell[sizeof(cell) - 1] = '\0';
    expires = sqlite3_column_int64(st, 1);
    state = sqlite3_column_int(st, 2);
    reason[0] = '\0';
    if (sqlite3_column_text(st, 3) != NULL) {
        strncpy(reason, (const char *)sqlite3_column_text(st, 3), sizeof(reason) - 1);
        reason[sizeof(reason) - 1] = '\0';
    }
    sqlite3_finalize(st);

    if (state == AUTH_PENDING && expires <= (sqlite3_int64)now) state = AUTH_KO;
    if (state == AUTH_PENDING) {
        json_headers();
        printf("\r\n{\"result\":\"PENDING\"}\n");
        return;
    }
    if (state == AUTH_KO) {
        if (reason[0] == '\0') strcpy(reason, "authentication_failed");
        if (!json_escape(reason, ereason, sizeof(ereason))) strcpy(ereason, "authentication_failed");
        json_headers();
        cookie_clear(LOGIN_COOKIE);
        printf("\r\n{\"result\":\"KO\",\"reason\":\"%s\"}\n", ereason);
        return;
    }
    if (!session_create(db, cell, session_token, csrf)) {
        json_headers();
        printf("\r\n{\"result\":\"KO\",\"reason\":\"session_error\"}\n");
        return;
    }
    st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM auth_pending WHERE token=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, login_token, -1, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    json_headers();
    cookie_set(SESSION_COOKIE, session_token, -1);
    cookie_clear(LOGIN_COOKIE);
    printf("\r\n{\"result\":\"OK\"}\n");
}

static int portal_add(sqlite3 *db, const char *body, const char *cell, int advanced, char *message, size_t msgsz) {
    sqlite3_stmt *st;
    char destination[MAX_URL], description[1024], shortcut[128], origin[256], random_code[SHORTCUT_LEN + 1];
    time_t now;
    int rc, tries;

    destination[0] = '\0';
    description[0] = '\0';
    shortcut[0] = '\0';
    if (!form_get(body, "destination", destination, sizeof(destination)) || !valid_web_url(destination)) {
        snprintf(message, msgsz, "Destination non valida");
        return 0;
    }
    form_get(body, "description", description, sizeof(description));
    form_get(body, "shortcut", shortcut, sizeof(shortcut));
    now = time(NULL);

    if (shortcut[0] != '\0') {
        if (!advanced) {
            snprintf(message, msgsz, "La scelta dello shortcut richiede la modalita advanced");
            return 0;
        }
        if (!make_origin(shortcut, origin, sizeof(origin))) {
            snprintf(message, msgsz, "Shortcut non valido");
            return 0;
        }
        tries = 1;
    } else {
        tries = 20;
    }

    for (; tries > 0; tries--) {
        if (shortcut[0] == '\0') {
            if (!random_shortcut(random_code) || !make_origin(random_code, origin, sizeof(origin))) {
                snprintf(message, msgsz, "Errore generazione shortcut");
                return 0;
            }
        }
        st = NULL;
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO redirect(origin,destination,hit,description,cell,last_hit,created,updated) "
            "VALUES(?1,?2,0,?3,?4,0,?5,?5)", -1, &st, NULL);
        if (rc != SQLITE_OK) {
            snprintf(message, msgsz, "Errore database");
            return 0;
        }
        sqlite3_bind_text(st, 1, origin, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, destination, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, description, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, cell, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc == SQLITE_DONE) {
            snprintf(message, msgsz, "Creato: %s", origin);
            return 1;
        }
        if (rc != SQLITE_CONSTRAINT) {
            snprintf(message, msgsz, "Errore database");
            return 0;
        }
        if (shortcut[0] != '\0') {
            snprintf(message, msgsz, "Shortcut gia utilizzato");
            return 0;
        }
    }
    snprintf(message, msgsz, "Impossibile generare uno shortcut libero");
    return 0;
}

static int portal_update(sqlite3 *db, const char *body, const char *cell, char *message, size_t msgsz) {
    sqlite3_stmt *st;
    char origin[256], destination[MAX_URL], description[1024];
    time_t now;
    int rc;

    origin[0] = '\0';
    destination[0] = '\0';
    description[0] = '\0';
    if (!form_get(body, "origin", origin, sizeof(origin)) ||
        !form_get(body, "destination", destination, sizeof(destination)) || !valid_web_url(destination)) {
        snprintf(message, msgsz, "Dati non validi");
        return 0;
    }
    form_get(body, "description", description, sizeof(description));
    now = time(NULL);
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "UPDATE redirect SET destination=?1,description=?2,updated=?3 WHERE origin=?4 COLLATE NOCASE AND cell=?5", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, destination, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, description, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
    sqlite3_bind_text(st, 4, origin, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 5, cell, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE && sqlite3_changes(db) == 1) {
        snprintf(message, msgsz, "Modificato");
        return 1;
    }
    snprintf(message, msgsz, "Link non trovato");
    return 0;
}

static int portal_delete(sqlite3 *db, const char *body, const char *cell, char *message, size_t msgsz) {
    sqlite3_stmt *st;
    char origin[256];
    int rc;

    origin[0] = '\0';
    if (!form_get(body, "origin", origin, sizeof(origin))) return 0;
    st = NULL;
    rc = sqlite3_prepare_v2(db, "DELETE FROM redirect WHERE origin=?1 COLLATE NOCASE AND cell=?2", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, origin, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, cell, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE && sqlite3_changes(db) == 1) {
        snprintf(message, msgsz, "Cancellato");
        return 1;
    }
    snprintf(message, msgsz, "Link non trovato");
    return 0;
}

static void dashboard(sqlite3 *db, const char *cell, const char *csrf, int advanced, const char *message, int message_ok) {
    sqlite3_stmt *st;
    char ecell[64], ecsrf[160], emsg[1024], eorigin[600], edest[MAX_URL * 6 + 1], edesc[6145], etime[64];
    sqlite3_int64 links, hits, active7, last;
    time_t now;
    int rc, rowid;

    links = 0;
    hits = 0;
    active7 = 0;
    last = 0;
    now = time(NULL);
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*),COALESCE(SUM(hit),0),COALESCE(SUM(CASE WHEN last_hit>=?2 THEN 1 ELSE 0 END),0),COALESCE(MAX(last_hit),0) "
        "FROM redirect WHERE cell=?1", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, cell, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)(now - 604800));
        if (sqlite3_step(st) == SQLITE_ROW) {
            links = sqlite3_column_int64(st, 0);
            hits = sqlite3_column_int64(st, 1);
            active7 = sqlite3_column_int64(st, 2);
            last = sqlite3_column_int64(st, 3);
        }
    }
    sqlite3_finalize(st);
    html_escape(cell, ecell, sizeof(ecell));
    html_escape(csrf, ecsrf, sizeof(ecsrf));
    html_headers();
    printf("\r\n");
    page_begin("chaos.cc");
    printf("<div class=\"top\"><div><h1>chaos.cc</h1><div>%s %s</div></div><form method=\"post\"><input type=\"hidden\" name=\"action\" value=\"logout\"><input type=\"hidden\" name=\"csrf\" value=\"%s\"><button type=\"submit\">Esci</button></form></div>", ecell, advanced ? "<b>advanced</b>" : "generic", ecsrf);
    if (message != NULL && *message != '\0') {
        html_escape(message, emsg, sizeof(emsg));
        printf("<div class=\"msg%s\">%s</div>", message_ok ? "" : " err", emsg);
    }
    format_epoch(last, etime, sizeof(etime));
    printf("<div class=\"card stats\"><div class=\"stat\"><span>Shortcut</span><b>%lld</b></div><div class=\"stat\"><span>Hit totali</span><b>%lld</b></div><div class=\"stat\"><span>Usati negli ultimi 7 giorni</span><b>%lld</b></div><div class=\"stat\"><span>Ultimo utilizzo</span><b style=\"font-size:16px\">%s</b></div></div>", (long long)links, (long long)hits, (long long)active7, etime);

    printf("<div class=\"card\"><h2>Nuovo shortcut</h2><form method=\"post\"><input type=\"hidden\" name=\"action\" value=\"add\"><input type=\"hidden\" name=\"csrf\" value=\"%s\"><div class=\"row\">", ecsrf);
    if (advanced) printf("<div><label>Shortcut opzionale</label><input type=\"text\" name=\"shortcut\" placeholder=\"nome\"><div class=\"small\">Se vuoto viene generato casualmente. Suffisso: .chaos.cc</div></div>");
    printf("<div><label>Destination</label><input type=\"url\" name=\"destination\" required placeholder=\"https://...\"></div><div><label>Description</label><input type=\"text\" name=\"description\"></div></div><br><button type=\"submit\">Crea</button></form></div>");

    printf("<div class=\"card\"><h2>I tuoi link</h2><div style=\"overflow:auto\"><table><thead><tr><th>Shortcut</th><th>Destination</th><th>Description</th><th>Hit</th><th>Ultimo hit</th><th>Azioni</th></tr></thead><tbody>");
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT origin,destination,description,hit,last_hit FROM redirect WHERE cell=?1 ORDER BY origin COLLATE NOCASE", -1, &st, NULL);
    if (rc == SQLITE_OK) sqlite3_bind_text(st, 1, cell, -1, SQLITE_STATIC);
    rowid = 0;
    for (;;) {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) break;
        html_escape((const char *)sqlite3_column_text(st, 0), eorigin, sizeof(eorigin));
        html_escape((const char *)sqlite3_column_text(st, 1), edest, sizeof(edest));
        html_escape((const char *)sqlite3_column_text(st, 2), edesc, sizeof(edesc));
        format_epoch(sqlite3_column_int64(st, 4), etime, sizeof(etime));
        printf("<tr><td><a href=\"https://%s\" target=\"_blank\">%s</a></td>", eorigin, eorigin);
        printf("<td><input form=\"u%d\" type=\"url\" name=\"destination\" value=\"%s\" required></td>", rowid, edest);
        printf("<td><input form=\"u%d\" type=\"text\" name=\"description\" value=\"%s\"></td>", rowid, edesc);
        printf("<td>%d</td><td>%s</td><td><div class=\"actions\">", sqlite3_column_int(st, 3), etime);
        printf("<form id=\"u%d\" method=\"post\"><input type=\"hidden\" name=\"action\" value=\"update\"><input type=\"hidden\" name=\"csrf\" value=\"%s\"><input type=\"hidden\" name=\"origin\" value=\"%s\"><button type=\"submit\">Salva</button></form>", rowid, ecsrf, eorigin);
        printf("<form method=\"post\" onsubmit=\"return confirm('Cancellare?')\"><input type=\"hidden\" name=\"action\" value=\"delete\"><input type=\"hidden\" name=\"csrf\" value=\"%s\"><input type=\"hidden\" name=\"origin\" value=\"%s\"><button class=\"danger\" type=\"submit\">Cancella</button></form></div></td></tr>", ecsrf, eorigin);
        rowid++;
    }
    sqlite3_finalize(st);
    printf("</tbody></table></div></div>");
    printf("<p class=\"small\"><a href=\"/?privacy=1\" target=\"_blank\" rel=\"noopener\">Privacy e condizioni d'uso</a></p>");
    page_end();
}

static int portal_web(sqlite3 *db) {
    char body[MAX_BODY + 1], action[64], cell_raw[64], cell[32], csrf[65], notice[8];
    char challenge[33], code[6], number[256], login_token[65], message[512];
    const char *method, *signature, *query;
    int body_rc, ttl, advanced, authed, ok;

    method = getenv("REQUEST_METHOD");
    if (method == NULL) method = "GET";
    query = getenv("QUERY_STRING");
    if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) && query != NULL &&
        strstr(query, "privacy=1") != NULL) {
        privacy_page();
        return 1;
    }
    body[0] = '\0';
    body_rc = 0;
    if (strcmp(method, "POST") == 0) body_rc = read_body(body, sizeof(body));
    signature = getenv("HTTP_X_SMSAUTH_SIGNATURE");
    if (strcmp(method, "POST") == 0 && signature != NULL && *signature != '\0') {
        if (body_rc <= 0) {
            printf("Status: 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nBad Request\n");
            return 1;
        }
        return callback_handle(db, body);
    }
    if (strcmp(method, "POST") == 0 && body_rc < 0) {
        printf("Status: 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nBad Request\n");
        return 1;
    }

    action[0] = '\0';
    if (body_rc > 0) form_get(body, "action", action, sizeof(action));
    if (strcmp(action, "auth_check") == 0) {
        auth_check(db);
        return 1;
    }

    cell[0] = '\0';
    csrf[0] = '\0';
    advanced = 0;
    authed = session_get(db, cell, sizeof(cell), csrf, sizeof(csrf), &advanced);

    if (!authed && strcmp(action, "login") == 0) {
        notice[0] = '\0';
        if (!form_get(body, "notice", notice, sizeof(notice)) || strcmp(notice, "1") != 0) {
            login_page("Devi prendere visione dell'informativa privacy e accettare le condizioni d'uso");
            return 1;
        }
        cell_raw[0] = '\0';
        if (!form_get(body, "cell", cell_raw, sizeof(cell_raw)) || !normalize_cell(cell_raw, cell, sizeof(cell))) {
            login_page("Numero non valido");
            return 1;
        }
        ttl = 0;
        login_token[0] = '\0';
        code[0] = '\0';
        number[0] = '\0';
        if (pending_existing(db, cell, login_token, code, number, &ttl)) {
            wait_page(login_token, cell, code, number, ttl);
            return 1;
        }
        challenge[0] = '\0';
        if (!smsauth_start(db, cell, challenge, code, number, &ttl)) {
            login_page("Servizio SMS temporaneamente non disponibile o non configurato");
            return 1;
        }
        if (!pending_create(db, cell, challenge, code, number, ttl, login_token)) {
            login_page("Errore creazione autenticazione");
            return 1;
        }
        wait_page(login_token, cell, code, number, ttl);
        return 1;
    }

    if (!authed) {
        login_page("");
        return 1;
    }

    if (strcmp(action, "logout") == 0) {
        if (!csrf_ok(body, csrf)) {
            dashboard(db, cell, csrf, advanced, "Richiesta non valida", 0);
            return 1;
        }
        session_delete(db);
        printf("Status: 303 See Other\r\nLocation: /\r\nCache-Control: no-store\r\n");
        cookie_clear(SESSION_COOKIE);
        printf("\r\n");
        return 1;
    }

    message[0] = '\0';
    ok = 1;
    if (strcmp(method, "POST") == 0 && action[0] != '\0') {
        if (!csrf_ok(body, csrf)) {
            snprintf(message, sizeof(message), "Richiesta non valida");
            ok = 0;
        } else if (strcmp(action, "add") == 0) {
            ok = portal_add(db, body, cell, advanced, message, sizeof(message));
        } else if (strcmp(action, "update") == 0) {
            ok = portal_update(db, body, cell, message, sizeof(message));
        } else if (strcmp(action, "delete") == 0) {
            ok = portal_delete(db, body, cell, message, sizeof(message));
        }
    }
    dashboard(db, cell, csrf, advanced, message, ok);
    return 1;
}

static int is_prefetch(void) {
    const char *purpose;

    purpose = getenv("HTTP_SEC_PURPOSE");
    return purpose != NULL && strcasestr(purpose, "prefetch") != NULL;
}

static int redirect_web(sqlite3 *db, const char *host) {
    sqlite3_stmt *st;
    char url[MAX_URL];
    time_t now;
    int rc, found;

    strcpy(url, FALLBACK_URL);
    found = 0;
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT destination FROM redirect WHERE origin=?1 COLLATE NOCASE LIMIT 1", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, host, -1, SQLITE_STATIC);
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW && sqlite3_column_text(st, 0) != NULL &&
            strlen((const char *)sqlite3_column_text(st, 0)) < sizeof(url)) {
            strcpy(url, (const char *)sqlite3_column_text(st, 0));
            found = valid_header_value(url);
            if (!found) strcpy(url, FALLBACK_URL);
        }
    }
    sqlite3_finalize(st);

    if (found && !is_prefetch()) {
        now = time(NULL);
        st = NULL;
        rc = sqlite3_prepare_v2(db,
            "UPDATE redirect SET hit=hit+1,last_hit=?1 WHERE origin=?2 COLLATE NOCASE", -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
            sqlite3_bind_text(st, 2, host, -1, SQLITE_STATIC);
            if (sqlite3_step(st) != SQLITE_DONE) fprintf(stderr, "redirect hit update: %s\n", sqlite3_errmsg(db));
        }
        sqlite3_finalize(st);
    }

    printf("Status: 302 Found\r\nLocation: %s\r\nCache-Control: no-store\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n", url);
    return 1;
}

static int web(sqlite3 *db) {
    const char *raw;
    char host[256];
    const char *colon;
    size_t n;

    raw = getenv("HTTP_HOST");
    if (raw == NULL || *raw == '\0') return redirect_web(db, "");
    colon = strchr(raw, ':');
    n = colon != NULL ? (size_t)(colon - raw) : strlen(raw);
    if (n == 0 || n >= sizeof(host)) return redirect_web(db, "");
    memcpy(host, raw, n);
    host[n] = '\0';
    if (strcasecmp(host, portal_host()) == 0) return portal_web(db);
    return redirect_web(db, host);
}

static void print_row(sqlite3_stmt *st) {
    char last[64];

    format_epoch(sqlite3_column_int64(st, 5), last, sizeof(last));
    printf("%-28s %8d  %-12s  %-16s  %s\n",
        sqlite3_column_text(st, 0), sqlite3_column_int(st, 2), sqlite3_column_text(st, 4), last, sqlite3_column_text(st, 3));
    printf("  %s\n", sqlite3_column_text(st, 1));
}

static int run_rows(sqlite3 *db, const char *sql, const char *value) {
    sqlite3_stmt *st;
    int rc;

    st = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    if (value != NULL) sqlite3_bind_text(st, 1, value, -1, SQLITE_STATIC);
    printf("ORIGIN                            HIT  CELL          LAST HIT          DESCRIPTION\n");
    for (;;) {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) break;
        print_row(st);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

static int cli_list(sqlite3 *db) {
    return run_rows(db,
        "SELECT origin,destination,hit,description,cell,last_hit FROM redirect ORDER BY origin COLLATE NOCASE", NULL);
}

static int cli_show(sqlite3 *db, const char *origin) {
    return run_rows(db,
        "SELECT origin,destination,hit,description,cell,last_hit FROM redirect WHERE origin=?1 COLLATE NOCASE", origin);
}

static int cli_search(sqlite3 *db, const char *text) {
    sqlite3_stmt *st;
    char pattern[SEARCH_SIZE];
    int rc;

    if (snprintf(pattern, sizeof(pattern), "%%%s%%", text) >= (int)sizeof(pattern)) return 0;
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT origin,destination,hit,description,cell,last_hit FROM redirect "
        "WHERE origin LIKE ?1 COLLATE NOCASE OR destination LIKE ?1 COLLATE NOCASE "
        "OR description LIKE ?1 COLLATE NOCASE OR cell LIKE ?1 COLLATE NOCASE ORDER BY origin COLLATE NOCASE", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, pattern, -1, SQLITE_STATIC);
    printf("ORIGIN                            HIT  CELL          LAST HIT          DESCRIPTION\n");
    for (;;) {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) break;
        print_row(st);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

static int cli_stats(sqlite3 *db, int limit) {
    sqlite3_stmt *st;
    int rc;

    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT origin,destination,hit,description,cell,last_hit FROM redirect "
        "ORDER BY hit DESC,origin COLLATE NOCASE LIMIT ?1", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, limit);
    printf("ORIGIN                            HIT  CELL          LAST HIT          DESCRIPTION\n");
    for (;;) {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) break;
        print_row(st);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

static int cli_add(sqlite3 *db, int argc, char **argv) {
    sqlite3_stmt *st;
    const char *description, *cell;
    time_t now;
    int rc;

    if (argc < 4 || argc > 6 || !valid_header_value(argv[3])) return 0;
    description = argc >= 5 ? argv[4] : "";
    cell = argc >= 6 ? argv[5] : "";
    now = time(NULL);
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO redirect(origin,destination,hit,description,cell,last_hit,created,updated) "
        "VALUES(?1,?2,0,?3,?4,0,?5,?5)", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, argv[2], -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, argv[3], -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, description, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, cell, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "add: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    printf("add: ok\n");
    return 1;
}

static int cli_edit(sqlite3 *db, const char *origin, const char *field, const char *value) {
    sqlite3_stmt *st;
    const char *sql;
    time_t now;
    int rc, n;

    now = time(NULL);
    if (strcmp(field, "origin") == 0) sql = "UPDATE redirect SET origin=?1,updated=?3 WHERE origin=?2 COLLATE NOCASE";
    else if (strcmp(field, "destination") == 0) {
        if (!valid_header_value(value)) return 0;
        sql = "UPDATE redirect SET destination=?1,updated=?3 WHERE origin=?2 COLLATE NOCASE";
    } else if (strcmp(field, "description") == 0) sql = "UPDATE redirect SET description=?1,updated=?3 WHERE origin=?2 COLLATE NOCASE";
    else if (strcmp(field, "cell") == 0) sql = "UPDATE redirect SET cell=?1,updated=?3 WHERE origin=?2 COLLATE NOCASE";
    else if (strcmp(field, "hit") == 0) {
        if (!parse_uint(value, &n)) return 0;
        sql = "UPDATE redirect SET hit=?1,updated=?3 WHERE origin=?2 COLLATE NOCASE";
    } else return 0;

    st = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    if (strcmp(field, "hit") == 0) sqlite3_bind_int(st, 1, n);
    else sqlite3_bind_text(st, 1, value, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, origin, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE && sqlite3_changes(db) == 1) {
        if (strcmp(field, "cell") == 0 && value[0] != '\0') user_ensure(db, value);
        printf("edit: ok\n");
        return 1;
    }
    if (rc != SQLITE_DONE) fprintf(stderr, "edit: %s\n", sqlite3_errmsg(db));
    else fprintf(stderr, "edit: origin not found\n");
    return 0;
}

static int cli_delete(sqlite3 *db, const char *origin) {
    sqlite3_stmt *st;
    int rc;

    st = NULL;
    rc = sqlite3_prepare_v2(db, "DELETE FROM redirect WHERE origin=?1 COLLATE NOCASE", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, origin, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE && sqlite3_changes(db) == 1) {
        printf("delete: ok\n");
        return 1;
    }
    if (rc != SQLITE_DONE) fprintf(stderr, "delete: %s\n", sqlite3_errmsg(db));
    else fprintf(stderr, "delete: origin not found\n");
    return 0;
}

static int cli_user(sqlite3 *db, int argc, char **argv) {
    sqlite3_stmt *st;
    char cell[32];
    int rc, advanced;

    if (argc == 3 && strcmp(argv[2], "list") == 0) {
        st = NULL;
        rc = sqlite3_prepare_v2(db, "SELECT cell,advanced,created FROM users ORDER BY cell", -1, &st, NULL);
        if (rc != SQLITE_OK) return 0;
        printf("CELL             MODE\n");
        for (;;) {
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) break;
            printf("%-16s %s\n", sqlite3_column_text(st, 0), sqlite3_column_int(st, 1) ? "advanced" : "generic");
        }
        sqlite3_finalize(st);
        return rc == SQLITE_DONE;
    }
    if (argc != 4 || (strcmp(argv[2], "advanced") != 0 && strcmp(argv[2], "generic") != 0)) return 0;
    if (!normalize_cell(argv[3], cell, sizeof(cell))) return 0;
    if (!user_ensure(db, cell)) return 0;
    advanced = strcmp(argv[2], "advanced") == 0;
    st = NULL;
    rc = sqlite3_prepare_v2(db, "UPDATE users SET advanced=?1 WHERE cell=?2", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, advanced);
    sqlite3_bind_text(st, 2, cell, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return 0;
    printf("user %s: %s\n", cell, advanced ? "advanced" : "generic");
    return 1;
}

static int cli_smsauth(sqlite3 *db, int argc, char **argv) {
    char url[MAX_URL], channel[65], key[256], secret[256];

    if (argc == 3 && strcmp(argv[2], "show") == 0) {
        url[0] = '\0';
        channel[0] = '\0';
        key[0] = '\0';
        secret[0] = '\0';
        setting_get(db, "smsauth_url", url, sizeof(url));
        setting_get(db, "smsauth_channel", channel, sizeof(channel));
        setting_get(db, "smsauth_api_key", key, sizeof(key));
        setting_get(db, "smsauth_callback_secret", secret, sizeof(secret));
        printf("url %s\nchannel %s\napi_key %s\ncallback_secret %s\n",
            url, channel, key[0] != '\0' ? "configured" : "missing", secret[0] != '\0' ? "configured" : "missing");
        return 1;
    }
    if (argc != 7 || strcmp(argv[2], "set") != 0) return 0;
    if (strncmp(argv[3], "https://", 8) != 0 || argv[4][0] == '\0' || argv[5][0] == '\0' || argv[6][0] == '\0') return 0;
    if (!setting_set(db, "smsauth_url", argv[3]) ||
        !setting_set(db, "smsauth_channel", argv[4]) ||
        !setting_set(db, "smsauth_api_key", argv[5]) ||
        !setting_set(db, "smsauth_callback_secret", argv[6])) return 0;
    printf("smsauth: ok\n");
    return 1;
}

static void usage(const char *name) {
    fprintf(stderr,
        "usage:\n"
        "  %s init\n"
        "  %s list\n"
        "  %s show origin\n"
        "  %s search text\n"
        "  %s stats [limit]\n"
        "  %s add origin destination [description] [cell]\n"
        "  %s edit origin field value\n"
        "  %s delete origin\n"
        "  %s user list\n"
        "  %s user advanced cell\n"
        "  %s user generic cell\n"
        "  %s smsauth show\n"
        "  %s smsauth set url channel api_key callback_secret\n",
        name,name,name,name,name,name,name,name,name,name,name,name,name);
}

static int cli(sqlite3 *db, int argc, char **argv) {
    int limit;

    if (argc == 2 && strcmp(argv[1], "init") == 0) return init_db(db);
    if (argc == 2 && strcmp(argv[1], "list") == 0) return cli_list(db);
    if (argc == 3 && strcmp(argv[1], "show") == 0) return cli_show(db, argv[2]);
    if (argc == 3 && strcmp(argv[1], "search") == 0) return cli_search(db, argv[2]);
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "stats") == 0) {
        limit = DEFAULT_STATS;
        if (argc == 3 && (!parse_uint(argv[2], &limit) || limit == 0)) return 0;
        return cli_stats(db, limit);
    }
    if (argc >= 4 && strcmp(argv[1], "add") == 0) return cli_add(db, argc, argv);
    if (argc == 5 && strcmp(argv[1], "edit") == 0) return cli_edit(db, argv[2], argv[3], argv[4]);
    if (argc == 3 && strcmp(argv[1], "delete") == 0) return cli_delete(db, argv[2]);
    if (argc >= 3 && strcmp(argv[1], "user") == 0) return cli_user(db, argc, argv);
    if (argc >= 3 && strcmp(argv[1], "smsauth") == 0) return cli_smsauth(db, argc, argv);
    return 0;
}

int main(int argc, char **argv) {
    sqlite3 *db;
    const char *gateway;
    int rc, ok, webmode, flags;

    gateway = getenv("GATEWAY_INTERFACE");
    webmode = gateway != NULL && *gateway != '\0';
    flags = SQLITE_OPEN_READWRITE;
    if (!webmode && argc == 2 && strcmp(argv[1], "init") == 0) flags |= SQLITE_OPEN_CREATE;
    rc = sqlite3_open_v2(db_path(), &db, flags, NULL);
    if (rc != SQLITE_OK) {
        if (webmode) printf("Status: 302 Found\r\nLocation: %s\r\nContent-Type: text/plain\r\n\r\n", FALLBACK_URL);
        else fprintf(stderr, "database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 2;
    }
    sqlite3_busy_timeout(db, 3000);
    if (!set_persist(db) || !db_exec(db, "PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;")) {
        if (!webmode) fprintf(stderr, "database: cannot configure SQLite\n");
        sqlite3_close(db);
        return 3;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (webmode) ok = web(db);
    else {
        ok = cli(db, argc, argv);
        if (!ok) usage(argv[0]);
    }
    curl_global_cleanup();
    sqlite3_close(db);
    return ok ? 0 : 1;
}
