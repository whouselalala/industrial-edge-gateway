"""Durable outbox: mark rows sent only after broker acknowledgment."""

import sqlite3
import time


class Store:
    def __init__(self, path: str):
        self.db = sqlite3.connect(path)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("CREATE TABLE IF NOT EXISTS samples (id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, slave INTEGER NOT NULL, temperature REAL NOT NULL, humidity REAL NOT NULL, sent INTEGER NOT NULL DEFAULT 0)")
        self.db.commit()

    def add(self, slave: int, temperature: float, humidity: float) -> int:
        with self.db:
            cursor = self.db.execute("INSERT INTO samples(ts, slave, temperature, humidity) VALUES(?,?,?,?)", (int(time.time()), slave, temperature, humidity))
        return cursor.lastrowid

    def pending(self, limit: int = 100) -> list[tuple]:
        return self.db.execute("SELECT id, ts, slave, temperature, humidity FROM samples WHERE sent=0 ORDER BY id LIMIT ?", (limit,)).fetchall()

    def mark_sent(self, sample_id: int) -> None:
        with self.db:
            self.db.execute("UPDATE samples SET sent=1 WHERE id=?", (sample_id,))

    def close(self) -> None:
        self.db.close()
