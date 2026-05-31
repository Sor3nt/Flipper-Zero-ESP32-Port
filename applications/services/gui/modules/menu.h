/**
 * @file menu.h
 * GUI: Menu view module API
 */

#pragma once

#include <gui/view.h>
#include <components/momentum/settings.h>

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

/** Get index of the selected menu item
 *
 * @param      menu  Menu instance
 *
 * @return     selected item index
 */
uint32_t menu_get_selected_item(const Menu* menu);

/** Set menu style
 *
 * @param      menu   Menu instance
 * @param      style  Desired MenuStyle
 */
void menu_set_style(Menu* menu, MenuStyle style);

/** Get current menu style
 *
 * @param      menu  Menu instance
 *
 * @return     current MenuStyle
 */
MenuStyle menu_get_style(const Menu* menu);

#ifdef __cplusplus
}
#endif
