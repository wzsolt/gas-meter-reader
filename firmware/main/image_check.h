#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define GR_IMAGE_PREFIX_SIZE 288
bool gr_image_prefix_valid(const uint8_t *data, size_t available, size_t total, size_t capacity);
