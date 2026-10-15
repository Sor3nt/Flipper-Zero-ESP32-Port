#include "protopirate_models.h"
#ifdef ENABLE_MODELS_DATABASE

#define TAG "PPModels"

// Same CC1101 hardware ranges as furi_hal_subghz_is_frequency_valid;
// that convenience helper is not exported by the deployed ESP32 FAP ABI.
static bool pp_model_frequency_valid(uint32_t value) {
    return (value >= 281000000 && value <= 361000000) ||
           (value >= 378000000 && value <= 481000000) ||
           (value >= 749000000 && value <= 962000000);
}

#define PROTOPIRATE_MODELS_FILE APP_ASSETS_PATH("models_db/models.txt")

//Models File Format.
#define MODELS_FILE_HEADER              "ProtoPirate Car Models Database"
#define MODELS_FILE_VERSION             2
#define MODELS_FIELD_MODEL_COUNT        "ModelCount"
#define MODELS_FIELD_MODEL_NAME         "ModelName"
#define MODELS_FIELD_FREQUENCY          "Frequency"
#define MODELS_FIELD_PRESET_NAME        "PresetName"
#define MODELS_FIELD_CUSTOM_PRESET_DATA "CustomPresetData"

bool car_model_get_by_index(
    ProtoPirateCarModel* car_model,
    uint16_t index,
    uint16_t car_models_count,
    SubGhzSetting* app_settings) {
    if(!car_model || !app_settings) return false;
    if(index > car_models_count) index = 0;
    //Temp variables
    uint32_t version = 0;
    uint32_t frequency;
    uint8_t* preset_data = NULL;
    uint32_t preset_data_size = 0;
    bool error = true;
    bool custom_preset = false;

    //Objects that need allocating.
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    FuriString* preset_name = furi_string_alloc();
    FuriString* model_name = furi_string_alloc();
    FuriString* header = furi_string_alloc();

    //Index 0 is None.
    switch(index) {
    case 0:
        //Leave as same as error (none)
        break;
    default: {
        //Open Models File, check the Header, check Version
        if(!flipper_format_file_open_existing(ff, PROTOPIRATE_MODELS_FILE)) {
            FURI_LOG_I(TAG, "models.txt file not found");
            break;
        } else if(!flipper_format_read_header(ff, header, &version)) {
            FURI_LOG_W(TAG, "Failed to read models.txt header");
            break;
        } else if(furi_string_cmp_str(header, MODELS_FILE_HEADER) != 0) {
            FURI_LOG_W(TAG, "Invalid models.txt file header");
            break;
        } else if(version != MODELS_FILE_VERSION) {
            FURI_LOG_W(TAG, "Unsupported models.txt version %lu", (unsigned long)version);
            break;
        }

        //Fields we add the number to for searching the file.
        char model_name_index[16]; //Frequency65535 + NULL
        char frequency_index[16]; //Frequency65535 + NULL
        char preset_name_index[18]; //PresetName65535 + NULL
        char preset_data_index[23]; //CustomPresetData65535 + NULL

        //Build the values we need to grab.
        snprintf(model_name_index, sizeof(model_name_index), MODELS_FIELD_MODEL_NAME "%u", index);
        snprintf(frequency_index, sizeof(frequency_index), MODELS_FIELD_FREQUENCY "%u", index);
        snprintf(
            preset_name_index, sizeof(preset_name_index), MODELS_FIELD_PRESET_NAME "%u", index);
        snprintf(
            preset_data_index,
            sizeof(preset_data_index),
            MODELS_FIELD_CUSTOM_PRESET_DATA "%u",
            index);

        //Read the Model Name.
        if(!flipper_format_read_string(ff, model_name_index, model_name)) {
            FURI_LOG_E(TAG, "Failed to read %s", model_name_index);
            break;
        }

        // Read frequency
        if(!flipper_format_read_uint32(ff, frequency_index, &frequency, 1) ||
           !pp_model_frequency_valid(frequency)) {
            FURI_LOG_W(TAG, "Failed to read frequency.");
            frequency = 0;
            break;
        }

        //Read the Modulation Preset.
        if(!flipper_format_read_string(ff, preset_name_index, preset_name)) {
            //Set the Custom preset name.
            furi_string_set_str(preset_name, "Custom");
            custom_preset = true;
            flipper_format_rewind(ff);
        } else {
            //Get the built in preset if it exists
            const char* preset_name_long = furi_string_get_cstr(preset_name);
            const char* preset_name_short = preset_name_long;

            //Which Preset have we selected?
            if(!strcmp(preset_name_long, "FuriHalSubGhzPresetOok270Async")) {
                preset_name_short = "AM270";
            } else if(!strcmp(preset_name_long, "FuriHalSubGhzPresetOok650Async")) {
                preset_name_short = "AM650";
            } else if(!strcmp(preset_name_long, "FuriHalSubGhzPreset2FSKDev238Async")) {
                preset_name_short = "FM238";
            } else if(!strcmp(preset_name_long, "FuriHalSubGhzPreset2FSKDev12KAsync")) {
                preset_name_short = "FM12K";
            } else if(!strcmp(preset_name_long, "FuriHalSubGhzPreset2FSKDev476Async")) {
                preset_name_short = "FM476";
            } else if(!strcmp(preset_name_long, "FuriHalSubGhzPresetCustom")) {
                preset_name_short = "Custom";
                custom_preset = true;
            }

            // Updating the string can invalidate a borrowed pointer into it.
            furi_string_set_str(preset_name, preset_name_short);
            preset_name_short = furi_string_get_cstr(preset_name);

            if(!custom_preset) {
                size_t preset_index = subghz_setting_get_preset_count(app_settings);
                for(size_t i = 0; i < subghz_setting_get_preset_count(app_settings); i++) {
                    if(!strcmp(
                           subghz_setting_get_preset_name(app_settings, i), preset_name_short)) {
                        preset_index = i;
                        break;
                    }
                }
                if(preset_index >= subghz_setting_get_preset_count(app_settings)) {
                    preset_name_short = "AM650";
                    for(size_t i = 0; i < subghz_setting_get_preset_count(app_settings); i++) {
                        if(!strcmp(
                               subghz_setting_get_preset_name(app_settings, i),
                               preset_name_short)) {
                            preset_index = i;
                            break;
                        }
                    }
                    if(preset_index >= subghz_setting_get_preset_count(app_settings)) {
                        FURI_LOG_E(TAG, "Failed to get preset index!");
                        break;
                    }
                }

                furi_string_set_str(preset_name, preset_name_short);

                //Get the preset data into a buffer to copy it.
                uint8_t* temp_preset_data = NULL;
                uint32_t temp_preset_data_size = 0;
                temp_preset_data = subghz_setting_get_preset_data(app_settings, preset_index);
                temp_preset_data_size =
                    subghz_setting_get_preset_data_size(app_settings, preset_index);

                //Error fallback
                if(temp_preset_data == NULL) {
                    FURI_LOG_E(TAG, "Failed to get preset data!");
                    break;
                } else {
                    //Copy the preset data, so we can initialize it.
                    preset_data = malloc(temp_preset_data_size);
                    if(!preset_data || !temp_preset_data_size) break;
                    memcpy(preset_data, temp_preset_data, temp_preset_data_size);
                    preset_data_size = temp_preset_data_size;

                    //Yay, we made it...
                    error = false;
                }
            }
        }

        /* Custom_preset_data (only if present) */
        if(custom_preset) {
            if(flipper_format_get_value_count(ff, preset_data_index, &preset_data_size) &&
               preset_data_size > 0) {
                if(preset_data_size >= 1024) {
                    FURI_LOG_E(
                        TAG,
                        "%s too large: %lu",
                        preset_data_index,
                        (unsigned long)preset_data_size);
                    break;
                }

                preset_data = malloc(preset_data_size);
                if(!preset_data) {
                    FURI_LOG_E(
                        TAG,
                        "Malloc failed: %s (%lu bytes)",
                        preset_data_index,
                        (unsigned long)preset_data_size);
                    break;
                }

                flipper_format_rewind(ff);
                if(!flipper_format_read_hex(ff, preset_data_index, preset_data, preset_data_size)) {
                    break;
                }

                // CC1101 register/value pairs must terminate before the 8-byte PA table.
                bool terminated = false;
                for(size_t n = 0; n + 9 < preset_data_size; n += 2) {
                    if(preset_data[n] == 0 && preset_data[n + 1] == 0) {
                        terminated = (n + 10 == preset_data_size);
                        break;
                    }
                    if(preset_data[n] > 0x2e) break;
                }
                if(!terminated) break;
                FURI_LOG_D(TAG, "Using Custom Preset Data from Models File...");

                //Yay, we made it...
                error = false;
            }
        }
    }
    }

    char* new_name = NULL;
    SubGhzRadioPreset* new_preset = NULL;
    if(!error) {
        new_name = strdup(furi_string_get_cstr(model_name));
        new_preset = calloc(1, sizeof(SubGhzRadioPreset));
        if(!new_name || !new_preset) error = true;
    }
    if(!error) {
        new_preset->name = furi_string_alloc_set(preset_name);
        new_preset->data = preset_data;
        new_preset->data_size = preset_data_size;
        new_preset->frequency = frequency;
        preset_data = NULL; // ownership transferred
    } else {
        free(new_name);
        free(new_preset);
        new_preset = NULL;
        new_name = strdup(car_models_count ? "< Select a Car Model >" : "No Models in Database");
    }
    if(car_model->preset) {
        free(car_model->preset->data);
        furi_string_free(car_model->preset->name);
        free(car_model->preset);
    }
    free(car_model->name);
    car_model->name = new_name;
    car_model->preset = new_preset;
    car_model->index = error ? 0 : index;
    free(preset_data);

    //Close up and return.
    flipper_format_free(ff);
    furi_string_free(header);
    furi_string_free(model_name);
    furi_string_free(preset_name);
    furi_record_close(RECORD_STORAGE);
    return (!error);
}

uint16_t car_model_get_count(void) {
    uint32_t version = 0;
    uint32_t model_count = 0;

    //Set up storage, init temp variables
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    FuriString* header = furi_string_alloc();

    //Open file, read header, check version, and grab the model count.
    if(!flipper_format_file_open_existing(ff, PROTOPIRATE_MODELS_FILE)) {
        FURI_LOG_W(TAG, "Failed to open " PROTOPIRATE_MODELS_FILE);
    } else if(!flipper_format_read_header(ff, header, &version)) {
        FURI_LOG_W(TAG, "Failed to read models file header.");
    } else if(version != MODELS_FILE_VERSION || furi_string_cmp_str(header, MODELS_FILE_HEADER) != 0) {
        FURI_LOG_W(TAG, "Models file version is invalid.");
    } else if(!flipper_format_read_uint32(ff, MODELS_FIELD_MODEL_COUNT, &model_count, 1)) {
        FURI_LOG_W(TAG, "Failed to read " MODELS_FIELD_MODEL_COUNT ".");
    }

    //Free temp variables, close storage.
    furi_string_free(header);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);

    //Finished, return the count or 0 if error.
    return model_count <= UINT16_MAX ? (uint16_t)model_count : 0;
}
#endif
