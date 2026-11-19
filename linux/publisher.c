#include "gateway.h"
#include <mosquitto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sqlite3 *open_store(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc == SQLITE_OK) {
        sqlite3_busy_timeout(db, 5000);
        rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL;"
            "CREATE TABLE IF NOT EXISTS outbox("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,ts INTEGER NOT NULL,"
            "slave INTEGER NOT NULL,fields TEXT NOT NULL)", NULL, NULL, NULL);
    }
    if (rc == SQLITE_OK) return db;
    fprintf(stderr, "open outbox: %s\n", sqlite3_errmsg(db));
    sqlite3_close(db);
    return NULL;
}

struct delivery {
    int mid;
    bool acknowledged;
};

static void on_publish(struct mosquitto *client, void *userdata, int mid) {
    (void)client;
    struct delivery *delivery = userdata;
    if (mid == delivery->mid) delivery->acknowledged = true;
}

static int drain(struct mosquitto *client, struct delivery *delivery,
                 sqlite3 *db, const char *topic) {
    sqlite3_stmt *next = NULL, *remove = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id,ts,slave,fields FROM outbox ORDER BY id LIMIT 1",
                           -1, &next, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "DELETE FROM outbox WHERE id=?", -1, &remove, NULL) != SQLITE_OK)
        goto database_error;
    while (!stopping()) {
        int rc = sqlite3_step(next);
        if (rc == SQLITE_DONE) {
            sqlite3_reset(next);
            if (mosquitto_loop(client, 100, 1) != MOSQ_ERR_SUCCESS) break;
            continue;
        }
        if (rc != SQLITE_ROW) goto database_error;
        sqlite3_int64 id = sqlite3_column_int64(next, 0);
        char *payload = sqlite3_mprintf("{\"id\":%lld,\"ts\":%lld,\"slave\":%d,%s}",
            id, sqlite3_column_int64(next, 1), sqlite3_column_int(next, 2),
            sqlite3_column_text(next, 3));
        sqlite3_reset(next);
        if (!payload) goto database_error;
        delivery->acknowledged = false;
        rc = mosquitto_publish(client, &delivery->mid, topic, (int)strlen(payload),
                               payload, 1, false);
        sqlite3_free(payload);
        if (rc != MOSQ_ERR_SUCCESS) break;
        for (int i = 0; i < 100 && !delivery->acknowledged && !stopping(); ++i)
            if (mosquitto_loop(client, 100, 1) != MOSQ_ERR_SUCCESS) break;
        if (!delivery->acknowledged) break;
        sqlite3_bind_int64(remove, 1, id);
        if (sqlite3_step(remove) != SQLITE_DONE) goto database_error;
        sqlite3_reset(remove);
    }
    sqlite3_finalize(next);
    sqlite3_finalize(remove);
    return 0;
database_error:
    fprintf(stderr, "read/update outbox: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(next);
    sqlite3_finalize(remove);
    return -1;
}

void *publish_loop(void *argument) {
    struct publisher_config *cfg = argument;
    sqlite3 *db = NULL;
    while (!stopping()) {
        if (!db) db = open_store(cfg->database);
        struct delivery delivery = {0};
        struct mosquitto *client = mosquitto_new(NULL, true, &delivery);
        if (db && client) {
            mosquitto_publish_callback_set(client, on_publish);
            int rc = mosquitto_connect(client, cfg->host, cfg->port, 20);
            if (rc == MOSQ_ERR_SUCCESS) drain(client, &delivery, db, cfg->topic);
            if (!stopping()) fprintf(stderr, "MQTT unavailable, retrying\n");
        }
        if (client) mosquitto_destroy(client);
        pause_ms(2000);
    }
    sqlite3_close(db);
    return NULL;
}
