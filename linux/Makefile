CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
# 链接 Modbus 串口库、MQTT 客户端库、SQLite 和 POSIX 线程支持。
LDLIBS += -lmodbus -lmosquitto -lsqlite3 -pthread

# 从仓库根目录运行 make -C linux。下方 $@ 是输出文件名 edge-gateway。
edge-gateway: main.c publisher.c gateway.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -pthread main.c publisher.c $(LDFLAGS) $(LDLIBS) -o $@

clean:
	$(RM) edge-gateway
.PHONY: clean
