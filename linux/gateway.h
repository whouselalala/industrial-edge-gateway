#pragma once
#include <sqlite3.h>
#include <stdbool.h>

/* 返回 true 表示已请求停止，供两个线程检查。 */
bool stopping(void);
/* ms 为期望等待的毫秒数，收到停止请求可提前返回。 */
void pause_ms(unsigned ms);
/* path 是数据库文件路径。成功返回调用线程专用的连接（用 sqlite3_close 关闭），失败返回 NULL。 */
sqlite3 *open_store(const char *path);
struct publisher_config {
    /* database：SQLite 文件路径；host：MQTT 服务器域名或 IP；topic：发布主题。 */
    const char *database, *host, *topic;
    /* MQTT 服务器的 TCP 端口，示例使用 1883。 */
    int port;
};
/* pthread 入口。argument 指向 main 中的 publisher_config，线程退出时返回 NULL。 */
void *publish_loop(void *argument);
