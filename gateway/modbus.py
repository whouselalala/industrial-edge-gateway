"""Small Modbus RTU master for function 03 (read holding registers)."""


class ModbusError(Exception):
    pass


def crc16(data: bytes) -> int:
    value = 0xFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
    return value


def add_crc(payload: bytes) -> bytes:
    return payload + crc16(payload).to_bytes(2, "little")


def build_read(slave: int, start: int, count: int) -> bytes:
    if not 1 <= slave <= 247 or not 0 <= start <= 65535 or not 1 <= count <= 4 or start + count > 65536:
        raise ValueError("invalid Modbus address or register range")
    return add_crc(bytes((slave, 3)) + start.to_bytes(2, "big") + count.to_bytes(2, "big"))


def parse_read(frame: bytes, slave: int, count: int) -> list[int]:
    if len(frame) < 5 or crc16(frame) != 0:
        raise ModbusError("truncated frame or CRC mismatch")
    if frame[0] != slave:
        raise ModbusError("unexpected slave address")
    if frame[1] == 0x83 and len(frame) == 5:
        raise ModbusError(f"device exception {frame[2]}")
    if frame[1] != 3 or frame[2] != count * 2 or len(frame) != 5 + count * 2:
        raise ModbusError("unexpected function or byte count")
    return [int.from_bytes(frame[3 + 2 * i:5 + 2 * i], "big") for i in range(count)]


def read_holding(serial_port, slave: int, start: int, count: int) -> list[int]:
    request = build_read(slave, start, count)
    serial_port.reset_input_buffer()
    serial_port.write(request)
    header = serial_port.read(3)
    if len(header) != 3:
        raise ModbusError("response timeout")
    if header[1] == 0x83:
        rest = serial_port.read(2)
    elif header[1] == 3 and header[2] <= 8:
        rest = serial_port.read(header[2] + 2)
    else:
        raise ModbusError("invalid response header")
    return parse_read(header + rest, slave, count)
