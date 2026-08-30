CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu89
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

TARGET = runtelnet
SRC = runtelnet.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
