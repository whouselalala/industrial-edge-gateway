"""Run from the repository root after make -C linux. Uses a local MQTT broker."""
import json
import os
from pathlib import Path
import pty
import select
import socket
import sqlite3
import struct
import subprocess
import tempfile
import threading
import time

from gateway.modbus import add_crc, build_read


def until(predicate, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(f"timeout waiting for {predicate}")


def simulate(master, stop):
    buffer = bytearray()
    bad_light_crc = True
    while not stop.is_set():
        if not select.select([master], [], [], 0.1)[0]:
            continue
        buffer.extend(os.read(master, 256))
        while len(buffer) >= 8:
            request = bytes(buffer[:8])
            del buffer[:8]
            slave = request[0]
            assert slave in (1, 2) and request == build_read(slave, slave, 2)
            data = struct.pack(">hH", -123, 567) if slave == 1 else struct.pack(">I", 123456)
            frame = add_crc(bytes((slave, 3, 4)) + data)
            if slave == 2 and bad_light_crc:
                frame = frame[:-1] + bytes([frame[-1] ^ 1])
                bad_light_crc = False
            os.write(master, frame)


def check(root):
    master, terminal = pty.openpty()
    stop = threading.Event()
    simulator = threading.Thread(target=simulate, args=(master, stop))
    simulator.start()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = str(sock.getsockname()[1])
    database = root / "cache.sqlite3"
    command = ["linux/edge-gateway", os.ttyname(terminal), database, "127.0.0.1", port, "test/sensor"]
    children = []
    def launch(args, **options):
        child = subprocess.Popen(args, **options)
        children.append(child)
        return child

    def rows():
        if not database.exists():
            return []
        with sqlite3.connect(database) as db:
            if not db.execute("SELECT name FROM sqlite_master WHERE name='outbox'").fetchone():
                return []
            return db.execute("SELECT id,slave FROM outbox ORDER BY id").fetchall()

    try:
        gateway = launch(command)
        until(lambda: len(rows()) >= 3)
        gateway.terminate()
        assert gateway.wait(timeout=5) == 0
        saved = rows()
        assert [node for _, node in saved[:3]] == [1, 1, 2]
        log = root / "broker.log"
        with log.open("w") as broker_log:
            broker = launch(["mosquitto", "-p", port, "-v"], stderr=broker_log)
            until(lambda: " running" in log.read_text())
            subscriber = launch(["mosquitto_sub", "-h", "127.0.0.1", "-p", port, "-q", "1",
                                 "-t", "test/sensor", "-C", str(len(saved))], stdout=subprocess.PIPE)
            until(lambda: "Sending SUBACK" in log.read_text())
            gateway = launch(command)
            output, _ = subscriber.communicate(timeout=15)
            received = [json.loads(line) for line in output.splitlines()]
            assert [(r["id"], r["slave"]) for r in received] == saved
            assert received[0]["temperature_c"] == -12.3 and received[0]["humidity_pct"] == 56.7
            assert received[2]["illuminance_lux"] == 123.456
            until(lambda: not rows())
            broker.terminate()
            broker.wait(timeout=5)
            until(lambda: len(rows()) >= 2)
            last_id = rows()[-1][0]
            launch(["mosquitto", "-p", port], stderr=broker_log)
            until(lambda: all(row[0] > last_id for row in rows()))
            assert gateway.poll() is None
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=8)
        stop.set()
        simulator.join()
        os.close(master)
        os.close(terminal)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as path:
        check(Path(path))
    print("PASS: dual RTU polling, CRC rejection, offline restart, QoS 1 replay and reconnect")
