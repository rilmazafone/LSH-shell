CC      = clang
CFLAGS  = -Wall -Wextra -std=c11
LDFLAGS = 

TARGET  = shell
SRCS    = main.c syscalls.S
OBJS    = main.o syscalls.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

syscalls.o: syscalls.S
	$(CC) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
