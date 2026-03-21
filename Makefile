CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
          $(shell pkg-config --cflags libwebsockets)
LDFLAGS = $(shell pkg-config --libs libwebsockets) -lpthread

SRCS = src/main.c src/evdev.c src/uinput.c src/net.c
OBJS = $(SRCS:.c=.o)
BIN  = coop-bridge

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)

install: $(BIN)
	install -Dm755 $(BIN) /usr/local/bin/$(BIN)
	install -Dm644 99-uinput.rules /etc/udev/rules.d/99-uinput.rules
	udevadm control --reload-rules && udevadm trigger

.PHONY: all clean install
