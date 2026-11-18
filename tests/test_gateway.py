import tempfile
import unittest
from pathlib import Path

from gateway.modbus import ModbusError, add_crc, build_read, parse_read, read_holding
from gateway.store import Store


class FakeSerial:
    def __init__(self, response):
        self.response = response

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.request = data

    def read(self, n):
        data, self.response = self.response[:n], self.response[n:]
        return data


class GatewayTest(unittest.TestCase):
    def test_modbus_read_and_errors(self):
        response = add_crc(bytes((1, 3, 4, 0, 253, 2, 106)))
        port = FakeSerial(response)
        self.assertEqual(read_holding(port, 1, 0, 2), [253, 618])
        self.assertEqual(port.request, build_read(1, 0, 2))
        with self.assertRaises(ModbusError):
            parse_read(response[:-1] + b"\x00", 1, 2)
        with self.assertRaises(ModbusError):
            read_holding(FakeSerial(b""), 1, 0, 2)
        with self.assertRaises(ModbusError):
            parse_read(add_crc(bytes((1, 0x83, 2))), 1, 2)

    def test_store_reopens_with_unsent_records(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "gateway.db")
            first = Store(path)
            row = first.add(1, 25.3, 61.8)
            first.close()
            second = Store(path)
            self.assertEqual([item[0] for item in second.pending()], [row])
            second.mark_sent(row)
            self.assertEqual(second.pending(), [])
            second.close()


if __name__ == "__main__":
    unittest.main()
