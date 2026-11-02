#pragma once

/*
 * This app posts NO custom events from the UART worker thread (that would risk the
 * worker blocking on the view-dispatcher queue during shutdown). Worker -> GUI signalling
 * is done with flags in Bw16App (console_dirty / ap_list_done / client_list_done) that the
 * scenes poll from their tick handler, all under app->mutex.
 *
 * The only custom events are submenu selections, posted from the GUI thread by each
 * scene's submenu callback. Each scene offsets its item ids by a private base so a
 * selection is unambiguous:
 *   start          : BW16_MENU_ITEM_BASE   (500)
 *   station_action : BW16_ACT_ITEM_BASE    (100)
 *   ap_list        : BW16_AP_ITEM_BASE     (1000)
 *   client_list    : BW16_CLIENT_ITEM_BASE (2000)
 */
