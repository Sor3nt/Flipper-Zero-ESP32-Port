#include "emv_render.h"

#include "../iso14443_4a/iso14443_4a_render.h"
#include <bit_lib.h>
#include <string.h>
#include "nfc/nfc_app_i.h"

void nfc_render_emv_info(const EmvData* data, NfcProtocolFormatType format_type, FuriString* str) {
    nfc_render_emv_header(str);
    nfc_render_emv_uid(
        data->iso14443_4a_data->iso14443_3a_data->uid,
        data->iso14443_4a_data->iso14443_3a_data->uid_len,
        str);

    if(format_type == NfcProtocolFormatTypeFull) nfc_render_emv_extra(data, str);

    if(format_type == NfcProtocolFormatTypeFull) {
        FURI_LOG_I("EMVInfo", "----- BEGIN RENDERED EMV INFO -----");
        FURI_LOG_I("EMVInfo", "%s", furi_string_get_cstr(str));
        FURI_LOG_I("EMVInfo", "------ END RENDERED EMV INFO ------");
    }
}

void nfc_render_emv_header(FuriString* str) {
    furi_string_cat_printf(str, "\e#%s\n", "EMV");
}

void nfc_render_emv_uid(const uint8_t* uid, const uint8_t uid_len, FuriString* str) {
    if(uid_len == 0) return;

    furi_string_cat_printf(str, "UID: ");

    for(uint8_t i = 0; i < uid_len; i++) {
        furi_string_cat_printf(str, "%02X ", uid[i]);
    }

    furi_string_cat_printf(str, "\n");
}

void nfc_render_emv_data(const EmvData* data, FuriString* str) {
    nfc_render_emv_pan(data->emv_application.pan, data->emv_application.pan_len, str);
    nfc_render_emv_name(data->emv_application.application_name, str);
}

void nfc_render_emv_pan(const uint8_t* data, const uint8_t len, FuriString* str) {
    if(len == 0) return;

    FuriString* card_number = furi_string_alloc();
    for(uint8_t i = 0; i < len; i++) {
        if((i % 2 == 0) && (i != 0)) furi_string_cat_printf(card_number, " ");
        furi_string_cat_printf(card_number, "%02X", data[i]);
    }

    // Cut padding 'F' from card number
    furi_string_trim(card_number, "F");
    furi_string_cat(str, card_number);
    furi_string_free(card_number);

    furi_string_cat_printf(str, "\n");
}

void nfc_render_emv_currency(uint16_t cur_code, FuriString* str) {
    if(!cur_code) return;

    furi_string_cat_printf(str, "Currency code: %04X\n", cur_code);
}

void nfc_render_emv_country(uint16_t country_code, FuriString* str) {
    if(!country_code) return;

    furi_string_cat_printf(str, "Country code: %04X\n", country_code);
}

void nfc_render_emv_application(const EmvApplication* apl, FuriString* str) {
    const uint8_t len = apl->aid_len;

    furi_string_cat_printf(str, "AID: ");
    for(uint8_t i = 0; i < len; i++)
        furi_string_cat_printf(str, "%02X", apl->aid[i]);
    furi_string_cat_printf(str, "\n");
}

void nfc_render_emv_application_interchange_profile(const EmvApplication* apl, FuriString* str) {
    uint16_t data = bit_lib_bytes_to_num_be(apl->application_interchange_profile, 2);

    if(!data) {
        furi_string_cat_printf(str, "No Interchange profile found\n");
        return;
    }

    uint8_t b1 = apl->application_interchange_profile[0];
    uint8_t b2 = apl->application_interchange_profile[1];
    furi_string_cat_printf(str, "AIP: %02X%02X", b1, b2);
    bool first = true;
    #define AIP_FLAG(cond, name) do { \
        if(cond) { furi_string_cat_printf(str, first ? " " : "/"); \
                   furi_string_cat_printf(str, name); first = false; } \
    } while(0)
    AIP_FLAG(b1 & 0x40, "SDA");
    AIP_FLAG(b1 & 0x20, "DDA");
    AIP_FLAG(b1 & 0x10, "CV");
    AIP_FLAG(b1 & 0x08, "TRM");
    AIP_FLAG(b1 & 0x04, "IssAuth");
    AIP_FLAG(b1 & 0x01, "CDA");
    AIP_FLAG(b2 & 0x80, "EMVMode");
    #undef AIP_FLAG
    furi_string_cat_printf(str, "\n");
}

