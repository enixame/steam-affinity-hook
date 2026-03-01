CC = gcc
SRC = affinity_hook_fullmask.c

CFLAGS = -shared -fPIC -O2 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS = -ldl -pthread

TARGET64 = libaffinity_hook.so
TARGET32 = libaffinity_hook32.so

all: $(TARGET64) $(TARGET32)

$(TARGET64): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(TARGET32): $(SRC)
	$(CC) -m32 $(CFLAGS) -o $@ $< $(LDFLAGS)

64: $(TARGET64)

32: $(TARGET32)

clean:
	rm -f $(TARGET64) $(TARGET32)

rebuild: clean all

info:
	@echo "Source:   $(SRC)"
	@echo "64-bit:   $(TARGET64)"
	@echo "32-bit:   $(TARGET32)"

.PHONY: all 64 32 clean rebuild info
