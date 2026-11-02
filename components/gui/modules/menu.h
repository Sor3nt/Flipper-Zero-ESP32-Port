/**
 * @file menu.h
 * GUI: Menu view module API
 */

#pragma once

#include "../view.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Menu anonymous structure */
typedef struct Menu Menu;

/** Menu Item Callback */
typedef void (*MenuItemCallback)(void* context, uint32_t index);

/** Menu allocation and initialization
 *
 * @return     Menu instance
 */
Menu* menu_alloc(void);

/** Free menu
 *
 * @param      menu  Menu instance
 */
void menu_free(Menu* menu);

/** Get Menu view
 *
 * @param      menu  Menu instance
 *
 * @return     View instance
 */
View* menu_get_view(Menu* menu);

/** Add item to menu
 *
 * @param      menu      Menu instance
 * @param      label     menu item string label
 * @param      icon      IconAnimation instance
 * @param      index     menu item index
 * @param      callback  MenuItemCallback instance
 * @param      context   pointer to context
 */
void menu_add_item(
    Menu* menu,
    const char* label,
    const Icon* icon,
    uint32_t index,
    MenuItemCallback callback,
    void* context);

/** Clean menu
 * @note       this function does not free menu instance
 *
 * @param      menu  Menu instance
 */
void menu_reset(Menu* menu);

/** Set current menu item
 *
 * @param      menu   Menu instance
 * @param      index  The index
 */
void menu_set_selected_item(Menu* menu, uint32_t index);

/** Menu render style
 *  @note  Mirrors Momentum-Firmware menu styles (List/Dsi/Wii).
 */
typedef enum {
    MenuStyleList, // default vertical list
    MenuStyleWii, // Nintendo Wii Channel grid
    MenuStyleDsi, // Nintendo DSi home menu
    MenuStylePs4, // PS4-style horizontal bar
    MenuStyleVertical, // rotated vertical list
    MenuStyleC64, // Commodore 64 BASIC listing
    MenuStyleCompact, // dense two-column list
    MenuStyleMNTM, // Momentum home-screen dashboard
    MenuStyleCoverFlow, // album-coverflow carousel
    MenuStyleCount,
} MenuStyle;

/** Set current menu render style (persisted).
 *
 * @param      style  MenuStyle to apply
 */
void menu_set_style(MenuStyle style);

/** Get current menu render style.
 *
 * @return     active MenuStyle
 */
MenuStyle menu_get_style(void);

/** Lock screen render style (Momentum-Firmware Control Center).
 */
typedef enum {
    LockScreenStyleDefault, // weak text list (stock port)
    LockScreenStyleMomentum, // Momentum Control Center grid + sliders
    LockScreenStyleCount,
} LockScreenStyle;

/** Set current lock screen render style (persisted).
 *
 * @param      style  LockScreenStyle to apply
 */
void lock_screen_set_style(LockScreenStyle style);

/** Get current lock screen render style.
 *
 * @return     active LockScreenStyle
 */
LockScreenStyle lock_screen_get_style(void);

#ifdef __cplusplus
}
#endif
