#include "image_check.h"
#include <string.h>

/* ESP-IDF application format: 24-byte image + 8-byte segment + 256-byte app descriptor. */
bool gr_image_prefix_valid(const uint8_t *d, size_t available, size_t total, size_t capacity) {
    if (available < GR_IMAGE_PREFIX_SIZE || total < GR_IMAGE_PREFIX_SIZE || total > capacity) return false;
    if (d[0] != 0xe9 || d[1] == 0 || d[1] > 16 || d[12] != 0x0d || d[13] != 0) return false;
    if (d[32] != 0x32 || d[33] != 0x54 || d[34] != 0xcd || d[35] != 0xab) return false;
    const char expected[32] = "gas_reader";
    return memcmp(d + 80, expected, sizeof(expected)) == 0;
}
