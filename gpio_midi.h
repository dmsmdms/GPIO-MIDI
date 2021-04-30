#pragma once

#include <stdint.h>

enum {
    CONFIG_TEST_KEY_TIMEOUT = 1,
    CONFIG_CONNECT_TIMEOUT  = 1,
    CONFIG_MAX_GPIO_TIMEOUT = 64 * 1024,
    CONFIG_MAX_EPOLL_EVENTS = 4,
    CONFIG_MAX_MIDI_EVENTS  = 16,
};

typedef struct {
    uint8_t key;
    uint8_t velocity;
} midi_event_t;
