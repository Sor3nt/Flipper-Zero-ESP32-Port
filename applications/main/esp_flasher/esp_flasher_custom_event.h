#pragma once

/* Starts above every submenu index: a console refresh that lands after the console scene
 * exited must not alias a menu item (0 used to be "Quick Flash" in the start menu). */
typedef enum {
    EspFlasherEventRefreshConsoleOutput = 100,
    EspFlasherEventStartConsole,
    EspFlasherEventStartKeyboard,
    EspFlasherEventStartFlasher,
    EspFlasherEventRefreshSubmenu,
    EspFlasherEventShowAddressInput, // show hex address text input for pending slot
    EspFlasherEventAddrInputDone, // address text input confirmed
    EspFlasherEventShowPartConfirm, // show partition address confirm widget
    EspFlasherEventPartConfirmYes, // user accepted parsed partition addresses
    EspFlasherEventPartConfirmNo, // user declined parsed partition addresses
} EspFlasherCustomEvent;
