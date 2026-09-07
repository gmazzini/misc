// Gianluca Mazzini @2026- Version 1.10

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sqlite3.h>

#define DB_PATH "/home/tools/mcp/work/misc/smsauth.db"
#define MAX_BODY 8192
#define MAX_VALUE 1024
#define DEFAULT_TTL 90
#define STATUS_MIN_MS 100
#define CALLBACK_TIMEOUT_MS 5000L
#define RETENTION_SECONDS 604800

#define STATE_PENDING 0
#define STATE_OK 1
#define STATE_WRONG 2
#define STATE_TIMEOUT 3

static const char *db_path(void) {
    const char *p;

    p = getenv("SMSAUTH_DB");
    if (p != NULL && *p != '\0') return p;
    return DB_PATH;
}

static long long now_ms(void) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
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

static int random_code(char out[6]) {
    uint32_t v, limit;

    limit = UINT32_MAX - (UINT32_MAX % 100000U);
    do {
        if (RAND_bytes((unsigned char *)&v, sizeof(v)) != 1) return 0;
    } while (v >= limit);
    snprintf(out, 6, "%05u", (unsigned int)(v % 100000U));
    return 1;
}

static void sha256_hex(const char *s, char out[65]) {
    unsigned char digest[SHA256_DIGEST_LENGTH];

    SHA256((const unsigned char *)s, strlen(s), digest);
    hex_encode(digest, sizeof(digest), out);
}

static int valid_channel_name(const char *s) {
    size_t i, n;

    if (s == NULL) return 0;
    n = strlen(s);
    if (n < 1 || n > 64) return 0;
    for (i = 0; i < n; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-' && s[i] != '.') return 0;
    }
    return 1;
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

static int normalize_number_list(const char *in, char *out, size_t outsz) {
    size_t i, j, digits;
    int count;

    if (in == NULL || outsz < 2) return 0;
    i = 0;
    j = 0;
    count = 0;
    for (;;) {
        while (isspace((unsigned char)in[i])) i++;
        if (in[i] == '\0') break;
        if (count > 0) {
            if (j + 1 >= outsz) return 0;
            out[j++] = ',';
        }
        if (in[i] == '+') {
            if (j + 1 >= outsz) return 0;
            out[j++] = in[i++];
        }
        digits = 0;
        while (isdigit((unsigned char)in[i])) {
            if (j + 1 >= outsz) return 0;
            out[j++] = in[i++];
            digits++;
        }
        if (digits < 7 || digits > 15) return 0;
        count++;
        while (isspace((unsigned char)in[i])) i++;
        if (in[i] == '\0') break;
        if (in[i] != ',') return 0;
        i++;
    }
    if (count == 0) return 0;
    out[j] = '\0';
    return 1;
}

static int extract_code(const char *text, char out[6]) {
    int i;

    if (text == NULL) return 0;
    for (i = 0; i < 5; i++) {
        if (!isdigit((unsigned char)text[i])) return 0;
        out[i] = text[i];
    }
    out[5] = '\0';
    return 1;
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
    while (isspace((unsigned char)*p)) p++;
    if (*p++ != ':') return 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p++ != '"') return 0;

    j = 0;
    while (*p != '\0' && *p != '"') {
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
            p++;
        } else {
            out[j++] = *p++;
        }
    }
    if (*p != '"') return 0;
    out[j] = '\0';
    return 1;
}

static int param_get(const char *body, const char *ctype, const char *name, char *out, size_t outsz) {
    if (ctype != NULL && strstr(ctype, "application/json") != NULL) return json_get(body, name, out, outsz);
    return form_get(body, name, out, outsz);
}

