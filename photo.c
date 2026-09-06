// Gianluca Mazzini @2004- Version 3.04

#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sqlite3.h>

#define DB_PATH "/home/tools/mcp/work/misc/photo.db"
#define PARAM_SIZE 1024

static void usage(const char *name) {
    fprintf(stderr,
        "usage:\n"
        "  %s init\n"
        "  %s list [text]\n"
        "  %s search text\n"
        "  %s add album url year1 [year2]\n"
        "  %s edit id field value\n"
        "  %s hide id\n"
        "  %s show id\n"
        "  %s delete id\n",
        name, name, name, name, name, name, name, name);
}

static int parse_int(const char *s, int *value) {
    char *end;
    long v;

    if (s == NULL || *s == '\0')
        return 0;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < 0 || v > 2147483647L)
        return 0;
    *value = (int)v;
    return 1;
}

static int valid_year(int year) {
    return year == 0 || (year >= 1900 && year <= 2200);
}

static int hexval(int c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    c = tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static void decode(char *dst, size_t size, const char *src, size_t len) {
    size_t i, j;
    int a, b;

    j = 0;
    for (i = 0; i < len && j + 1 < size; i++) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && i + 2 < len) {
            a = hexval((unsigned char)src[i + 1]);
            b = hexval((unsigned char)src[i + 2]);
            if (a >= 0 && b >= 0) {
                dst[j++] = (char)((a << 4) | b);
                i += 2;
            } else {
                dst[j++] = src[i];
            }
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static int param(const char *data, const char *name, char *out, size_t size) {
    const char *p, *eq, *end;
    size_t nlen;

    if (data == NULL)
        return 0;
    nlen = strlen(name);
    p = data;
    for (;;) {
        end = strchr(p, '&');
        if (end == NULL)
            end = p + strlen(p);
        eq = memchr(p, '=', (size_t)(end - p));
        if (eq != NULL && (size_t)(eq - p) == nlen && memcmp(p, name, nlen) == 0) {
            decode(out, size, eq + 1, (size_t)(end - eq - 1));
            return 1;
        }
        if (*end == '\0')
            break;
        p = end + 1;
    }
    return 0;
}

static void html(const char *s) {
    const unsigned char *p;

    p = (const unsigned char *)s;
    for (; *p != '\0'; p++) {
        if (*p == '&')
            fputs("&amp;", stdout);
        else if (*p == '<')
            fputs("&lt;", stdout);
        else if (*p == '>')
            fputs("&gt;", stdout);
        else if (*p == '"')
            fputs("&quot;", stdout);
        else if (*p == '\'')
            fputs("&#39;", stdout);
        else
            putchar(*p);
    }
}

static void page_header(void) {
    printf("Content-Type: text/html; charset=utf-8\r\n");
    printf("Cache-Control: no-cache\r\n\r\n");
    printf("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">");
    printf("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    printf("<title>Gianluca Mazzini Photos</title><style>");
    printf("*{box-sizing:border-box}body{margin:0;background:#f4f5f4;color:#252725;font-family:Arial,sans-serif}");
    printf("main{max-width:1100px;margin:auto;padding:30px 20px 55px}h1{margin:0;font-size:2rem}.sub{margin:6px 0 24px;color:#6a6d6a}");
    printf("a{color:#176b45;text-decoration:none}.years{display:flex;gap:7px;flex-wrap:wrap;margin:18px 0}.year{padding:7px 10px;border:1px solid #cfd6d1;border-radius:7px;background:white}");
    printf("form{display:flex;gap:8px;margin:18px 0 25px}input{width:100%%;padding:10px 12px;border:1px solid #cbd1cd;border-radius:8px;font:inherit}");
    printf("button{padding:10px 15px;border:0;border-radius:8px;background:#176b45;color:white;font:inherit;cursor:pointer}");
    printf(".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:12px}.card{padding:15px;background:white;border:1px solid #e0e3e1;border-radius:10px}");
    printf(".name{font-weight:bold;font-size:1.05rem}.when{color:#777;margin-top:6px}.count,.footer{color:#777;margin-top:18px}.footer{padding-top:18px;border-top:1px solid #ddd;font-size:.9rem}");
    printf("</style></head><body><main><h1>Gianluca Mazzini Photos</h1>");
    printf("<div class=\"sub\">Photo by Gianluca Mazzini. All rights reserved.</div>");
}

static void page_footer(void) {
    printf("<div class=\"footer\">Archive online since 2004.</div></main></body></html>\n");
}

static void print_year(int year1, int year2) {
    if (year1 > 0 && year2 > 0)
        printf("%d &ndash; %d", year1, year2);
    else if (year1 > 0)
        printf("%d", year1);
    else if (year2 > 0)
        printf("%d", year2);
}

static int web_years(sqlite3 *db) {
    sqlite3_stmt *st;
    int rc, year;

    rc = sqlite3_prepare_v2(db,
        "SELECT year FROM (SELECT year1 AS year FROM photo WHERE flag=0 AND year1>0 "
        "UNION SELECT year2 AS year FROM photo WHERE flag=0 AND year2>0) ORDER BY year DESC",
        -1, &st, NULL);
    if (rc != SQLITE_OK)
        return 0;

    printf("<div class=\"years\"><a class=\"year\" href=\"?rel=all\">All</a>");
    for (;;) {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW)
            break;
        year = sqlite3_column_int(st, 0);
        printf("<a class=\"year\" href=\"?rel=%d\">%d</a>", year, year);
    }
    printf("</div>");
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

static int web(sqlite3 *db) {
    sqlite3_stmt *st;
    const char *query, *sql;
    const unsigned char *album, *url;
    char lookup[PARAM_SIZE], rel[64], pattern[PARAM_SIZE + 2];
    int rc, year, year1, year2, count, use_lookup, use_year;

    query = getenv("QUERY_STRING");
    lookup[0] = '\0';
    rel[0] = '\0';
    use_lookup = param(query, "lookup", lookup, sizeof(lookup)) && lookup[0] != '\0';
    use_year = param(query, "rel", rel, sizeof(rel));
    if (!use_year)
        use_year = param(query, "year", rel, sizeof(rel));

    page_header();
    if (!web_years(db)) {
        printf("<p>Database error.</p>");
        page_footer();
        return 1;
    }

    printf("<form method=\"get\"><input name=\"lookup\" placeholder=\"Search albums\" value=\"");
    html(lookup);
    printf("\"><button type=\"submit\">Search</button></form>");

    if (use_lookup) {
        sql = "SELECT album,url,year1,year2 FROM photo WHERE flag=0 AND album LIKE ?1 COLLATE NOCASE ORDER BY album COLLATE NOCASE,year1,year2";
    } else if (use_year && strcmp(rel, "all") != 0 && parse_int(rel, &year)) {
        sql = "SELECT album,url,year1,year2 FROM photo WHERE flag=0 AND (year1=?1 OR year2=?1) ORDER BY album COLLATE NOCASE,year1,year2";
    } else {
        sql = "SELECT album,url,year1,year2 FROM photo WHERE flag=0 ORDER BY album COLLATE NOCASE,year1,year2";
    }

    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        printf("<p>Database error.</p>");
        page_footer();
        return 1;
    }
    if (use_lookup) {
        snprintf(pattern, sizeof(pattern), "%%%s%%", lookup);
        sqlite3_bind_text(st, 1, pattern, -1, SQLITE_TRANSIENT);
    } else if (use_year && strcmp(rel, "all") != 0 && parse_int(rel, &year)) {
        sqlite3_bind_int(st, 1, year);
    }

    count = 0;
    printf("<div class=\"grid\">");
    for (;;) {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW)
            break;
        album = sqlite3_column_text(st, 0);
        url = sqlite3_column_text(st, 1);
        year1 = sqlite3_column_int(st, 2);
        year2 = sqlite3_column_int(st, 3);
        printf("<div class=\"card\"><a class=\"name\" href=\"");
        html((const char *)url);
        printf("\" target=\"_blank\" rel=\"noopener\">");
        html((const char *)album);
        printf("</a><div class=\"when\">");
        print_year(year1, year2);
        printf("</div></div>");
        count++;
    }
    printf("</div><div class=\"count\">%d album%s</div>", count, count == 1 ? "" : "s");
    sqlite3_finalize(st);
    page_footer();
    return rc == SQLITE_DONE ? 0 : 1;
}

static int cli_list(sqlite3 *db, const char *text) {
    sqlite3_stmt *st;
    const char *sql;
    char pattern[PARAM_SIZE + 2];
    int rc;

    if (text != NULL) {
        sql = "SELECT id,flag,album,url,year1,year2 FROM photo WHERE album LIKE ?1 COLLATE NOCASE ORDER BY album COLLATE NOCASE,year1,year2";
    } else {
        sql = "SELECT id,flag,album,url,year1,year2 FROM photo ORDER BY album COLLATE NOCASE,year1,year2";
    }
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return 0;
    if (text != NULL) {
        snprintf(pattern, sizeof(pattern), "%%%s%%", text);
        sqlite3_bind_text(st, 1, pattern, -1, SQLITE_TRANSIENT);
    }

    printf("ID   S  YEAR       ALBUM | URL\n");
    for (;;) {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW)
            break;
        printf("%-4d %c  ", sqlite3_column_int(st, 0), sqlite3_column_int(st, 1) ? 'H' : 'V');
        if (sqlite3_column_int(st, 5) > 0)
            printf("%d-%-4d ", sqlite3_column_int(st, 4), sqlite3_column_int(st, 5));
        else
            printf("%-9d ", sqlite3_column_int(st, 4));
        printf("%s | %s\n", sqlite3_column_text(st, 2), sqlite3_column_text(st, 3));
    }
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

static int changed(sqlite3 *db, const char *what) {
    if (sqlite3_changes(db) != 1) {
        fprintf(stderr, "%s: id not found\n", what);
        return 0;
    }
    printf("%s: ok\n", what);
    return 1;
}

static int cli_add(sqlite3 *db, int argc, char **argv) {
    sqlite3_stmt *st;
    int rc, year1, year2;

    if (argc != 5 && argc != 6)
        return 0;
    if (!parse_int(argv[4], &year1))
        return 0;
    year2 = 0;
    if (argc == 6 && !parse_int(argv[5], &year2))
        return 0;
    if (!valid_year(year1) || !valid_year(year2) || year1 == 0)
        return 0;

    rc = sqlite3_prepare_v2(db, "INSERT INTO photo(album,url,year1,year2,flag) VALUES(?1,?2,?3,?4,0)", -1, &st, NULL);
    if (rc != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, argv[2], -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, argv[3], -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, year1);
    sqlite3_bind_int(st, 4, year2);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "add: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    printf("add: id %lld\n", (long long)sqlite3_last_insert_rowid(db));
    return 1;
}

static int cli_edit(sqlite3 *db, int argc, char **argv) {
    sqlite3_stmt *st;
    const char *sql, *field;
    int rc, id, value;

    if (argc != 5)
        return 0;
    if (!parse_int(argv[2], &id))
        return 0;

    field = argv[3];
    if (strcmp(field, "album") == 0) {
        sql = "UPDATE photo SET album=?1 WHERE id=?2";
    } else if (strcmp(field, "url") == 0) {
        sql = "UPDATE photo SET url=?1 WHERE id=?2";
    } else if (strcmp(field, "year1") == 0 || strcmp(field, "year2") == 0) {
        if (!parse_int(argv[4], &value) || !valid_year(value))
            return 0;
        if (strcmp(field, "year1") == 0 && value == 0)
            return 0;
        sql = strcmp(field, "year1") == 0 ?
            "UPDATE photo SET year1=?1 WHERE id=?2" :
            "UPDATE photo SET year2=?1 WHERE id=?2";
    } else {
        return 0;
    }

    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return 0;
    if (strcmp(field, "year1") == 0 || strcmp(field, "year2") == 0)
        sqlite3_bind_int(st, 1, value);
    else
        sqlite3_bind_text(st, 1, argv[4], -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "edit: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    return changed(db, "edit");
}

static int cli_flag(sqlite3 *db, const char *idstr, int flag, const char *what) {
    sqlite3_stmt *st;
    int rc, id;

    if (!parse_int(idstr, &id))
        return 0;
    rc = sqlite3_prepare_v2(db, "UPDATE photo SET flag=?1 WHERE id=?2", -1, &st, NULL);
    if (rc != SQLITE_OK)
        return 0;
    sqlite3_bind_int(st, 1, flag);
    sqlite3_bind_int(st, 2, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return 0;
    return changed(db, what);
}

static int cli_delete(sqlite3 *db, const char *idstr) {
    sqlite3_stmt *st;
    int rc, id;

    if (!parse_int(idstr, &id))
        return 0;
    rc = sqlite3_prepare_v2(db, "DELETE FROM photo WHERE id=?1", -1, &st, NULL);
    if (rc != SQLITE_OK)
        return 0;
    sqlite3_bind_int(st, 1, id);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        return 0;
    return changed(db, "delete");
}

static int cli_init(sqlite3 *db) {
    const char *sql;
    char *error;
    int rc;

    sql =
        "BEGIN;"
        "CREATE TABLE IF NOT EXISTS photo ("
        "id INTEGER PRIMARY KEY,"
        "album TEXT NOT NULL,"
        "url TEXT NOT NULL,"
        "year1 INTEGER NOT NULL,"
        "year2 INTEGER NOT NULL DEFAULT 0,"
        "flag INTEGER NOT NULL DEFAULT 0,"
        "UNIQUE(album,year1,year2));"
        "CREATE INDEX IF NOT EXISTS photo_album ON photo(album COLLATE NOCASE);"
        "CREATE INDEX IF NOT EXISTS photo_year1 ON photo(year1);"
        "CREATE INDEX IF NOT EXISTS photo_year2 ON photo(year2);"
        "CREATE INDEX IF NOT EXISTS photo_flag ON photo(flag);"
        "COMMIT;";
    error = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "init: %s\n", error != NULL ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        return 0;
    }
    printf("init: ok\n");
    return 1;
}

static int cli(sqlite3 *db, int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "init") == 0)
        return cli_init(db);
    if (argc >= 2 && strcmp(argv[1], "list") == 0 && argc <= 3)
        return cli_list(db, argc == 3 ? argv[2] : NULL);
    if (argc == 3 && strcmp(argv[1], "search") == 0)
        return cli_list(db, argv[2]);
    if (argc >= 2 && strcmp(argv[1], "add") == 0)
        return cli_add(db, argc, argv);
    if (argc >= 2 && strcmp(argv[1], "edit") == 0)
        return cli_edit(db, argc, argv);
    if (argc == 3 && strcmp(argv[1], "hide") == 0)
        return cli_flag(db, argv[2], 1, "hide");
    if (argc == 3 && strcmp(argv[1], "show") == 0)
        return cli_flag(db, argv[2], 0, "show");
    if (argc == 3 && strcmp(argv[1], "delete") == 0)
        return cli_delete(db, argv[2]);
    return 0;
}

int main(int argc, char **argv) {
    sqlite3 *db;
    const char *gateway;
    int rc, ok, readonly;

    gateway = getenv("GATEWAY_INTERFACE");
    readonly = gateway != NULL && *gateway != '\0';
    rc = sqlite3_open_v2(DB_PATH, &db,
        readonly ? SQLITE_OPEN_READONLY :
        (argc == 2 && strcmp(argv[1], "init") == 0 ?
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READWRITE),
        NULL);
    if (rc != SQLITE_OK) {
        if (readonly)
            printf("Content-Type: text/plain; charset=utf-8\r\n\r\ndatabase unavailable\n");
        else
            fprintf(stderr, "database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 2;
    }
    sqlite3_busy_timeout(db, 3000);

    if (readonly) {
        ok = web(db) == 0;
    } else {
        ok = cli(db, argc, argv);
        if (!ok)
            usage(argv[0]);
    }

    sqlite3_close(db);
    return ok ? 0 : 1;
}
