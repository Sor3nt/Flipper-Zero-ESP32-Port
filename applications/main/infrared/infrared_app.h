/**
 * @file infrared_app.h
 * @brief Infrared application - start here.
 *
 * @see infrared_app_i.h for the main application data structure and functions.
 * @see infrared_remote.h for the infrared remote library - loading, storing and manipulating remotes
 */
#pragma once

/**
 * @brief InfraredApp opaque type declaration.
 */
typedef struct InfraredApp InfraredApp;

#include <storage/storage.h>
#include <furi_hal_infrared.h>

#define INFRARED_SETTINGS_PATH    EXT_PATH("infrared/.infrared.settings")
<<<<<<< HEAD
#define INFRARED_SETTINGS_VERSION (2)
=======
#define INFRARED_SETTINGS_VERSION (1)
>>>>>>> 05c91cb486590019377b94b79a37919e1c650685
#define INFRARED_SETTINGS_MAGIC   (0x1F)

typedef struct {
    FuriHalInfraredTxPin tx_pin;
    bool otg_enabled;
<<<<<<< HEAD
    bool easy_mode;
=======
>>>>>>> 05c91cb486590019377b94b79a37919e1c650685
} InfraredSettings;