static int api_key_get(const char *body, const char *ctype, char *out, size_t outsz) {
    const char *h;

    h = getenv("HTTP_AUTHORIZATION");
    if (h != NULL && strncasecmp(h, "Bearer ", 7) == 0) {
        h += 7;
        if (strlen(h) + 1 > outsz) return 0;
        strcpy(out, h);
        return 1;
    }
    h = getenv("HTTP_X_API_KEY");
    if (h != NULL && *h != '\0') {
        if (strlen(h) + 1 > outsz) return 0;
        strcpy(out, h);
        return 1;
    }
    return param_get(body, ctype, "key", out, outsz);
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

static void http_json(int status, const char *body) {
    const char *text;

    text = "OK";
    if (status == 400) text = "Bad Request";
    else if (status == 401) text = "Unauthorized";
    else if (status == 403) text = "Forbidden";
    else if (status == 404) text = "Not Found";
    else if (status == 405) text = "Method Not Allowed";
    else if (status == 429) text = "Too Many Requests";
    else if (status >= 500) text = "Internal Server Error";

    printf("Status: %d %s\r\n", status, text);
    printf("Content-Type: application/json; charset=utf-8\r\n");
    printf("Cache-Control: no-store\r\n\r\n");
    printf("%s\n", body);
}

static void http_gateway_ok(void) {
    printf("Content-Type: text/plain; charset=utf-8\r\n");
    printf("Cache-Control: no-store\r\n\r\n");
    printf("OK\n");
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

static int db_exec(sqlite3 *db, const char *sql) {
    char *err;
    int rc;

    err = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "smsauth: %s\n", err != NULL ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        return 0;
    }
    return 1;
}

static int db_open(sqlite3 **db, int create) {
    int flags, rc;

    flags = SQLITE_OPEN_READWRITE;
    if (create) flags |= SQLITE_OPEN_CREATE;
    rc = sqlite3_open_v2(db_path(), db, flags, NULL);
    if (rc != SQLITE_OK) {
        if (*db != NULL) {
            fprintf(stderr, "smsauth: %s\n", sqlite3_errmsg(*db));
            sqlite3_close(*db);
            *db = NULL;
        }
        return 0;
    }
    sqlite3_busy_timeout(*db, 3000);
    if (!db_exec(*db, "PRAGMA journal_mode=PERSIST;")) return 0;
    if (!db_exec(*db, "PRAGMA synchronous=NORMAL;")) return 0;
    if (!db_exec(*db, "PRAGMA foreign_keys=ON;")) return 0;
    return 1;
}

static int db_init(sqlite3 *db) {
    const char *sql;

    sql =
        "PRAGMA journal_mode=PERSIST;"
        "PRAGMA synchronous=NORMAL;"
        "BEGIN;"
        "CREATE TABLE IF NOT EXISTS settings("
        "name TEXT PRIMARY KEY,"
        "value TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS channels("
        "name TEXT PRIMARY KEY COLLATE NOCASE,"
        "api_hash TEXT NOT NULL,"
        "callback_url TEXT NOT NULL DEFAULT '',"
        "callback_secret TEXT NOT NULL,"
        "ttl INTEGER NOT NULL DEFAULT 90,"
        "enabled INTEGER NOT NULL DEFAULT 1"
        ");"
        "CREATE TABLE IF NOT EXISTS challenges("
        "id TEXT PRIMARY KEY,"
        "channel TEXT NOT NULL,"
        "cell TEXT NOT NULL,"
        "code TEXT NOT NULL,"
        "created INTEGER NOT NULL,"
        "expires INTEGER NOT NULL,"
        "state INTEGER NOT NULL DEFAULT 0,"
        "completed INTEGER NOT NULL DEFAULT 0,"
        "callback_done INTEGER NOT NULL DEFAULT 0,"
        "last_status_ms INTEGER NOT NULL DEFAULT 0,"
        "FOREIGN KEY(channel) REFERENCES channels(name) ON DELETE CASCADE"
        ");"
        "CREATE INDEX IF NOT EXISTS challenges_cell_state ON challenges(cell,state,expires);"
        "CREATE INDEX IF NOT EXISTS challenges_expires ON challenges(expires);"
        "COMMIT;";
    return db_exec(db, sql);
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

static void expire_challenges(sqlite3 *db, time_t now) {
    sqlite3_stmt *st;

    st = NULL;
    if (sqlite3_prepare_v2(db,
        "UPDATE challenges SET state=?1,completed=expires WHERE state=?2 AND expires<=?3", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, STATE_TIMEOUT);
        sqlite3_bind_int(st, 2, STATE_PENDING);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM challenges WHERE created<?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)(now - RETENTION_SECONDS));
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

static int channel_auth(sqlite3 *db, const char *channel, const char *key, int *ttl) {
    sqlite3_stmt *st;
    const unsigned char *stored;
    char hash[65];
    int rc, enabled, ok;

    if (key == NULL || *key == '\0') return 0;
    sha256_hex(key, hash);
    st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT api_hash,ttl,enabled FROM channels WHERE name=?1", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    ok = 0;
    if (rc == SQLITE_ROW) {
        stored = sqlite3_column_text(st, 0);
        enabled = sqlite3_column_int(st, 2);
        if (stored != NULL && enabled && ct_equal(hash, (const char *)stored)) {
            if (ttl != NULL) *ttl = sqlite3_column_int(st, 1);
            ok = 1;
        }
    }
    sqlite3_finalize(st);
    return ok;
}

static size_t discard_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

static int callback_send(const char *url, const char *secret, const char *body) {
    CURL *curl;
    CURLcode rc;
    struct curl_slist *headers;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len;
    char sig[EVP_MAX_MD_SIZE * 2 + 1];
    char sig_header[256];
    long response;

    if (url == NULL || *url == '\0') return 0;
    if (strncmp(url, "https://", 8) != 0) return 0;

    digest_len = 0;
    if (HMAC(EVP_sha256(), secret, (int)strlen(secret),
        (const unsigned char *)body, strlen(body), digest, &digest_len) == NULL) return 0;
    hex_encode(digest, digest_len, sig);
    snprintf(sig_header, sizeof(sig_header), "X-Smsauth-Signature: %s", sig);

    curl = curl_easy_init();
    if (curl == NULL) return 0;
    headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, sig_header);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "smsauth/1.09");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, CALLBACK_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, CALLBACK_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
    rc = curl_easy_perform(curl);
    response = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && response >= 200 && response < 300;
}

