CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu89
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

TARGETS = runtelnet file photo redirect smsauth

all: $(TARGETS)

runtelnet: runtelnet.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

file: file.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

photo: photo.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lsqlite3

redirect: redirect.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lsqlite3 -lcurl -lcrypto

smsauth: smsauth.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS) -lsqlite3 -lcurl -lcrypto

clean:
	rm -f $(TARGETS)

.PHONY: all clean
