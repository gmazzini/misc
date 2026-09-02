CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu89
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

TARGETS = runtelnet file

all: $(TARGETS)

runtelnet: runtelnet.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

file: file.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