static int challenge_callback_data(sqlite3 *db, const char *id,
    char *channel, size_t channel_sz, char *cell, size_t cell_sz,
    char *url, size_t url_sz, char *secret, size_t secret_sz, int *state) {
    sqlite3_stmt *st;
    const unsigned char *v;
    int rc;

    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT c.channel,c.cell,c.state,h.callback_url,h.callback_secret "
        "FROM challenges c JOIN channels h ON h.name=c.channel WHERE c.id=?1", -1, &st, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        return 0;
    }

    v = sqlite3_column_text(st, 0);
    if (v == NULL || strlen((const char *)v) + 1 > channel_sz) { sqlite3_finalize(st); return 0; }
    strcpy(channel, (const char *)v);
    v = sqlite3_column_text(st, 1);
    if (v == NULL || strlen((const char *)v) + 1 > cell_sz) { sqlite3_finalize(st); return 0; }
    strcpy(cell, (const char *)v);
    *state = sqlite3_column_int(st, 2);
    v = sqlite3_column_text(st, 3);
    if (v == NULL || strlen((const char *)v) + 1 > url_sz) { sqlite3_finalize(st); return 0; }
    strcpy(url, (const char *)v);
    v = sqlite3_column_text(st, 4);
    if (v == NULL || strlen((const char *)v) + 1 > secret_sz) { sqlite3_finalize(st); return 0; }
    strcpy(secret, (const char *)v);
    sqlite3_finalize(st);
    return 1;
}

static void mark_callback_done(sqlite3 *db, const char *id) {
    sqlite3_stmt *st;

    st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE challenges SET callback_done=1 WHERE id=?1", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static void cgi_start(sqlite3 *db, const char *body, const char *ctype) {
    char channel[65], cell_raw[64], cell[32], key[256];
    char id[33], code[6], number[256], enumber[512];
    sqlite3_stmt *st;
    time_t now;
    int ttl, rc, tries;

    if (!param_get(body, ctype, "channel", channel, sizeof(channel)) ||
        !param_get(body, ctype, "cell", cell_raw, sizeof(cell_raw)) ||
        !api_key_get(body, ctype, key, sizeof(key))) {
        http_json(400, "{\"result\":\"KO\",\"reason\":\"missing_parameter\"}");
        return;
    }
    if (!valid_channel_name(channel) || !normalize_cell(cell_raw, cell, sizeof(cell))) {
        http_json(400, "{\"result\":\"KO\",\"reason\":\"invalid_parameter\"}");
        return;
    }
    ttl = 0;
    if (!channel_auth(db, channel, key, &ttl)) {
        http_json(403, "{\"result\":\"KO\",\"reason\":\"unauthorized\"}");
        return;
    }
    if (ttl < 10 || ttl > 600) ttl = DEFAULT_TTL;

    now = time(NULL);
    expire_challenges(db, now);
    st = NULL;
    for (tries = 0; tries < 10; tries++) {
        if (!random_hex(16, id) || !random_code(code)) {
            http_json(500, "{\"result\":\"KO\",\"reason\":\"random_failure\"}");
            return;
        }
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO challenges(id,channel,cell,code,created,expires,state) VALUES(?1,?2,?3,?4,?5,?6,0)",
            -1, &st, NULL);
        if (rc != SQLITE_OK) break;
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, cell, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, code, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
        sqlite3_bind_int64(st, 6, (sqlite3_int64)(now + ttl));
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        st = NULL;
        if (rc == SQLITE_DONE) break;
    }
    if (st != NULL) sqlite3_finalize(st);
    if (tries == 10 || rc != SQLITE_DONE) {
        http_json(500, "{\"result\":\"KO\",\"reason\":\"database_error\"}");
        return;
    }

    number[0] = '\0';
    setting_get(db, "sms_number", number, sizeof(number));
    if (!json_escape(number, enumber, sizeof(enumber))) enumber[0] = '\0';
    printf("Status: 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n\r\n");
    printf("{\"result\":\"PENDING\",\"challenge\":\"%s\",\"code\":\"%s\",\"expires\":%d,\"number\":\"%s\"}\n",
        id, code, ttl, enumber);
}

