#pragma once

#include <stdint.h>
#include <gui/view.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DictAttack DictAttack;

typedef enum {
    DictAttackEventSkipPressed,
} DictAttackEvent;

typedef void (*DictAttackCallback)(DictAttackEvent event, void* context);

DictAttack* nfcm_dict_attack_alloc();

void nfcm_dict_attack_free(DictAttack* instance);

void nfcm_dict_attack_reset(DictAttack* instance);

View* nfcm_dict_attack_get_view(DictAttack* instance);

void nfcm_dict_attack_set_callback(DictAttack* instance, DictAttackCallback callback, void* context);

void nfcm_dict_attack_set_header(DictAttack* instance, const char* header);

void nfcm_dict_attack_set_card_state(DictAttack* instance, bool detected);

void nfcm_dict_attack_set_sectors_total(DictAttack* instance, uint8_t sectors_total);

void nfcm_dict_attack_set_sectors_read(DictAttack* instance, uint8_t sectors_read);

void nfcm_dict_attack_set_keys_found(DictAttack* instance, uint8_t keys_found);

void nfcm_dict_attack_set_current_sector(DictAttack* instance, uint8_t curr_sec);

void nfcm_dict_attack_set_total_dict_keys(DictAttack* instance, size_t dict_keys_total);

void nfcm_dict_attack_set_current_dict_key(DictAttack* instance, size_t cur_key_num);

void nfcm_dict_attack_set_key_attack(DictAttack* instance, uint8_t sector);

void nfcm_dict_attack_reset_key_attack(DictAttack* instance);

#ifdef __cplusplus
}
#endif
