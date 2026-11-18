#include "modbus_slave.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void set_crc(uint8_t *data, size_t count) {
    const uint16_t crc = mb_crc16(data, count - 2u);
    data[count - 2u] = (uint8_t)crc;
    data[count - 1u] = (uint8_t)(crc >> 8u);
}

int main(void) {
    mb_slave_t slave = {1u, {253u, 618u, 0u, 0u}};
    uint8_t req[8] = {1u, 3u, 0u, 0u, 0u, 2u, 0u, 0u};
    uint8_t reply[MB_MAX_FRAME] = {0};
    set_crc(req, sizeof req);
    size_t n = mb_handle(&slave, req, sizeof req, reply, sizeof reply);
    assert(n == 9u && mb_crc16(reply, n) == 0u);
    assert(reply[3] == 0u && reply[4] == 253u && reply[5] == 2u && reply[6] == 106u);
    req[7] ^= 0x80u;
    assert(mb_handle(&slave, req, sizeof req, reply, sizeof reply) == 0u);
    req[7] ^= 0x80u;
    req[4] = 0u; req[5] = 5u; set_crc(req, sizeof req);
    n = mb_handle(&slave, req, sizeof req, reply, sizeof reply);
    assert(n == 5u && reply[1] == 0x83u && reply[2] == 2u);
    assert(mb_handle(&slave, req, sizeof req, reply, 4u) == 0u);
    puts("modbus core tests passed");
    return 0;
}
