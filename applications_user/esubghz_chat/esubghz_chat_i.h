/*
 * Ported from https://github.com/twisted-pear/esubghz_chat (GPL-3.0).
 * Adapted for this ESP32 port: dropped the NFC key-read/key-share feature
 * (nfc_worker/nfc_dev_data/nfc_popup fields, the NfcPopup view, and the
 * KeyMenuReadKeyFromNfc/KeyDisplayShare events below) -- it depended on the
 * legacy NfcWorker API (lib/nfc/nfc_worker.h), which doesn't exist on this
 * port (built around a PN532 instead of the ST25R3916, with a newer NFC
 * stack). Password, hex-key and generated-key encryption are unaffected.
 */
#pragma once

#include <furi.h>
#include <gui/view_dispatcher_i.h>
#include <gui/view_port_i.h>
#include <gui/scene_manager.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/menu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <notification/notification_messages.h>
#include <lib/subghz/subghz_tx_rx_worker.h>
#include <mbedtls/sha256.h>

#include "crypto_wrapper.h"
#include "scenes/esubghz_chat_scene.h"

#include "esubghz_chat_icons.h"

#define APPLICATION_NAME "ESubGhzChat"

#define DEFAULT_FREQ 433920000

#define RX_TX_BUFFER_SIZE 1024

#define CHAT_BOX_STORE_SIZE 4096
#define TEXT_INPUT_STORE_SIZE 256
#define MSG_PREVIEW_SIZE 32

#define KEY_HEX_STR_SIZE ((KEY_BITS / 8) * 3)

typedef struct {
	SceneManager *scene_manager;
	ViewDispatcher *view_dispatcher;
	NotificationApp *notification;

	// UI elements
	Menu *menu;
	TextBox *chat_box;
	FuriString *chat_box_store;
	TextInput *text_input;
	char text_input_store[TEXT_INPUT_STORE_SIZE + 1];
	ByteInput *hex_key_input;
	uint8_t hex_key_input_store[KEY_BITS / 8];
	DialogEx *key_display;
	char key_hex_str[KEY_HEX_STR_SIZE + 1];

	// for Sub-GHz
	uint32_t frequency;
	SubGhzTxRxWorker *subghz_worker;
	const SubGhzDevice *subghz_device;

	// message assembly before TX
	FuriString *name_prefix;
	FuriString *msg_input;

	// message preview
	char msg_preview[MSG_PREVIEW_SIZE + 1];

	// encryption
	bool encrypted;
	ESubGhzChatCryptoCtx *crypto_ctx;

	// RX and TX buffers
	uint8_t rx_buffer[RX_TX_BUFFER_SIZE];
	uint8_t tx_buffer[RX_TX_BUFFER_SIZE];
	char rx_str_buffer[RX_TX_BUFFER_SIZE + 1];
	volatile uint32_t last_time_rx_data;
	size_t rx_last_avail; /* last observed subghz_tx_rx_worker_available()
	                       * count, used to detect "still growing" vs.
	                       * "settled" -- see esubghz_chat_check_messages(). */

	// for locking
	ViewPortDrawCallback orig_draw_cb;
	ViewPortInputCallback orig_input_cb;
	bool kbd_locked;
	uint32_t kbd_lock_msg_ticks;
	uint8_t kbd_lock_count;

	// for ongoing inputs
	bool kbd_ok_input_ongoing;
	bool kbd_left_input_ongoing;
	bool kbd_right_input_ongoing;

	// set once and never cleared again -- kept for symmetry with upstream's
	// exit path, real backgrounding (bgloader) support was dropped
	bool exit_for_real;
} ESubGhzChatState;

typedef enum {
	ESubGhzChatEvent_FreqEntered,
	ESubGhzChatEvent_KeyMenuNoEncryption,
	ESubGhzChatEvent_KeyMenuPassword,
	ESubGhzChatEvent_KeyMenuHexKey,
	ESubGhzChatEvent_KeyMenuGenKey,
	ESubGhzChatEvent_PassEntered,
	ESubGhzChatEvent_HexKeyEntered,
	ESubGhzChatEvent_MsgEntered,
	ESubGhzChatEvent_GotoMsgInput,
	ESubGhzChatEvent_GotoKeyDisplay,
	ESubGhzChatEvent_KeyDisplayBack,
} ESubGhzChatEvent;

typedef enum {
	ESubGhzChatView_Menu,
	ESubGhzChatView_Input,
	ESubGhzChatView_HexKeyInput,
	ESubGhzChatView_ChatBox,
	ESubGhzChatView_KeyDisplay,
} ESubGhzChatView;

void set_chat_input_header(ESubGhzChatState *state);
void append_msg(ESubGhzChatState *state, const char *msg);
void tx_msg_input(ESubGhzChatState *state);
void enter_chat(ESubGhzChatState *state);
