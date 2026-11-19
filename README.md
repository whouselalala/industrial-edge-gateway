# 基于 Linux 的工业物联网边缘网关

Linux 主机通过 USB-RS485 轮询温湿度和光照从站。C 程序将每次成功读取的样本写入 SQLite，独立线程通过 MQTT QoS 1 上传。适合在 Linux PC、树莓派或带 USB 主机接口的 ARM Linux 板上运行；目前验证平台为 x86-64 Linux。

## 接线与寄存器

USB-RS485 适配器选自动收发方向型号，使用 9600 baud、8N1。可连接两个协议兼容的商用传感器，也可以用 STM32 作为采集节点，在采集任务中读取传感器并更新下表寄存器。仓库含 STM32 从站协议核心与移植说明，板级采样和接线仍需按具体硬件完成。

| 从站地址 | 功能码 | 起始地址（线上零基地址） | 两个 16 位寄存器的含义 |
|---|---|---|---|
| 1 | 0x03 | 0x0001 | 有符号温度 ×10、无符号相对湿度 ×10 |
| 2 | 0x03 | 0x0002 | 光照 ×1000，高 16 位在前，换算为 lux |

任一节点超时或 CRC 错误时跳过该样本，继续轮询另一节点。两站都失败时重新打开串口。每轮休眠 2 秒。两个从站分别发布样本，避免离线设备的旧值混入当前读数。

## 编译运行

在 Debian / Ubuntu 上，从仓库根目录执行。示例将 broker 与网关部署在同一台机器。

```sh
sudo apt install build-essential libmodbus-dev libsqlite3-dev libmosquitto-dev mosquitto mosquitto-clients
make -C linux
sudo systemctl start mosquitto
mosquitto_sub -h 127.0.0.1 -q 1 -t lab/gateway/telemetry -v
```

另开终端启动网关，当前账号需有串口读写权限。五个参数依次为串口、数据库文件、MQTT 主机、端口、主题。

```sh
./linux/edge-gateway /dev/ttyUSB0 outbox.sqlite3 127.0.0.1 1883 lab/gateway/telemetry
```

消息示例：

```json
{"id":1,"ts":1700000000,"slave":1,"temperature_c":-12.3,"humidity_pct":56.7}
{"id":2,"ts":1700000000,"slave":2,"illuminance_lux":123.456}
```

## 离线缓存与服务管理

采集线程与上传线程各用一个 SQLite 连接，通过 WAL 模式并发访问同一数据库。发布线程按 ID 顺序发送，每次仅保留一条在途消息，收到对应 PUBACK 后删除记录。连接中断或进程重启后继续读取未确认记录。若 broker 已接收而本地删除前掉电，重发可能产生重复，订阅端可按网关标识（部署时分配独立主题）和样本 ID 去重。当前 C 版面向本地或受控网络的匿名 MQTT broker，缓存容量取决于可用磁盘空间。

在使用 systemd 和 dialout 组的设备上部署：

```sh
sudo install -m 755 linux/edge-gateway /usr/local/bin/
sudo install -m 644 deploy/edge-gateway.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now edge-gateway
journalctl -u edge-gateway -f
```

服务默认使用 /dev/ttyUSB0 和本机 broker，数据库位于 /var/lib/edge-gateway/outbox.sqlite3。可在 /etc/default/edge-gateway 设置 SERIAL、MQTT_HOST、MQTT_PORT、MQTT_TOPIC 后重启服务。串口建议使用 /dev/serial/by-id/ 下的稳定名称。

## 验证与参考

以下集成测试用 Linux 伪终端模拟两个 Modbus 从站，自动启动本地 Mosquitto，检查寄存器解码、坏 CRC 丢弃、离线采集、进程重启后的补传，以及 broker 中断后的自动恢复。需先安装上述依赖和 Python 3。

```sh
make -C linux
PYTHONPATH=. python3 tests/linux/e2e.py
python3 -m unittest discover -s tests -v
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/core firmware/core/modbus_slave.c tests/test_modbus_slave.c -o /tmp/test-slave && /tmp/test-slave
```

功能和寄存器映射参考 [zgd-dev 的工业边缘网关 V2.0](https://github.com/zgd-dev/-V2.0/tree/40b4460e1f576e0f8223338f06698bc6bdd1bf90)。Linux 端独立实现双从站采集、缓存、重连与日志；服务托管使用 systemd。原项目的 LVGL 触摸屏、息屏和 MCU 硬件看门狗未移植。

[STM32 从站移植说明](firmware/stm32/README.md) · [原 Python 网关运行说明](gateway/README.md)。旧版的 TOML 配置、数据库与启动命令仍可使用。C 版使用独立的 outbox 表；从 Python 版切换前，应先用旧程序发完旧库中的积压记录。当前尚未做 ARM 板、STM32 真板或物理 RS485 总线联调。
