CC      = clang
CFLAGS  = -Wall -Wextra -std=c11 -O2
ASFLAGS = -Wall
LDFLAGS =

TARGET  = shell
OBJS    = main.o syscalls.o

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifneq ($(UNAME_S),Darwin)
$(error LSH only builds on macOS: syscalls.S uses raw Darwin arm64 syscalls)
endif
ifneq ($(UNAME_M),arm64)
$(error LSH only builds on Apple silicon: syscalls.S uses raw arm64 syscalls (found $(UNAME_M)))
endif

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

main.o: main.c syscalls.h
	$(CC) $(CFLAGS) -c $< -o $@

syscalls.o: syscalls.S
	$(CC) $(ASFLAGS) -c $< -o $@

asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -g -fsanitize=address" LDFLAGS="-fsanitize=address" all

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all asan clean