void nfc_render_emv_transactions(const EmvApplication* apl, FuriString* str) {
    if(apl->transaction_counter)
        furi_string_cat_printf(str, "Transactions count: %d\n", apl->transaction_counter);
    if(apl->last_online_atc)
        furi_string_cat_printf(str, "Last Online ATC: %d\n", apl->last_online_atc);

    const uint8_t len = apl->active_tr;
    if(!len) {
        furi_string_cat_printf(str, "No transactions info\n");
        return;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* tmp = furi_string_alloc();

    furi_string_cat_printf(str, "Transactions:\n");
    for(int i = 0; i < len; i++) {
        // If no date and amount - skip
        if((!apl->trans[i].date) && (!apl->trans[i].amount)) continue;
        // transaction counter
        furi_string_cat_printf(str, "\e#%d: ", apl->trans[i].atc);

        // Print transaction amount
        if(!apl->trans[i].amount) {
            furi_string_cat_printf(str, "???");
        } else {
            FURI_LOG_D("EMV Render", "Amount: %llX\n", apl->trans[i].amount);
            uint8_t amount_bytes[6];
            bit_lib_num_to_bytes_le(apl->trans[i].amount, 6, amount_bytes);

            bool junk = false;
            uint64_t amount = bit_lib_bytes_to_num_bcd(amount_bytes, 6, &junk);
            uint8_t amount_cents = amount % 100;

            furi_string_cat_printf(str, "%llu.%02u", amount / 100, amount_cents);
        }

        if(apl->trans[i].currency) {
            furi_string_set_str(tmp, "UNK");
            nfc_emv_parser_get_currency_name(storage, apl->trans[i].currency, tmp);
            furi_string_cat_printf(str, " %s\n", furi_string_get_cstr(tmp));
        }

        if(apl->trans[i].country) {
            furi_string_set_str(tmp, "UNK");
            nfc_emv_parser_get_country_name(storage, apl->trans[i].country, tmp);
            furi_string_cat_printf(str, "Country: %s\n", furi_string_get_cstr(tmp));
        }

        if(apl->trans[i].date)
            furi_string_cat_printf(
                str,
                "%02lx.%02lx.%02lx  ",
                apl->trans[i].date >> 16,
                (apl->trans[i].date >> 8) & 0xff,
                apl->trans[i].date & 0xff);

        if(apl->trans[i].time)
            furi_string_cat_printf(
                str,
                "%02lx:%02lx:%02lx",
                apl->trans[i].time & 0xff,
                (apl->trans[i].time >> 8) & 0xff,
                apl->trans[i].time >> 16);

        // Line break
        furi_string_cat_printf(str, "\n");
    }

    furi_string_free(tmp);
    furi_record_close(RECORD_STORAGE);
}

static void nfc_render_emv_vendor(const EmvApplication* apl, FuriString* str) {
    if(apl->application_name[0]) {
        furi_string_cat_printf(str, "\e#%s\n", apl->application_name);
    } else if(apl->application_label[0]) {
        furi_string_cat_printf(str, "\e#%s\n", apl->application_label);
    } else if(apl->aid_len >= 7) {
        const uint8_t* a = apl->aid;
        if(a[0] == 0xA0 && a[1] == 0x00 && a[2] == 0x00 && a[3] == 0x00 && a[4] == 0x03)
            furi_string_cat_printf(str, "\e#VISA\n");
        else if(a[0] == 0xA0 && a[1] == 0x00 && a[2] == 0x00 && a[3] == 0x00 && a[4] == 0x04)
            furi_string_cat_printf(str, "\e#MasterCard\n");
        else if(a[0] == 0xA0 && a[1] == 0x00 && a[2] == 0x00 && a[3] == 0x00 && a[4] == 0x25)
            furi_string_cat_printf(str, "\e#American Express\n");
        else if(a[0] == 0xA0 && a[1] == 0x00 && a[2] == 0x00 && a[3] == 0x00 && a[4] == 0x65)
            furi_string_cat_printf(str, "\e#JCB\n");
        else if(a[0] == 0xA0 && a[1] == 0x00 && a[2] == 0x00 && a[3] == 0x01 && a[4] == 0x52)
            furi_string_cat_printf(str, "\e#Discover\n");
        else if(a[0] == 0xA0 && a[1] == 0x00 && a[2] == 0x00 && a[3] == 0x03 && a[4] == 0x33)
            furi_string_cat_printf(str, "\e#UnionPay\n");
    }
}

static void nfc_render_emv_pan_pretty(const EmvApplication* apl, FuriString* str) {
    if(apl->pan_len == 0) return;
    FuriString* num = furi_string_alloc();
    for(uint8_t i = 0; i < apl->pan_len; i++) {
        if((i % 2 == 0) && (i != 0)) furi_string_cat_printf(num, " ");
        furi_string_cat_printf(num, "%02X", apl->pan[i]);
    }
    furi_string_trim(num, "F");
    furi_string_cat_printf(str, "PAN: %s\n", furi_string_get_cstr(num));
    furi_string_free(num);
}

static void nfc_render_emv_dates(const EmvApplication* apl, FuriString* str) {
    if(apl->exp_year || apl->exp_month) {
        furi_string_cat_printf(str, "Exp: %02X/%02X\n", apl->exp_month, apl->exp_year);
    }
    if(apl->effective_year || apl->effective_month) {
        furi_string_cat_printf(
            str, "Issued: %02X/%02X\n", apl->effective_month, apl->effective_year);
    }
}

static void nfc_render_emv_cardholder(const EmvApplication* apl, FuriString* str) {
    if(apl->cardholder_name[0] && apl->cardholder_name[0] != ' ') {
        furi_string_cat_printf(str, "Holder: %s\n", apl->cardholder_name);
        return;
    }
    /* Card didn't return tag 0x5F20 — emit the network's default placeholder
     * the way OFW EMV reader does. Inferred from AID prefix. */
    if(apl->aid_len >= 5) {
        const uint8_t* a = apl->aid;
        if(a[0] == 0xA0 && a[1] == 0x00 && a[2] == 0x00 && a[3] == 0x00 && a[4] == 0x03)
            furi_string_cat_printf(str, "Holder: CARDHOLDER/VISA\n");
        else if(a[0] == 0xA0 && a[1] == 0x00 && a[2] == 0x00 && a[3] == 0x00 && a[4] == 0x04)
            furi_string_cat_printf(str, "Holder: CARDHOLDER/MASTERCARD\n");
    }
}

static void nfc_render_emv_label(const EmvApplication* apl, FuriString* str) {
    if(apl->application_label[0] &&
       (!apl->application_name[0] ||
        strcmp(apl->application_name, apl->application_label) != 0)) {
        furi_string_cat_printf(str, "Label: %s\n", apl->application_label);
    }
}

static void nfc_render_emv_service_code(const EmvApplication* apl, FuriString* str) {
    if(!apl->service_code) return;
    uint16_t sc = apl->service_code;
    uint8_t d1 = (sc / 100) % 10;
    uint8_t d2 = (sc / 10) % 10;
    uint8_t d3 = sc % 10;

    const char* tech;
    switch(d1) {
    case 1: tech = "Intl"; break;
    case 2: tech = "IntlIC"; break;
    case 5: tech = "Natl"; break;
    case 6: tech = "NatlIC"; break;
    case 7: tech = "Priv"; break;
    case 9: tech = "Test"; break;
    default: tech = "?"; break;
    }
    const char* auth;
    switch(d2) {
    case 0: auth = "Norm"; break;
    case 2: auth = "Online"; break;
    case 4: auth = "Off+Auth"; break;
    default: auth = "?"; break;
    }
    const char* svc;
    switch(d3) {
    case 0: svc = "PIN"; break;
    case 1: svc = "Free"; break;
    case 2: svc = "G&S"; break;
    case 3: svc = "ATM+PIN"; break;
    case 4: svc = "Cash"; break;
    case 5: svc = "G&S+PIN"; break;
    case 6: svc = "PINprompt"; break;
    case 7: svc = "G&S+PINprompt"; break;
    default: svc = "?"; break;
    }
    furi_string_cat_printf(str, "SC %u%u%u %s/%s/%s\n", d1, d2, d3, tech, auth, svc);
}

static void nfc_render_emv_cvm_list(const EmvApplication* apl, FuriString* str) {
    if(apl->cvm_list_len < 10) {
        /* Card didn't expose a CVM list (most contactless cards don't).
         * Tell the user we checked, and infer the practical PIN policy:
         * if no CVM rules are published, the card relies on the terminal's
         * floor limit — small purchases skip cardholder verification, large
         * purchases prompt the terminal (typically signature or no-CVM). */
        furi_string_cat_printf(str, "\e#CVM list\n");
        furi_string_cat_printf(str, "(no CVM list found)\n");
        furi_string_cat_printf(str, "PIN: card never asks\n");
        furi_string_cat_printf(str, "(terminal limit applies)\n");
        return;
    }
    /* CVM List: [X amount: 4][Y amount: 4][CV rule: 2]+ */
    uint32_t amount_x = (apl->cvm_list[0] << 24) | (apl->cvm_list[1] << 16) |
                       (apl->cvm_list[2] << 8) | apl->cvm_list[3];
    uint32_t amount_y = (apl->cvm_list[4] << 24) | (apl->cvm_list[5] << 16) |
                       (apl->cvm_list[6] << 8) | apl->cvm_list[7];
    furi_string_cat_printf(str, "\e#CVM list\n");
    if(amount_x) furi_string_cat_printf(str, "X amount: %lu\n", (unsigned long)amount_x);
    if(amount_y) furi_string_cat_printf(str, "Y amount: %lu\n", (unsigned long)amount_y);

    for(uint8_t pos = 8; pos + 1 < apl->cvm_list_len; pos += 2) {
        uint8_t cvm = apl->cvm_list[pos] & 0x3F;
        bool fail_continue = (apl->cvm_list[pos] & 0x40) != 0;
        uint8_t cond = apl->cvm_list[pos + 1];

        const char* method;
        switch(cvm) {
        case 0x00: method = "Fail CVM"; break;
        case 0x01: method = "Plain PIN"; break;
        case 0x02: method = "Online PIN"; break;
        case 0x03: method = "Plain PIN + sig"; break;
        case 0x04: method = "Enciphered PIN"; break;
        case 0x05: method = "Enciphered PIN + sig"; break;
        case 0x1E: method = "Signature"; break;
        case 0x1F: method = "No CVM"; break;
        default: method = "?"; break;
        }
        const char* condition;
        switch(cond) {
        case 0x00: condition = "always"; break;
        case 0x01: condition = "if cash"; break;
        case 0x02: condition = "if not cash"; break;
        case 0x03: condition = "if terminal supports"; break;
        case 0x04: condition = "if manual cash"; break;
        case 0x05: condition = "if purchase + cashback"; break;
        case 0x06: condition = "if X currency, < X"; break;
        case 0x07: condition = "if X currency, > X"; break;
        case 0x08: condition = "if Y currency, < Y"; break;
        case 0x09: condition = "if Y currency, > Y"; break;
        default: condition = "?"; break;
        }
        furi_string_cat_printf(
            str, "%s%s, %s\n", method, fail_continue ? " (try next on fail)" : "", condition);
    }
}

static void nfc_render_emv_records_hex(const EmvApplication* apl, FuriString* str) {
    if(apl->records_raw_len == 0) return;
    furi_string_cat_printf(str, "\e#Records (%u bytes)\n", apl->records_raw_len);
    for(uint16_t k = 0; k < apl->records_raw_len; k++) {
        if(apl->records_raw[k] == 0xFF && k > 0 && k + 1 < apl->records_raw_len) {
            /* separator between records: insert a newline */
            furi_string_cat_printf(str, "\n");
        } else {
            furi_string_cat_printf(str, "%02X", apl->records_raw[k]);
            if((k % 16) == 15) furi_string_cat_printf(str, "\n");
            else if((k % 2) == 1) furi_string_cat_printf(str, " ");
        }
    }
    furi_string_cat_printf(str, "\n");
}

static void nfc_render_emv_counters(const EmvApplication* apl, FuriString* str) {
    if(apl->transaction_counter)
        furi_string_cat_printf(str, "ATC: %d\n", apl->transaction_counter);
    if(apl->last_online_atc)
        furi_string_cat_printf(str, "Last Online ATC: %d\n", apl->last_online_atc);
    /* PIN try counter: only show if a valid GET DATA response set it.
     * 0xFF is the sentinel for "no card response / parse failed"; 0 means
     * the field was never populated. EMV cards never legitimately report
     * 255 retries, so suppress that value. */
    if(apl->pin_try_counter && apl->pin_try_counter != 0xFF)
        furi_string_cat_printf(str, "PIN tries left: %d\n", apl->pin_try_counter);
}

void nfc_render_emv_extra(const EmvData* data, FuriString* str) {
    const EmvApplication* apl = &data->emv_application;

    nfc_render_emv_vendor(apl, str);
    nfc_render_emv_pan_pretty(apl, str);
    nfc_render_emv_dates(apl, str);
    nfc_render_emv_cardholder(apl, str);
    nfc_render_emv_label(apl, str);
    nfc_render_emv_application(apl, str);
    nfc_render_emv_application_interchange_profile(apl, str);
    nfc_render_emv_service_code(apl, str);
    nfc_render_emv_counters(apl, str);
    nfc_render_emv_currency(apl->currency_code, str);
    nfc_render_emv_country(apl->country_code, str);
    nfc_render_emv_cvm_list(apl, str);
    nfc_render_emv_records_hex(apl, str);
}
