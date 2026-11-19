#include "gateway.h"
#include <mosquitto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sqlite3 *open_store(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc == SQLITE_OK) {
        /* 两个线程各自打开同一数据库文件。遇到锁最多等待 5000 ms；WAL 允许读写并行，写入仍串行。 */
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

/* mid 是本次 MQTT 发布的报文标识，acknowledged 表示收到相应确认；数据库样本另有持久 ID。 */
struct delivery {
    int mid;
    bool acknowledged;
};

/* QoS 1 确认回调：client 为当前客户端，userdata 指向本次连接的 delivery，mid 由库传回。 */
static void on_publish(struct mosquitto *client, void *userdata, int mid) {
    (void)client;
    struct delivery *delivery = userdata;
    if (mid == delivery->mid) delivery->acknowledged = true;
}

/* client：当前 MQTT 客户端；delivery：当前在途消息的确认状态。
 * db：上传线程自己的 SQLite 连接；topic：目标主题。
 * 按样本 ID 从小到大发送。返回 -1 表示数据库或组包错误；
 * 返回 0 表示本轮结束（包括断线、未收到确认或停止），积压记录下次继续处理。
 */
static int drain(struct mosquitto *client, struct delivery *delivery,
                 sqlite3 *db, const char *topic) {
    sqlite3_stmt *next = NULL, *remove = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id,ts,slave,fields FROM outbox ORDER BY id LIMIT 1",
                           -1, &next, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "DELETE FROM outbox WHERE id=?", -1, &remove, NULL) != SQLITE_OK)
        goto database_error;
    while (!stopping()) {
        int rc = sqlite3_step(next);
        /* 查询返回 DONE 表示目前没有待发记录。空闲时仍运行 MQTT 网络循环，处理心跳与断线。 */
        if (rc == SQLITE_DONE) {
            sqlite3_reset(next);
            if (mosquitto_loop(client, 100, 1) != MOSQ_ERR_SUCCESS) break;
            continue;
        }
        if (rc != SQLITE_ROW) goto database_error;
        sqlite3_int64 id = sqlite3_column_int64(next, 0);
        /* 取出时间、从站和测量字段，连同持久 ID 组成 JSON；之后可释放本次查询的读快照。 */
        char *payload = sqlite3_mprintf("{\"id\":%lld,\"ts\":%lld,\"slave\":%d,%s}",
            id, sqlite3_column_int64(next, 1), sqlite3_column_int(next, 2),
            sqlite3_column_text(next, 3));
        sqlite3_reset(next);
        if (!payload) goto database_error;
        delivery->acknowledged = false;
        /* &mid 接收本次报文标识；长度单位为字节，1 表示 QoS 1，false 表示不设置保留消息标志。 */
        rc = mosquitto_publish(client, &delivery->mid, topic, (int)strlen(payload),
                               payload, 1, false);
        sqlite3_free(payload);
        if (rc != MOSQ_ERR_SUCCESS) break;
        /* publish 成功只表示库接受了发送请求。循环处理网络事件，每次最多等待 100 ms，由回调识别 PUBACK。 */
        for (int i = 0; i < 100 && !delivery->acknowledged && !stopping(); ++i)
            if (mosquitto_loop(client, 100, 1) != MOSQ_ERR_SUCCESS) break;
        if (!delivery->acknowledged) break;
        /* 仅确认后按数据库 ID 删除。若确认后、删除前掉电，同一 ID 会再发一次，订阅端可据此去重。 */
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
        /* NULL 让库生成客户端 ID，true 开启干净会话，&delivery 作为 userdata 交给回调。 */
        struct mosquitto *client = mosquitto_new(NULL, true, &delivery);
        if (db && client) {
            mosquitto_publish_callback_set(client, on_publish);
            /* 最后一项 20 是 MQTT 保活间隔（秒）。每轮连接结束后重建客户端，积压记录仍在 SQLite 中。 */
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
