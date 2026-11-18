"""Single-threaded poll and outbox publisher; intentionally straightforward."""

import argparse
import json
import logging
import time
import tomllib

import paho.mqtt.client as mqtt
import serial

from .modbus import ModbusError, read_holding
from .store import Store


def signed16(value: int) -> int:
    return value - 65536 if value & 0x8000 else value


def publish_pending(client, store: Store, topic: str) -> None:
    for sample_id, ts, slave, temperature, humidity in store.pending():
        payload = json.dumps({"id": sample_id, "ts": ts, "slave": slave, "temperature_c": temperature, "humidity_pct": humidity}, separators=(",", ":"))
        info = client.publish(topic, payload, qos=1)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            break
        info.wait_for_publish(timeout=3)
        if not info.is_published():
            break
        store.mark_sent(sample_id)


def run(config: dict) -> None:
    mb, mq = config["modbus"], config["mqtt"]
    store = Store(config["storage"]["path"])
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=mq["client_id"])
    if mq.get("username"):
        client.username_pw_set(mq["username"], mq.get("password"))
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    client.connect_async(mq["host"], mq["port"], keepalive=30)
    client.loop_start()
    try:
        with serial.Serial(mb["port"], mb["baudrate"], timeout=mb["timeout_seconds"], parity=serial.PARITY_NONE, stopbits=1) as port:
            while True:
                try:
                    values = read_holding(port, mb["slave_id"], mb["start_register"], mb["count"])
                    if len(values) < 2:
                        raise ModbusError("need two measurement registers")
                    sample_id = store.add(mb["slave_id"], signed16(values[0]) / 10, values[1] / 10)
                    logging.info("stored sample %s", sample_id)
                except (ModbusError, serial.SerialException) as exc:
                    logging.warning("Modbus poll failed: %s", exc)
                try:
                    if client.is_connected():
                        publish_pending(client, store, mq["topic"])
                except (OSError, RuntimeError) as exc:
                    logging.warning("publish failed: %s", exc)
                time.sleep(mb["poll_seconds"])
    finally:
        client.disconnect()
        client.loop_stop()
        store.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", default="config.toml")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    with open(args.config, "rb") as file:
        run(tomllib.load(file))
