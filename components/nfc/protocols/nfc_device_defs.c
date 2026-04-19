/**
 * @file nfc_device_defs.c
 * @brief Main NFC device implementation definitions.
 *
 * All NFC device implementations must be registered here in order to be used
 * by the NfcDevice library.
 *
 * @see nfc_device.h
 *
 * This file is to be modified upon adding a new protocol (see below).
 */
#include "nfc_device_base_i.h"
#include "nfc_protocol.h"

#include <protocols/iso14443_3a/iso14443_3a_device_defs.h>
#include <protocols/iso14443_3b/iso14443_3b_device_defs.h>
#include <protocols/iso14443_4a/iso14443_4a_device_defs.h>
#include <protocols/iso14443_4b/iso14443_4b_device_defs.h>
#include <protocols/iso15693_3/iso15693_3_device_defs.h>
#include <protocols/felica/felica.h>
#include <protocols/mf_ultralight/mf_ultralight.h>
#include <protocols/mf_classic/mf_classic.h>
#include <protocols/mf_plus/mf_plus.h>
#include <protocols/mf_desfire/mf_desfire.h>
#include <protocols/slix/slix_device_defs.h>
#include <protocols/st25tb/st25tb.h>
#include <protocols/ntag4xx/ntag4xx.h>
#include <protocols/type_4_tag/type_4_tag.h>
#include <protocols/emv/emv.h>

/**
 * @brief List of registered NFC device implementations.
 *
 * When implementing a new protocol, add its implementation
 * here under its own index defined in nfc_protocol.h.
 */
const NfcDeviceBase* const nfc_devices[NfcProtocolNum] = {
    [NfcProtocolIso14443_3a] = &nfc_device_iso14443_3a,
    [NfcProtocolIso14443_3b] = &nfc_device_iso14443_3b,
    [NfcProtocolIso14443_4a] = &nfc_device_iso14443_4a,
    [NfcProtocolIso14443_4b] = &nfc_device_iso14443_4b,
    [NfcProtocolIso15693_3] = &nfc_device_iso15693_3,
    [NfcProtocolFelica] = &nfc_device_felica,
    [NfcProtocolMfUltralight] = &nfc_device_mf_ultralight,
    [NfcProtocolMfClassic] = &nfc_device_mf_classic,
    [NfcProtocolMfPlus] = &nfc_device_mf_plus,
    [NfcProtocolMfDesfire] = &nfc_device_mf_desfire,
    [NfcProtocolSlix] = &nfc_device_slix,
    [NfcProtocolSt25tb] = &nfc_device_st25tb,
    [NfcProtocolNtag4xx] = &nfc_device_ntag4xx,
    [NfcProtocolType4Tag] = &nfc_device_type_4_tag,
    [NfcProtocolEmv] = &nfc_device_emv,
    /* Add new protocols here */
};
