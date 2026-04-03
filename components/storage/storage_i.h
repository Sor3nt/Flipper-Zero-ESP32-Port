/**
 * @file storage_i.h
 * @brief Internal Storage struct (ESP32)
 */
#pragma once

#include <furi.h>
#include "storage.h"

struct Storage {
    FuriPubSub* pubsub;
    FuriMutex* mutex;
    bool sd_mounted;
};
