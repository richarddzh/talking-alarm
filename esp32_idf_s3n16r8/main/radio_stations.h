#pragma once

#include <stddef.h>

typedef struct {
    const char *name;
    const char *region;
    const char *url;
} radio_station_t;

size_t radio_station_count(void);
const radio_station_t *radio_station_get(size_t index);
