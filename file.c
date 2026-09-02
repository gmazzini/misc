// Gianluca Mazzini @2026- Version 1.5

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>

#ifndef FILE_ROOT
#define FILE_ROOT "/home/www/file/rep"
#endif

#define MAX_COMMAND 16
#define MAX_BOUNDARY 256
#define MAX_FIELD 256
#define MAX_VALUE 4096
#define MAX_PATH 4096
#define MAX_FILES 64
#define COPY_BUFFER 65536

typedef struct {
    char name[MAX_FIELD];
    long offset;
    long length;
} upload_part;

typedef struct {
    FILE *body;
    upload_part files[MAX_FILES];
    int file_count;
    char id[MAX_VALUE];
    char name[MAX_VALUE];
    char date[MAX_VALUE];
} request_data;

static void text_header(void) {
    printf("Content-Type: text/plain\r\n\r\n");
}

static int safe_component(const char *s) {
    const unsigned char *p;

    if (s == NULL || *s == '\0') return 0;
    if (!strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (p = (const unsigned char *)s; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '"' || *p < 32 || *p == 127) return 0;
    }
    return 1;
}

static int make_path(char *out, size_t size, const char *id, const char *sub, const char *name) {
    int n;

    if (sub != NULL && name != NULL)
        n = snprintf(out, size, "%s/%s/%s/%s", FILE_ROOT, id, sub, name);
    else if (sub != NULL)
        n = snprintf(out, size, "%s/%s/%s", FILE_ROOT, id, sub);
    else if (name != NULL)
        n = snprintf(out, size, "%s/%s/%s", FILE_ROOT, id, name);
    else
        n = snprintf(out, size, "%s/%s", FILE_ROOT, id);
    return n >= 0 && (size_t)n < size;
}

