// Gianluca Mazzini @2026- Version 1.02

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define TELNET_IAC  255
#define TELNET_DONT 254
#define TELNET_DO   253
#define TELNET_WONT 252
#define TELNET_WILL 251
#define TELNET_SB   250
#define TELNET_SE   240

#define READ_TIMEOUT 15
#define TEXT_SIZE 512

static int send_all(int fd, const unsigned char *buf, size_t len) {
    size_t done;
    ssize_t n;

    done = 0;
    for (; done < len; done += (size_t)n) {
        n = send(fd, buf + done, len - done, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                n = 0;
                continue;
            }
            return -1;
        }
    }
    return 0;
}

static int send_line(int fd, const char *s) {
    size_t len;

    len = strlen(s);
    if (send_all(fd, (const unsigned char *)s, len) < 0)
        return -1;
    return send_all(fd, (const unsigned char *)"\r\n", 2);
}

static int connect_host(const char *host) {
    struct addrinfo hints, *res, *p;
    int fd, rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(host, "23", &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    fd = -1;
    for (p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    if (fd < 0)
        perror("connect");
    return fd;
}

static int contains_ci(const char *text, const char *needle) {
    size_t i, j, lt, ln;

    lt = strlen(text);
    ln = strlen(needle);
    if (ln == 0 || lt < ln)
        return 0;

    for (i = 0; i + ln <= lt; i++) {
        for (j = 0; j < ln; j++) {
            if (tolower((unsigned char)text[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == ln)
            return 1;
    }
    return 0;
}

static void append_text(char *text, size_t *used, unsigned char c) {
    if (c == '\r')
        return;

    if (*used + 1 >= TEXT_SIZE) {
        memmove(text, text + TEXT_SIZE / 2, TEXT_SIZE / 2);
        *used = TEXT_SIZE / 2;
    }

    text[*used] = (char)c;
    (*used)++;
    text[*used] = '\0';
}

static int telnet_reply(int fd, unsigned char verb, unsigned char option) {
    unsigned char reply[3];

    reply[0] = TELNET_IAC;
    reply[1] = verb == TELNET_WILL ? TELNET_DONT : TELNET_WONT;
    reply[2] = option;
    return send_all(fd, reply, sizeof(reply));
}

static int wait_for(int fd, const char *needle, int prompt, int output) {
    unsigned char buf[1024];
    char text[TEXT_SIZE];
    fd_set rfds;
    struct timeval tv;
    size_t used, i;
    ssize_t n;
    int state, command, rc;

    used = 0;
    state = 0;
    command = 0;
    text[0] = '\0';

    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = READ_TIMEOUT;
        tv.tv_usec = 0;

        rc = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rc == 0)
            return 0;
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0)
            return -2;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        for (i = 0; i < (size_t)n; i++) {
            if (state == 0) {
                if (buf[i] == TELNET_IAC) {
                    state = 1;
                    continue;
                }
                append_text(text, &used, buf[i]);
                if (output)
                    putchar(buf[i]);
            } else if (state == 1) {
                if (buf[i] == TELNET_IAC) {
                    append_text(text, &used, buf[i]);
                    if (output)
                        putchar(buf[i]);
                    state = 0;
                } else if (buf[i] == TELNET_WILL || buf[i] == TELNET_DO) {
                    command = buf[i];
                    state = 2;
                } else if (buf[i] == TELNET_WONT || buf[i] == TELNET_DONT) {
                    state = 3;
                } else if (buf[i] == TELNET_SB) {
                    state = 4;
                } else {
                    state = 0;
                }
            } else if (state == 2) {
                if (telnet_reply(fd, (unsigned char)command, buf[i]) < 0)
                    return -1;
                state = 0;
            } else if (state == 3) {
                state = 0;
            } else if (state == 4) {
                if (buf[i] == TELNET_IAC)
                    state = 5;
            } else if (state == 5) {
                if (buf[i] == TELNET_SE)
                    state = 0;
                else if (buf[i] != TELNET_IAC)
                    state = 4;
            }
        }

        if (output)
            fflush(stdout);

        if (prompt) {
            if (used > 0 && text[used - 1] == '>')
                return 1;
            if (used > 1 && text[used - 2] == '>' && text[used - 1] == ' ')
                return 1;
        } else if (contains_ci(text, needle)) {
            return 1;
        }
    }
}

int main(int argc, char **argv) {
    int fd, rc, i;

    if (argc < 5) {
        fprintf(stderr, "usage: %s host user password command [command ...]\n", argv[0]);
        return 1;
    }

    fd = connect_host(argv[1]);
    if (fd < 0)
        return 2;

    rc = wait_for(fd, "login:", 0, 0);
    if (rc == 1) {
        if (send_line(fd, argv[2]) < 0) {
            perror("send login");
            close(fd);
            return 3;
        }
    } else if (rc != 0) {
        fprintf(stderr, "login prompt not received\n");
        close(fd);
        return 3;
    }

    rc = wait_for(fd, "password:", 0, 0);
    if (rc != 1) {
        fprintf(stderr, "password prompt not received\n");
        close(fd);
        return 4;
    }

    if (send_line(fd, argv[3]) < 0) {
        perror("send password");
        close(fd);
        return 5;
    }

    rc = wait_for(fd, NULL, 1, 0);
    if (rc != 1) {
        fprintf(stderr, "router prompt not received\n");
        close(fd);
        return 6;
    }

    for (i = 4; i < argc; i++) {
        if (send_line(fd, argv[i]) < 0) {
            perror("send command");
            close(fd);
            return 7;
        }

        rc = wait_for(fd, NULL, 1, 1);
        if (rc == 1)
            continue;
        if (rc == -2 && i == argc - 1 && strcmp(argv[i], "/quit") == 0) {
            close(fd);
            return 0;
        }

        if (rc == 0)
            fprintf(stderr, "timeout waiting for router prompt after command %d\n", i - 3);
        else if (rc == -2)
            fprintf(stderr, "connection closed after command %d\n", i - 3);
        else
            perror("receive");
        close(fd);
        return 8;
    }

    close(fd);
    return 0;
}
