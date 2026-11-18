#include "modbus_slave.h"

uint16_t mb_crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = (uint16_t)((crc >> 1u) ^ ((crc & 1u) ? 0xa001u : 0u));
        }
    }
    return crc;
}

static size_t finish(uint8_t *response, size_t length) {
    const uint16_t crc = mb_crc16(response, length);
    response[length] = (uint8_t)crc;
    response[length + 1u] = (uint8_t)(crc >> 8u);
    return length + 2u;
}

size_t mb_handle(const mb_slave_t *slave, const uint8_t *request, size_t length,
                 uint8_t *response, size_t capacity) {
    if (!slave || !request || !response || length != 8u || capacity < 5u ||
        request[0] != slave->address || slave->address == 0u ||
        mb_crc16(request, length) != 0u) {
        return 0u;
    }
    const uint8_t function = request[1];
    uint8_t exception = 0u;
    if (function != 3u) {
        exception = 1u; /* illegal function */
    } else {
        const uint16_t start = (uint16_t)((request[2] << 8u) | request[3]);
        const uint16_t count = (uint16_t)((request[4] << 8u) | request[5]);
        if (count == 0u || count > MB_REGISTER_COUNT || start >= MB_REGISTER_COUNT ||
            count > MB_REGISTER_COUNT - start) {
            exception = 2u; /* illegal data address */
        } else if (capacity < (size_t)(5u + 2u * count)) {
            return 0u;
        } else {
            response[0] = slave->address;
            response[1] = function;
            response[2] = (uint8_t)(2u * count);
            for (uint16_t i = 0; i < count; ++i) {
                const uint16_t value = slave->registers[start + i];
                response[3u + 2u * i] = (uint8_t)(value >> 8u);
                response[4u + 2u * i] = (uint8_t)value;
            }
            return finish(response, 3u + 2u * count);
        }
    }
    response[0] = slave->address;
    response[1] = (uint8_t)(function | 0x80u);
    response[2] = exception;
    return finish(response, 3u);
}