static void cgi_status(sqlite3 *db, const char *body, const char *ctype) {
    char channel[65], id[64], key[256], cell[32], ecell[64];
    sqlite3_stmt *st;
    time_t now;
    long long ms, last;
    int rc, state, callback_done;
    const char *result, *reason;

    if (!param_get(body, ctype, "channel", channel, sizeof(channel)) ||
        !param_get(body, ctype, "challenge", id, sizeof(id)) ||
        !api_key_get(body, ctype, key, sizeof(key))) {
        http_json(400, "{\"result\":\"KO\",\"reason\":\"missing_parameter\"}");
        return;
    }
    if (!valid_channel_name(channel) || strlen(id) != 32 || !channel_auth(db, channel, key, NULL)) {
        http_json(403, "{\"result\":\"KO\",\"reason\":\"unauthorized\"}");
        return;
    }

    now = time(NULL);
    expire_challenges(db, now);
    ms = now_ms();
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT cell,state,callback_done,last_status_ms FROM challenges WHERE id=?1 AND channel=?2", -1, &st, NULL);
    if (rc != SQLITE_OK) {
        http_json(500, "{\"result\":\"KO\",\"reason\":\"database_error\"}");
        return;
    }
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        http_json(404, "{\"result\":\"KO\",\"reason\":\"not_found\"}");
        return;
    }
    strncpy(cell, (const char *)sqlite3_column_text(st, 0), sizeof(cell) - 1);
    cell[sizeof(cell) - 1] = '\0';
    state = sqlite3_column_int(st, 1);
    callback_done = sqlite3_column_int(st, 2);
    last = sqlite3_column_int64(st, 3);
    sqlite3_finalize(st);

    if (last > 0 && ms - last < STATUS_MIN_MS) {
        http_json(429, "{\"result\":\"KO\",\"reason\":\"rate_limit\"}");
        return;
    }
    st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE challenges SET last_status_ms=?1 WHERE id=?2", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)ms);
        sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    result = "PENDING";
    reason = "";
    if (state == STATE_OK) result = "OK";
    else if (state == STATE_WRONG) { result = "KO"; reason = "wrong_code"; }
    else if (state == STATE_TIMEOUT) { result = "KO"; reason = "timeout"; }
    json_escape(cell, ecell, sizeof(ecell));
    printf("Status: 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n\r\n");
    printf("{\"result\":\"%s\",\"reason\":\"%s\",\"cell\":\"%s\",\"callback\":%s}\n",
        result, reason, ecell, callback_done ? "true" : "false");
}

