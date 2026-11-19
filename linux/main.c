#include "gateway.h"
#include <errno.h>
#include <modbus/modbus.h>
#include <mosquitto.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile sig_atomic_t stop_requested;
static void on_signal(int sig) { (void)sig; stop_requested = 1; }
bool stopping(void) { return stop_requested != 0; }

void pause_ms(unsigned ms) {
    while (ms && !stopping()) {
        unsigned step = ms < 100 ? ms : 100;
        struct timespec delay = {0, (long)step * 1000000L};
        nanosleep(&delay, NULL);
        ms -= step;
    }
}

static int sample(modbus_t *bus, sqlite3 *db, int slave, int start) {
    uint16_t words[2];
    if (modbus_set_slave(bus, slave) < 0 ||
        modbus_read_registers(bus, start, 2, words) != 2) {
        fprintf(stderr, "slave %d: %s\n", slave, modbus_strerror(errno));
        modbus_flush(bus);
        return -1;
    }
    char fields[128];
    if (slave == 1) {
        int temperature = words[0] > 32767 ? (int)words[0] - 65536 : words[0];
        snprintf(fields, sizeof fields, "\"temperature_c\":%.1f,\"humidity_pct\":%.1f",
                 temperature / 10.0, words[1] / 10.0);
    } else {
        uint32_t light = ((uint32_t)words[0] << 16) | words[1];
        snprintf(fields, sizeof fields, "\"illuminance_lux\":%.3f", light / 1000.0);
    }
    sqlite3_stmt *insert = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO outbox(ts,slave,fields) VALUES(?,?,?)",
                                -1, &insert, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(insert, 1, time(NULL));
        sqlite3_bind_int(insert, 2, slave);
        sqlite3_bind_text(insert, 3, fields, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(insert);
    }
    sqlite3_finalize(insert);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "store sample: %s\n", sqlite3_errmsg(db));
        return -2;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s SERIAL DATABASE MQTT_HOST MQTT_PORT TOPIC\n", argv[0]);
        return 2;
    }
    char *end;
    long port = strtol(argv[4], &end, 10);
    if (*end || port < 1 || port > 65535 || mosquitto_pub_topic_check(argv[5]) != MOSQ_ERR_SUCCESS)
        return 2;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    sqlite3 *db = open_store(argv[2]);
    if (!db) return 1;
    modbus_t *bus = modbus_new_rtu(argv[1], 9600, 'N', 8, 1);
    if (!bus) { sqlite3_close(db); return 1; }
    modbus_set_response_timeout(bus, 0, 400000);
    modbus_set_byte_timeout(bus, 0, 100000);
    struct publisher_config cfg = {argv[2], argv[3], argv[5], (int)port};
    pthread_t publisher;
    if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS ||
        pthread_create(&publisher, NULL, publish_loop, &cfg) != 0) {
        fprintf(stderr, "cannot start MQTT worker\n");
        modbus_free(bus);
        sqlite3_close(db);
        return 1;
    }
    int status = 0;
    while (!stopping()) {
        if (modbus_connect(bus) < 0) {
            fprintf(stderr, "open %s: %s\n", argv[1], modbus_strerror(errno));
            pause_ms(2000);
            continue;
        }
        while (!stopping()) {
            int first = sample(bus, db, 1, 1);
            pause_ms(30);
            if (stopping()) break;
            int second = sample(bus, db, 2, 2);
            if (first == -2 || second == -2) {
                status = 1;
                stop_requested = 1;
                break;
            }
            pause_ms(2000);
            if (first < 0 && second < 0) break;
        }
        modbus_close(bus);
    }
    pthread_join(publisher, NULL);
    mosquitto_lib_cleanup();
    modbus_free(bus);
    sqlite3_close(db);
    return status;
}
