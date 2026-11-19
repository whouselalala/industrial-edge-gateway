CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDLIBS += -lmodbus -lmosquitto -lsqlite3 -pthread

edge-gateway: main.c publisher.c gateway.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -pthread main.c publisher.c $(LDFLAGS) $(LDLIBS) -o $@

clean:
	$(RM) edge-gateway
.PHONY: clean
