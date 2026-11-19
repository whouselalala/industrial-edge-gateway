#pragma once
#include <sqlite3.h>
#include <stdbool.h>

bool stopping(void);
void pause_ms(unsigned ms);
sqlite3 *open_store(const char *path);
struct publisher_config {
    const char *database, *host, *topic;
    int port;
};
void *publish_loop(void *argument);
