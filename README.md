# 工业边缘网关（可运行原型）

Linux 网关通过 USB-RS485 定时读取 Modbus RTU 从站保持寄存器，先写入 SQLite，再以 MQTT QoS 1 发布。离线期间记录保留；收到 MQTT PUBACK 后才标记为已发送。附带一个可在 Linux 主机编译测试的 STM32 Modbus 从站协议核心，以及 STM32CubeIDE 移植说明。

> 当前版本：网关与协议核心已在 Linux 主机测试；STM32 板级驱动、RS485 实机联调、MQTT 断网实测仍需按具体开发板完成。不要将本仓库描述为已完成实机验证。

## 结构

```
gateway/         Python Linux 网关（串口、SQLite、MQTT）
firmware/core/   纯 C Modbus RTU 从站核心，支持 03 功能码
firmware/stm32/  STM32 移植接口与步骤
tests/           协议和队列测试
```

## 运行

需要 Python 3.11+。在 Linux 主机安装依赖，并启动本地 MQTT broker（例如 Mosquitto）：

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
cp config.example.toml config.toml
# 修改 config.toml 中串口路径与 MQTT 地址
python -m gateway.main --config config.toml
```

连接 RS485 温湿度变送器，示例默认从站地址 1、波特率 9600、8N1，从保持寄存器 0 读两个寄存器（有符号温度 ×10、无符号湿度 ×10）。具体寄存器地址、字节序与数值单位应以设备说明书为准；**不要将 RS485 A/B 直接连接 MCU UART 引脚**，需使用收发器。MQTT topic: `lab/gateway/telemetry`。可用 `mosquitto_sub -h 127.0.0.1 -t 'lab/gateway/telemetry' -v` 查看消息。生产部署应配置 MQTT TLS、认证、磁盘容量上限和监控。

运行测试：

```sh
python -m unittest discover -s tests -v
gcc -std=c11 -Wall -Wextra -Werror -Ifirmware/core firmware/core/modbus_slave.c tests/test_modbus_slave.c -o /tmp/test_modbus_slave
/tmp/test_modbus_slave
```

## 行为边界

- Modbus 只支持读保持寄存器（功能码 0x03），最大读取 4 个寄存器；UART 接收端须以 RTU 帧间静默时间或 IDLE+定时器判定帧结束。
- SQLite 使用自增 ID 追踪；崩溃发生在服务端收到消息、客户端收到 PUBACK 之前时，可能重复发布，消费者应按 `id` 幂等处理。
- 串口故障不会使已存数据丢失；MQTT 故障时积压数据保留，但数据库容量没有自动限额。
- 参考协议实现：[alejoseb/Modbus-STM32-HAL-FreeRTOS](https://github.com/alejoseb/Modbus-STM32-HAL-FreeRTOS)（LGPL-2.1）；本仓库代码为独立实现，没有复制其源码。

## 下一步实机工作

选定 STM32 型号并在 CubeMX 配置 FreeRTOS、RS485 UART/DE、传感器；移植 `firmware/core`；使用逻辑分析仪和 Modbus 主站测试正常读、CRC 错误、超时、重试、断电恢复。记录设备型号、接线图、串口日志与实测数据后再写简历结果。