static void cgi_incoming(sqlite3 *db, const char *body, const char *ctype) {
    char key[256], key_hash[65], stored_hash[65];
    char from_raw[64], cell[32], text[MAX_VALUE], code[6];
    char id[33], channel[65], cb_cell[32], url[MAX_VALUE], secret[129];
    char ecell[64], echannel[128], callback_body[512];
    sqlite3_stmt *st;
    time_t now;
    int rc, count, state, callback_ok;

    if (!api_key_get(body, ctype, key, sizeof(key))) {
        http_json(403, "{\"result\":\"KO\",\"reason\":\"unauthorized\"}");
        return;
    }
    stored_hash[0] = '\0';
    if (!setting_get(db, "gateway_hash", stored_hash, sizeof(stored_hash))) {
        http_json(403, "{\"result\":\"KO\",\"reason\":\"gateway_not_configured\"}");
        return;
    }
    sha256_hex(key, key_hash);
    if (!ct_equal(key_hash, stored_hash)) {
        http_json(403, "{\"result\":\"KO\",\"reason\":\"unauthorized\"}");
        return;
    }
    if (!param_get(body, ctype, "from", from_raw, sizeof(from_raw)) ||
        !param_get(body, ctype, "text", text, sizeof(text)) ||
        !normalize_cell(from_raw, cell, sizeof(cell))) {
        http_gateway_ok();
        return;
    }
    if (!extract_code(text, code)) {
        http_gateway_ok();
        return;
    }

    now = time(NULL);
    if (!db_exec(db, "BEGIN IMMEDIATE;")) {
        http_json(500, "{\"result\":\"KO\",\"reason\":\"database_error\"}");
        return;
    }
    expire_challenges(db, now);
    id[0] = '\0';
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT id FROM challenges WHERE cell=?1 AND code=?2 AND state=0 AND expires>?3 ORDER BY created DESC LIMIT 1",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, cell, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, code, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
        if (sqlite3_step(st) == SQLITE_ROW) {
            strncpy(id, (const char *)sqlite3_column_text(st, 0), sizeof(id) - 1);
            id[sizeof(id) - 1] = '\0';
        }
    }
    sqlite3_finalize(st);

    state = STATE_OK;
    if (id[0] == '\0') {
        count = 0;
        st = NULL;
        rc = sqlite3_prepare_v2(db,
            "SELECT id FROM challenges WHERE cell=?1 AND state=0 AND expires>?2 ORDER BY created DESC LIMIT 2",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(st, 1, cell, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
            while (sqlite3_step(st) == SQLITE_ROW) {
                if (count == 0) {
                    strncpy(id, (const char *)sqlite3_column_text(st, 0), sizeof(id) - 1);
                    id[sizeof(id) - 1] = '\0';
                }
                count++;
            }
        }
        sqlite3_finalize(st);
        if (count != 1) id[0] = '\0';
        else state = STATE_WRONG;
    }

    if (id[0] != '\0') {
        st = NULL;
        rc = sqlite3_prepare_v2(db,
            "UPDATE challenges SET state=?1,completed=?2 WHERE id=?3 AND state=0", -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int(st, 1, state);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
            sqlite3_bind_text(st, 3, id, -1, SQLITE_STATIC);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    db_exec(db, "COMMIT;");

    if (id[0] != '\0' && challenge_callback_data(db, id,
        channel, sizeof(channel), cb_cell, sizeof(cb_cell), url, sizeof(url), secret, sizeof(secret), &state)) {
        json_escape(cb_cell, ecell, sizeof(ecell));
        json_escape(channel, echannel, sizeof(echannel));
        if (state == STATE_OK) {
            snprintf(callback_body, sizeof(callback_body),
                "{\"challenge\":\"%s\",\"channel\":\"%s\",\"result\":\"OK\",\"cell\":\"%s\",\"timestamp\":%ld}",
                id, echannel, ecell, (long)now);
        } else {
            snprintf(callback_body, sizeof(callback_body),
                "{\"challenge\":\"%s\",\"channel\":\"%s\",\"result\":\"KO\",\"reason\":\"wrong_code\",\"cell\":\"%s\",\"timestamp\":%ld}",
                id, echannel, ecell, (long)now);
        }
        callback_ok = callback_send(url, secret, callback_body);
        if (callback_ok) mark_callback_done(db, id);
    }
    http_gateway_ok();
}

static void cgi_main(void) {
    sqlite3 *db;
    char body[MAX_BODY + 1], action[32], from[MAX_VALUE], text[MAX_VALUE];
    const char *method, *ctype;
    int body_rc;

    method = getenv("REQUEST_METHOD");
    ctype = getenv("CONTENT_TYPE");
    body[0] = '\0';
    body_rc = read_body(body, sizeof(body));
    if (method == NULL || strcmp(method, "POST") != 0) {
        http_json(405, "{\"result\":\"KO\",\"reason\":\"post_required\"}");
        return;
    }
    if (body_rc <= 0) {
        http_json(400, "{\"result\":\"KO\",\"reason\":\"invalid_body\"}");
        return;
    }
    if (!db_open(&db, 0)) {
        http_json(500, "{\"result\":\"KO\",\"reason\":\"database_unavailable\"}");
        return;
    }

    action[0] = '\0';
    param_get(body, ctype, "action", action, sizeof(action));
    from[0] = '\0';
    text[0] = '\0';
    param_get(body, ctype, "from", from, sizeof(from));
    param_get(body, ctype, "text", text, sizeof(text));

    if (strcmp(action, "start") == 0) cgi_start(db, body, ctype);
    else if (strcmp(action, "status") == 0) cgi_status(db, body, ctype);
    else if (strcmp(action, "incoming") == 0 || (from[0] != '\0' && text[0] != '\0')) cgi_incoming(db, body, ctype);
    else http_json(400, "{\"result\":\"KO\",\"reason\":\"invalid_action\"}");
    sqlite3_close(db);
}

static void usage(const char *p) {
    fprintf(stderr,
        "usage:\n"
        "  %s init\n"
        "  %s gateway set key\n"
        "  %s gateway generate\n"
        "  %s number set number\n"
        "  %s number show\n"
        "  %s channel list\n"
        "  %s channel add name callback_url [ttl]\n"
        "  %s channel delete name\n"
        "  %s channel key name\n"
        "  %s channel secret name\n"
        "  %s channel callback name callback_url\n"
        "  %s channel ttl name seconds\n"
        "  %s channel enable name\n"
        "  %s channel disable name\n"
        "  %s challenge add channel cell\n"
        "  %s challenge list [limit]\n",
        p,p,p,p,p,p,p,p,p,p,p,p,p,p,p,p);
}

static int cli_gateway(sqlite3 *db, int argc, char **argv) {
    char key[65], hash[65];

    if (argc == 4 && strcmp(argv[2], "set") == 0) {
        sha256_hex(argv[3], hash);
        if (!setting_set(db, "gateway_hash", hash)) return 1;
        printf("gateway: ok\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "generate") == 0) {
        if (!random_hex(32, key)) return 1;
        sha256_hex(key, hash);
        if (!setting_set(db, "gateway_hash", hash)) return 1;
        printf("gateway_key %s\n", key);
        return 0;
    }
    return -1;
}

static int cli_number(sqlite3 *db, int argc, char **argv) {
    char number[256];

    if (argc == 4 && strcmp(argv[2], "set") == 0) {
        if (!normalize_number_list(argv[3], number, sizeof(number))) {
            fprintf(stderr, "invalid number list\n");
            return 1;
        }
        if (!setting_set(db, "sms_number", number)) return 1;
        printf("number: ok\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[2], "show") == 0) {
        if (!setting_get(db, "sms_number", number, sizeof(number))) number[0] = '\0';
        printf("%s\n", number);
        return 0;
    }
    return -1;
}

static int cli_channel_list(sqlite3 *db) {
    sqlite3_stmt *st;
    int rc;

    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT name,ttl,enabled,callback_url FROM channels ORDER BY name COLLATE NOCASE", -1, &st, NULL);
    if (rc != SQLITE_OK) return 1;
    printf("CHANNEL                         TTL  E  CALLBACK\n");
    while (sqlite3_step(st) == SQLITE_ROW) {
        printf("%-30s %4d  %c  %s\n",
            sqlite3_column_text(st, 0), sqlite3_column_int(st, 1),
            sqlite3_column_int(st, 2) ? 'Y' : 'N', sqlite3_column_text(st, 3));
    }
    sqlite3_finalize(st);
    return 0;
}

static int cli_channel_add(sqlite3 *db, int argc, char **argv) {
    sqlite3_stmt *st;
    char api_key[65], api_hash[65], secret[65];
    const char *url;
    int ttl, rc;

    if (argc < 5 || argc > 6 || !valid_channel_name(argv[3])) return -1;
    url = strcmp(argv[4], "-") == 0 ? "" : argv[4];
    if (*url != '\0' && strncmp(url, "https://", 8) != 0) {
        fprintf(stderr, "callback must use https:// or -\n");
        return 1;
    }
    ttl = argc == 6 ? atoi(argv[5]) : DEFAULT_TTL;
    if (ttl < 10 || ttl > 600) {
        fprintf(stderr, "ttl must be between 10 and 600 seconds\n");
        return 1;
    }
    if (!random_hex(32, api_key) || !random_hex(32, secret)) return 1;
    sha256_hex(api_key, api_hash);
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO channels(name,api_hash,callback_url,callback_secret,ttl,enabled) VALUES(?1,?2,?3,?4,?5,1)",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return 1;
    sqlite3_bind_text(st, 1, argv[3], -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, api_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, url, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, secret, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 5, ttl);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "channel add: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    printf("channel %s\napi_key %s\ncallback_secret %s\n", argv[3], api_key, secret);
    return 0;
}

static int cli_channel_delete(sqlite3 *db, const char *name) {
    sqlite3_stmt *st;
    int rc;

    st = NULL;
    rc = sqlite3_prepare_v2(db, "DELETE FROM channels WHERE name=?1", -1, &st, NULL);
    if (rc != SQLITE_OK) return 1;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return 1;
    printf("channel delete: %s\n", sqlite3_changes(db) ? "ok" : "not found");
    return sqlite3_changes(db) ? 0 : 1;
}

static int cli_channel_rotate(sqlite3 *db, const char *name, int secret_mode) {
    sqlite3_stmt *st;
    char value[65], hash[65];
    int rc;

    if (!random_hex(32, value)) return 1;
    st = NULL;
    if (secret_mode) {
        rc = sqlite3_prepare_v2(db, "UPDATE channels SET callback_secret=?1 WHERE name=?2", -1, &st, NULL);
        if (rc != SQLITE_OK) return 1;
        sqlite3_bind_text(st, 1, value, -1, SQLITE_STATIC);
    } else {
        sha256_hex(value, hash);
        rc = sqlite3_prepare_v2(db, "UPDATE channels SET api_hash=?1 WHERE name=?2", -1, &st, NULL);
        if (rc != SQLITE_OK) return 1;
        sqlite3_bind_text(st, 1, hash, -1, SQLITE_STATIC);
    }
    sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || sqlite3_changes(db) == 0) return 1;
    printf("%s %s\n", secret_mode ? "callback_secret" : "api_key", value);
    return 0;
}

static int cli_channel_set(sqlite3 *db, int argc, char **argv) {
    sqlite3_stmt *st;
    const char *sql, *value;
    int rc, n;

    if (argc != 5) return -1;
    sql = NULL;
    value = argv[4];
    if (strcmp(argv[2], "callback") == 0) {
        if (strcmp(value, "-") == 0) value = "";
        if (*value != '\0' && strncmp(value, "https://", 8) != 0) return 1;
        sql = "UPDATE channels SET callback_url=?1 WHERE name=?2";
    } else if (strcmp(argv[2], "ttl") == 0) {
        n = atoi(value);
        if (n < 10 || n > 600) return 1;
        sql = "UPDATE channels SET ttl=?1 WHERE name=?2";
    } else return -1;

    st = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) return 1;
    if (strcmp(argv[2], "ttl") == 0) sqlite3_bind_int(st, 1, atoi(value));
    else sqlite3_bind_text(st, 1, value, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, argv[3], -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || sqlite3_changes(db) == 0) return 1;
    printf("channel %s: ok\n", argv[2]);
    return 0;
}

static int cli_channel_enable(sqlite3 *db, const char *name, int enabled) {
    sqlite3_stmt *st;
    int rc;

    st = NULL;
    rc = sqlite3_prepare_v2(db, "UPDATE channels SET enabled=?1 WHERE name=?2", -1, &st, NULL);
    if (rc != SQLITE_OK) return 1;
    sqlite3_bind_int(st, 1, enabled);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || sqlite3_changes(db) == 0) return 1;
    printf("channel %s: ok\n", enabled ? "enable" : "disable");
    return 0;
}

static int cli_channel(sqlite3 *db, int argc, char **argv) {
    if (argc == 3 && strcmp(argv[2], "list") == 0) return cli_channel_list(db);
    if (argc >= 5 && strcmp(argv[2], "add") == 0) return cli_channel_add(db, argc, argv);
    if (argc == 4 && strcmp(argv[2], "delete") == 0) return cli_channel_delete(db, argv[3]);
    if (argc == 4 && strcmp(argv[2], "key") == 0) return cli_channel_rotate(db, argv[3], 0);
    if (argc == 4 && strcmp(argv[2], "secret") == 0) return cli_channel_rotate(db, argv[3], 1);
    if (argc == 5 && (strcmp(argv[2], "callback") == 0 || strcmp(argv[2], "ttl") == 0)) return cli_channel_set(db, argc, argv);
    if (argc == 4 && strcmp(argv[2], "enable") == 0) return cli_channel_enable(db, argv[3], 1);
    if (argc == 4 && strcmp(argv[2], "disable") == 0) return cli_channel_enable(db, argv[3], 0);
    return -1;
}

static const char *state_name(int state) {
    if (state == STATE_OK) return "OK";
    if (state == STATE_WRONG) return "WRONG";
    if (state == STATE_TIMEOUT) return "TIMEOUT";
    return "PENDING";
}

static int cli_challenge_add(sqlite3 *db, int argc, char **argv) {
    sqlite3_stmt *st;
    char cell[32], id[33], code[6];
    time_t now;
    int ttl, enabled, rc, tries;

    if (argc != 5 || strcmp(argv[2], "add") != 0 || !valid_channel_name(argv[3])) return -1;
    if (!normalize_cell(argv[4], cell, sizeof(cell))) {
        fprintf(stderr, "invalid cell\n");
        return 1;
    }

    st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT ttl,enabled FROM channels WHERE name=?1", -1, &st, NULL);
    if (rc != SQLITE_OK) return 1;
    sqlite3_bind_text(st, 1, argv[3], -1, SQLITE_STATIC);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(st);
        fprintf(stderr, "channel not found\n");
        return 1;
    }
    ttl = sqlite3_column_int(st, 0);
    enabled = sqlite3_column_int(st, 1);
    sqlite3_finalize(st);
    if (!enabled) {
        fprintf(stderr, "channel disabled\n");
        return 1;
    }
    if (ttl < 10 || ttl > 600) ttl = DEFAULT_TTL;

    now = time(NULL);
    expire_challenges(db, now);
    st = NULL;
    rc = SQLITE_ERROR;
    for (tries = 0; tries < 10; tries++) {
        if (!random_hex(16, id) || !random_code(code)) return 1;
        rc = sqlite3_prepare_v2(db,
            "INSERT INTO challenges(id,channel,cell,code,created,expires,state) VALUES(?1,?2,?3,?4,?5,?6,0)",
            -1, &st, NULL);
        if (rc != SQLITE_OK) break;
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, argv[3], -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, cell, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, code, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
        sqlite3_bind_int64(st, 6, (sqlite3_int64)(now + ttl));
        rc = sqlite3_step(st);
        sqlite3_finalize(st);
        st = NULL;
        if (rc == SQLITE_DONE) break;
    }
    if (st != NULL) sqlite3_finalize(st);
    if (tries == 10 || rc != SQLITE_DONE) {
        fprintf(stderr, "challenge add: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    printf("challenge %s\ncode %s\nexpires %d\n", id, code, ttl);
    return 0;
}

static int cli_challenge_list(sqlite3 *db, int argc, char **argv) {
    sqlite3_stmt *st;
    time_t now;
    int limit, rc;

    if (argc < 3 || argc > 4 || strcmp(argv[2], "list") != 0) return -1;
    limit = argc == 4 ? atoi(argv[3]) : 20;
    if (limit < 1) limit = 20;
    if (limit > 1000) limit = 1000;
    now = time(NULL);
    expire_challenges(db, now);
    st = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT id,channel,cell,created,expires,state,callback_done FROM challenges ORDER BY created DESC LIMIT ?1",
        -1, &st, NULL);
    if (rc != SQLITE_OK) return 1;
    sqlite3_bind_int(st, 1, limit);
    printf("CHALLENGE                         CHANNEL          CELL             STATE    CALLBACK\n");
    while (sqlite3_step(st) == SQLITE_ROW) {
        printf("%-32s %-16s %-16s %-8s %s\n",
            sqlite3_column_text(st, 0), sqlite3_column_text(st, 1), sqlite3_column_text(st, 2),
            state_name(sqlite3_column_int(st, 5)), sqlite3_column_int(st, 6) ? "yes" : "no");
    }
    sqlite3_finalize(st);
    return 0;
}

int main(int argc, char **argv) {
    sqlite3 *db;
    int rc;

    if (getenv("GATEWAY_INTERFACE") != NULL && *getenv("GATEWAY_INTERFACE") != '\0') {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        cgi_main();
        curl_global_cleanup();
        return 0;
    }

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "init") == 0) {
        if (argc != 2 || !db_open(&db, 1)) return 1;
        rc = db_init(db) ? 0 : 1;
        if (rc == 0) printf("init: ok\n");
        sqlite3_close(db);
        return rc;
    }

    if (!db_open(&db, 0)) return 1;
    if (strcmp(argv[1], "gateway") == 0) rc = cli_gateway(db, argc, argv);
    else if (strcmp(argv[1], "number") == 0) rc = cli_number(db, argc, argv);
    else if (strcmp(argv[1], "channel") == 0) rc = cli_channel(db, argc, argv);
    else if (strcmp(argv[1], "challenge") == 0) {
        if (argc >= 3 && strcmp(argv[2], "add") == 0) rc = cli_challenge_add(db, argc, argv);
        else rc = cli_challenge_list(db, argc, argv);
    }
    else rc = -1;
    sqlite3_close(db);
    if (rc == -1) {
        usage(argv[0]);
        return 1;
    }
    return rc;
}