static int directory_exists(const char *path) {
    struct stat st;

    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static int regular_file_exists(const char *path) {
    struct stat st;

    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
}

static int lock_directory(const char *path, int exclusive) {
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (flock(fd, exclusive ? LOCK_EX : LOCK_SH) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int parse_command(char *out, size_t size) {
    const char *uri;
    const char *p;
    size_t n;

    uri = getenv("REQUEST_URI");
    if (uri == NULL) uri = getenv("SCRIPT_NAME");
    if (uri == NULL) return 0;
    p = uri;
    while (*p == '/') p++;
    n = 0;
    while (p[n] && p[n] != '.' && p[n] != '/' && p[n] != '?') n++;
    if (n == 0 || n >= size) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int get_boundary(char *out, size_t size) {
    const char *ct;
    const char *p;
    const char *end;
    size_t n;

    ct = getenv("CONTENT_TYPE");
    if (ct == NULL) return 0;
    p = strstr(ct, "boundary=");
    if (p == NULL) return 0;
    p += 9;
    if (*p == '"') {
        p++;
        end = strchr(p, '"');
    } else {
        end = p;
        while (*end && *end != ';' && *end != ' ' && *end != '\t') end++;
    }
    if (end == NULL) return 0;
    n = (size_t)(end - p);
    if (n == 0 || n + 3 >= size) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int spool_body(FILE **out) {
    const char *cl;
    char buffer[COPY_BUFFER];
    long remaining;
    size_t want;
    size_t got;
    FILE *f;

    cl = getenv("CONTENT_LENGTH");
    if (cl == NULL) return 0;
    errno = 0;
    remaining = strtol(cl, NULL, 10);
    if (errno != 0 || remaining < 0) return 0;
    f = tmpfile();
    if (f == NULL) return 0;
    while (remaining > 0) {
        want = remaining > (long)sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        got = fread(buffer, 1, want, stdin);
        if (got == 0) {
            fclose(f);
            return 0;
        }
        if (fwrite(buffer, 1, got, f) != got) {
            fclose(f);
            return 0;
        }
        remaining -= (long)got;
    }
    rewind(f);
    *out = f;
    return 1;
}

static int read_line(FILE *f, char *line, size_t size) {
    size_t n;

    if (fgets(line, (int)size, f) == NULL) return 0;
    n = strlen(line);
    if (n > 0 && line[n - 1] == '\n') line[--n] = '\0';
    if (n > 0 && line[n - 1] == '\r') line[--n] = '\0';
    return 1;
}

static int extract_quoted(const char *line, const char *key, char *out, size_t size) {
    const char *p;
    const char *q;
    size_t n;

    p = strstr(line, key);
    if (p == NULL) return 0;
    p += strlen(key);
    q = strchr(p, '"');
    if (q == NULL) return 0;
    n = (size_t)(q - p);
    if (n == 0 || n >= size) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}

static int find_boundary(FILE *f, const char *boundary, long *data_end, int *final_boundary) {
    char marker[MAX_BOUNDARY + 8];
    char buffer[COPY_BUFFER];
    long start;
    long pos;
    long candidate;
    size_t marker_len;
    size_t carry;
    size_t got;
    size_t i;

    if (snprintf(marker, sizeof(marker), "\r\n--%s", boundary) >= (int)sizeof(marker)) return 0;
    marker_len = strlen(marker);
    start = ftell(f);
    if (start < 0) return 0;
    carry = 0;
    pos = start;
    for (;;) {
        got = fread(buffer + carry, 1, sizeof(buffer) - carry, f);
        if (got == 0 && carry < marker_len) return 0;
        got += carry;
        for (i = 0; i + marker_len <= got; i++) {
            if (!memcmp(buffer + i, marker, marker_len)) {
                candidate = pos - (long)carry + (long)i;
                *data_end = candidate;
                if (fseek(f, candidate + (long)marker_len, SEEK_SET) != 0) return 0;
                if (fread(buffer, 1, 2, f) != 2) return 0;
                if (buffer[0] == '-' && buffer[1] == '-') {
                    *final_boundary = 1;
                    return 1;
                }
                if (buffer[0] == '\r' && buffer[1] == '\n') {
                    *final_boundary = 0;
                    return 1;
                }
                return 0;
            }
        }
        if (got < marker_len - 1) return 0;
        carry = marker_len - 1;
        memmove(buffer, buffer + got - carry, carry);
        pos = ftell(f);
        if (pos < 0) return 0;
    }
}

static int read_value(FILE *f, long start, long length, char *out, size_t size) {
    size_t n;

    if (length < 0 || (unsigned long)length >= (unsigned long)size) return 0;
    if (fseek(f, start, SEEK_SET) != 0) return 0;
    n = fread(out, 1, (size_t)length, f);
    if (n != (size_t)length) return 0;
    out[n] = '\0';
    return 1;
}

static int parse_multipart(request_data *r) {
    char boundary[MAX_BOUNDARY];
    char first[MAX_BOUNDARY + 8];
    char line[4096];
    char field[MAX_FIELD];
    char filename[MAX_FIELD];
    long data_start;
    long data_end;
    long return_pos;
    int final_boundary;
    int have_disposition;
    int is_file;

    if (!get_boundary(boundary, sizeof(boundary))) return 0;
    if (!spool_body(&r->body)) return 0;
    if (!read_line(r->body, line, sizeof(line))) return 0;
    if (snprintf(first, sizeof(first), "--%s", boundary) >= (int)sizeof(first)) return 0;
    if (strcmp(line, first)) return 0;
    final_boundary = 0;
    while (!final_boundary) {
        field[0] = '\0';
        filename[0] = '\0';
        have_disposition = 0;
        is_file = 0;
        for (;;) {
            if (!read_line(r->body, line, sizeof(line))) return 0;
            if (line[0] == '\0') break;
            if (!strncmp(line, "Content-Disposition:", 20)) {
                if (!extract_quoted(line, "name=\"", field, sizeof(field))) return 0;
                if (extract_quoted(line, "filename=\"", filename, sizeof(filename))) is_file = 1;
                have_disposition = 1;
            }
        }
        if (!have_disposition || field[0] == '\0') return 0;
        data_start = ftell(r->body);
        if (data_start < 0) return 0;
        if (!find_boundary(r->body, boundary, &data_end, &final_boundary)) return 0;
        return_pos = ftell(r->body);
        if (return_pos < 0 || data_end < data_start) return 0;
        if (is_file) {
            if (r->file_count >= MAX_FILES) return 0;
            if (!safe_component(field)) return 0;
            strcpy(r->files[r->file_count].name, field);
            r->files[r->file_count].offset = data_start;
            r->files[r->file_count].length = data_end - data_start;
            r->file_count++;
        } else if (!strcmp(field, "id")) {
            if (!read_value(r->body, data_start, data_end - data_start, r->id, sizeof(r->id))) return 0;
        } else if (!strcmp(field, "name")) {
            if (!read_value(r->body, data_start, data_end - data_start, r->name, sizeof(r->name))) return 0;
        } else if (!strcmp(field, "date")) {
            if (!read_value(r->body, data_start, data_end - data_start, r->date, sizeof(r->date))) return 0;
        }
        if (fseek(r->body, return_pos, SEEK_SET) != 0) return 0;
    }
    return 1;
}

static int validate_id(const request_data *r, char *dir, size_t size) {
    if (!safe_component(r->id)) return 0;
    if (!make_path(dir, size, r->id, NULL, NULL)) return 0;
    return directory_exists(dir);
}

static int archive_path(char *out, size_t size, const char *id, const char *name) {
    time_t now;
    time_t candidate;
    struct tm tmv;
    char stamp[32];
    char archived_name[MAX_VALUE + 48];
    struct stat st;
    int i;
    int n;

    now = time(NULL);
    for (i = 0; i < 1000000; i++) {
        candidate = now + i;
        if (localtime_r(&candidate, &tmv) == NULL) return 0;
        if (strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tmv) == 0) return 0;
        n = snprintf(archived_name, sizeof(archived_name), "%s.%s", name, stamp);
        if (n < 0 || (size_t)n >= sizeof(archived_name)) return 0;
        if (!make_path(out, size, id, "_old_", archived_name)) return 0;
        if (lstat(out, &st) != 0) {
            if (errno == ENOENT) return 1;
            return 0;
        }
    }
    return 0;
}

static int copy_range(FILE *src, long offset, long length, FILE *dst) {
    char buffer[COPY_BUFFER];
    long remaining;
    size_t want;
    size_t got;

    if (fseek(src, offset, SEEK_SET) != 0) return 0;
    remaining = length;
    while (remaining > 0) {
        want = remaining > (long)sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        got = fread(buffer, 1, want, src);
        if (got == 0) return 0;
        if (fwrite(buffer, 1, got, dst) != got) return 0;
        remaining -= (long)got;
    }
    return 1;
}

static int do_upload(request_data *r) {
    char dir[MAX_PATH];
    char olddir[MAX_PATH];
    char current[MAX_PATH];
    char archived[MAX_PATH];
    FILE *out;
    int lockfd;
    int i;
    int rc;

    text_header();
    if (!validate_id(r, dir, sizeof(dir))) {
        printf("ID not found\n");
        return 1;
    }
    lockfd = lock_directory(dir, 1);
    if (lockfd < 0) {
        printf("Storage error\n");
        return 1;
    }
    rc = 1;
    if (!make_path(olddir, sizeof(olddir), r->id, "_old_", NULL)) goto done;
    if (!directory_exists(olddir) && mkdir(olddir, 0775) != 0 && errno != EEXIST) {
        printf("Storage error\n");
        goto done;
    }
    for (i = 0; i < r->file_count; i++) {
        if (!make_path(current, sizeof(current), r->id, NULL, r->files[i].name)) {
            printf("Storage error\n");
            goto done;
        }
        if (regular_file_exists(current)) {
            if (!archive_path(archived, sizeof(archived), r->id, r->files[i].name)) {
                printf("Storage error\n");
                goto done;
            }
            if (rename(current, archived) != 0) {
                printf("Storage error\n");
                goto done;
            }
        }
        out = fopen(current, "wb");
        if (out == NULL) {
            printf("Storage error\n");
            goto done;
        }
        if (!copy_range(r->body, r->files[i].offset, r->files[i].length, out)) {
            fclose(out);
            unlink(current);
            printf("Storage error\n");
            goto done;
        }
        if (fclose(out) != 0) {
            unlink(current);
            printf("Storage error\n");
            goto done;
        }
        printf("File %s uploaded\n", r->files[i].name);
    }
    rc = 0;
done:
    close(lockfd);
    return rc;
}

static int do_list(request_data *r, int old) {
    char dir[MAX_PATH];
    char path[MAX_PATH];
    struct dirent **entries;
    struct stat st;
    int lockfd;
    int count;
    int i;

    text_header();
    if (!validate_id(r, dir, sizeof(dir))) {
        printf("ID not found\n");
        return 1;
    }
    lockfd = lock_directory(dir, 0);
    if (lockfd < 0) {
        printf("Storage error\n");
        return 1;
    }
    if (old && !make_path(dir, sizeof(dir), r->id, "_old_", NULL)) {
        close(lockfd);
        return 1;
    }
    count = scandir(dir, &entries, NULL, alphasort);
    if (count < 0) {
        close(lockfd);
        printf("Storage error\n");
        return 1;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(entries[i]->d_name, ".") && strcmp(entries[i]->d_name, "..") &&
            (old || strcmp(entries[i]->d_name, "_old_"))) {
            if (make_path(path, sizeof(path), r->id, old ? "_old_" : NULL, entries[i]->d_name) &&
                stat(path, &st) == 0 && S_ISREG(st.st_mode))
                printf("%s %ld\n", entries[i]->d_name, (long)st.st_size);
        }
        free(entries[i]);
    }
    free(entries);
    close(lockfd);
    return 0;
}

static int do_download(request_data *r, int old) {
    char dir[MAX_PATH];
    char file_name[MAX_VALUE * 2];
    char path[MAX_PATH];
    char buffer[COPY_BUFFER];
    FILE *f;
    size_t n;
    int lockfd;

    if (!validate_id(r, dir, sizeof(dir))) {
        text_header();
        printf("ID not found\n");
        return 1;
    }
    if (!safe_component(r->name)) {
        text_header();
        printf("File not found\n");
        return 1;
    }
    lockfd = lock_directory(dir, 0);
    if (lockfd < 0) {
        text_header();
        printf("Storage error\n");
        return 1;
    }
    if (old) {
        if (!safe_component(r->date)) {
            close(lockfd);
            text_header();
            printf("File not found\n");
            return 1;
        }
        if (snprintf(file_name, sizeof(file_name), "%s.%s", r->name, r->date) >= (int)sizeof(file_name)) {
            close(lockfd);
            text_header();
            printf("File not found\n");
            return 1;
        }
        if (!make_path(path, sizeof(path), r->id, "_old_", file_name)) {
            close(lockfd);
            return 1;
        }
    } else {
        strcpy(file_name, r->name);
        if (!make_path(path, sizeof(path), r->id, NULL, file_name)) {
            close(lockfd);
            return 1;
        }
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        close(lockfd);
        text_header();
        printf("File not found\n");
        return 1;
    }
    printf("Content-Type: application/octet-stream\r\n");
    printf("Content-Disposition: attachment; filename=\"%s\"\r\n\r\n", file_name);
    for (;;) {
        n = fread(buffer, 1, sizeof(buffer), f);
        if (n == 0) break;
        if (fwrite(buffer, 1, n, stdout) != n) break;
    }
    fclose(f);
    close(lockfd);
    return 0;
}

static int do_delete(request_data *r) {
    char dir[MAX_PATH];
    char current[MAX_PATH];
    char archived[MAX_PATH];
    int lockfd;
    int rc;

    text_header();
    if (!validate_id(r, dir, sizeof(dir))) {
        printf("ID not found\n");
        return 1;
    }
    if (!safe_component(r->name)) {
        printf("File not found\n");
        return 1;
    }
    lockfd = lock_directory(dir, 1);
    if (lockfd < 0) {
        printf("Storage error\n");
        return 1;
    }
    rc = 1;
    if (!make_path(current, sizeof(current), r->id, NULL, r->name)) goto done;
    if (!regular_file_exists(current)) {
        printf("File not found\n");
        goto done;
    }
    if (!archive_path(archived, sizeof(archived), r->id, r->name)) {
        printf("Storage error\n");
        goto done;
    }
    if (rename(current, archived) != 0) {
        printf("Storage error\n");
        goto done;
    }
    printf("File %s deleted\n", r->name);
    rc = 0;
done:
    close(lockfd);
    return rc;
}

int main(void) {
    request_data r;
    char command[MAX_COMMAND];
    const char *method;

    memset(&r, 0, sizeof(r));
    if (!parse_command(command, sizeof(command))) {
        text_header();
        printf("Command not found\n");
        return 0;
    }
    method = getenv("REQUEST_METHOD");
    if (method == NULL || strcmp(method, "POST")) {
        text_header();
        printf("POST required\n");
        return 0;
    }
    if (!parse_multipart(&r)) {
        text_header();
        printf("Invalid request\n");
        if (r.body != NULL) fclose(r.body);
        return 0;
    }
    if (!strcmp(command, "up")) do_upload(&r);
    else if (!strcmp(command, "ls")) do_list(&r, 0);
    else if (!strcmp(command, "lsold")) do_list(&r, 1);
    else if (!strcmp(command, "dw")) do_download(&r, 0);
    else if (!strcmp(command, "dwold")) do_download(&r, 1);
    else if (!strcmp(command, "del")) do_delete(&r);
    else {
        text_header();
        printf("Command not found\n");
    }
    fclose(r.body);
    return 0;
}
