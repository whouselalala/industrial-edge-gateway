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

/* 收到 Ctrl+C 或服务停止信号后置 1；两个循环看到标志后自行退出。 */
static volatile sig_atomic_t stop_requested;
/* sig 是 Linux 传入的信号编号。这里对 SIGINT 和 SIGTERM 做相同处理。 */
static void on_signal(int sig) { (void)sig; stop_requested = 1; }
bool stopping(void) { return stop_requested != 0; }

void pause_ms(unsigned ms) {
    while (ms && !stopping()) {
        /* ms 为剩余毫秒数；每段最多睡 100 ms，段间检查退出标志。timespec 的第二项以纳秒计。 */
        unsigned step = ms < 100 ? ms : 100;
        struct timespec delay = {0, (long)step * 1000000L};
        nanosleep(&delay, NULL);
        ms -= step;
    }
}

/* 读取一台设备，并把一条有效样本写入待发送队列表 outbox。
 * bus：已打开的 RTU 串口连接，由采集线程独占。
 * db：采集线程的数据库连接。slave：总线上的设备地址，1 为温湿度，2 为光照。
 * start：设备内部第一个保持寄存器的地址。设备地址和寄存器地址属于不同层次。
 * 返回 0 表示已入库，-1 表示本次通信失败，-2 表示数据库写入失败。
 */
static int sample(modbus_t *bus, sqlite3 *db, int slave, int start) {
    uint16_t words[2];
    /* 先选从站，再从 start 连读 2 个寄存器到 words。读函数返回实际读到的寄存器个数。 */
    if (modbus_set_slave(bus, slave) < 0 ||
        modbus_read_registers(bus, start, 2, words) != 2) {
        fprintf(stderr, "slave %d: %s\n", slave, modbus_strerror(errno));
        /* 清除串口缓冲区里的残留数据，避免影响下一次请求。 */
        modbus_flush(bus);
        return -1;
    }
    char fields[128];
    if (slave == 1) {
        /* 原始温度是 16 位补码，例如 0xFF85 表示 -123，除以 10 得 -12.3 °C。 */
        int temperature = words[0] > 32767 ? (int)words[0] - 65536 : words[0];
        snprintf(fields, sizeof fields, "\"temperature_c\":%.1f,\"humidity_pct\":%.1f",
                 temperature / 10.0, words[1] / 10.0);
    } else {
        /* 高字在 words[0]，低字在 words[1]。先扩成 32 位再左移，合并后除以 1000 得 lux。 */
        uint32_t light = ((uint32_t)words[0] << 16) | words[1];
        snprintf(fields, sizeof fields, "\"illuminance_lux\":%.3f", light / 1000.0);
    }
    sqlite3_stmt *insert = NULL;
    /* 三个问号按顺序绑定采集时间（Unix 秒）、从站地址和测量字段，编号从 1 开始。 */
    int rc = sqlite3_prepare_v2(db, "INSERT INTO outbox(ts,slave,fields) VALUES(?,?,?)",
                                -1, &insert, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(insert, 1, time(NULL));
        sqlite3_bind_int(insert, 2, slave);
        /* -1 表示字符串以空字符结束；SQLITE_TRANSIENT 让 SQLite 复制局部数组 fields。 */
        sqlite3_bind_text(insert, 3, fields, -1, SQLITE_TRANSIENT);
        /* step 执行 INSERT；只有写入成功，这条采样才交给上传线程。 */
        rc = sqlite3_step(insert);
    }
    sqlite3_finalize(insert);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "store sample: %s\n", sqlite3_errmsg(db));
        return -2;
    }
    return 0;
}

/* 启动示例（在仓库根目录运行）：
 * ./linux/edge-gateway /dev/ttyUSB0 outbox.sqlite3 127.0.0.1 1883 lab/gateway/telemetry
 * argc 包含程序名，共 6 项。argv[0] 是程序名，后面五项依次为：
 * [1] 串口设备路径  [2] SQLite 文件路径  [3] MQTT 服务器地址
 * [4] MQTT 端口（十进制）  [5] 发布主题。
 * 退出码：0 正常停止，1 初始化或存储失败，2 启动参数错误。
 */
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
    /* 依次传入串口路径、波特率、校验方式、数据位、停止位。此处创建配置，稍后 connect 才打开串口。 */
    modbus_t *bus = modbus_new_rtu(argv[1], 9600, 'N', 8, 1);
    if (!bus) { sqlite3_close(db); return 1; }
    /* 后两个参数是秒、微秒：最多等 400 ms 收到首字节，随后相邻字节最多间隔 100 ms。 */
    modbus_set_response_timeout(bus, 0, 400000);
    modbus_set_byte_timeout(bus, 0, 100000);
    struct publisher_config cfg = {argv[2], argv[3], argv[5], (int)port};
    /* pthread_create 的四项是线程 ID 输出、默认属性、入口函数和传给入口的 &cfg。主线程继续采集。 */
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
        /* 外层负责打开或重新打开串口；例如 USB 转接器暂时拔掉时，每隔 2 秒重试。 */
        if (modbus_connect(bus) < 0) {
            fprintf(stderr, "open %s: %s\n", argv[1], modbus_strerror(errno));
            pause_ms(2000);
            continue;
        }
        while (!stopping()) {
            /* sample(bus, db, 1, 1)：向 1 号节点读地址 1、2。间隔 30 ms 后向 2 号节点读地址 2、3。 */
            int first = sample(bus, db, 1, 1);
            pause_ms(30);
            if (stopping()) break;
            int second = sample(bus, db, 2, 2);
            /* 写库失败则退出并报告错误；单台通信失败只跳过它，两台都通信失败才重开串口。 */
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
    /* 等上传线程结束后再清理 MQTT 库；cfg 在 main 返回前一直有效。 */
    pthread_join(publisher, NULL);
    mosquitto_lib_cleanup();
    modbus_free(bus);
    sqlite3_close(db);
    return status;
}
