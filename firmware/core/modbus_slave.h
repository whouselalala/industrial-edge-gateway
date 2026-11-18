#ifndef MODBUS_SLAVE_H
#define MODBUS_SLAVE_H

#include <stddef.h>
#include <stdint.h>

#define MB_REGISTER_COUNT 4u
#define MB_MAX_FRAME 16u

typedef struct {
    uint8_t address;
    uint16_t registers[MB_REGISTER_COUNT];
} mb_slave_t;

uint16_t mb_crc16(const uint8_t *data, size_t length);
/* Return response length; zero means ignore invalid or unrelated frame. */
size_t mb_handle(const mb_slave_t *slave, const uint8_t *request, size_t length,
                 uint8_t *response, size_t response_capacity);

#endif
