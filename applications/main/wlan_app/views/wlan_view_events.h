#pragma once

#include <stdint.h>

/** Zentrale Definition aller Custom-Events der WLAN-App.
 *  Wird sowohl von wlan_app.h als auch von den Custom-Views inkludiert,
 *  damit Views keine Magic-Numbers mehr spiegeln müssen. */
typedef enum {
    WlanAppCustomEventScanComplete = 100,
    WlanAppCustomEventConnectSuccess = 101,
    WlanAppCustomEventConnectFailed = 102,
    WlanAppCustomEventNetworkScanComplete = 103,
    WlanAppCustomEventNetworkScanRetry = 110,
    WlanAppCustomEventNetworkScanCancel = 111,

    WlanAppCustomEventLanItemOk = 130,
    WlanAppCustomEventLanItemLongOk = 131,
    WlanAppCustomEventLanMenuOk = 132,
    WlanAppCustomEventLanPopupDone = 133,

    WlanAppCustomEventApSelected = 140,
    WlanAppCustomEventConnectLongOk = 141,
    WlanAppCustomEventConnectMenuOk = 142,

    WlanAppCustomEventSsidConnect = 105,
    WlanAppCustomEventSsidSelect = 106,
    WlanAppCustomEventPasswordEntered = 107,

    WlanAppCustomEventPortscanOk = 150,

    WlanAppCustomEventHandshakeDeauthStart = 160,
    WlanAppCustomEventHandshakeDeauthStop = 161,
    WlanAppCustomEventDeautherStart = 162,
    WlanAppCustomEventDeautherStop = 163,
    WlanAppCustomEventDeautherTargets = 164,
    WlanAppCustomEventDeautherAuto = 165,
    WlanAppCustomEventDeautherChannelUp = 166,
    WlanAppCustomEventDeautherChannelDown = 167,
    WlanAppCustomEventHandshakeChannelUp = 168,
    WlanAppCustomEventHandshakeChannelDown = 169,
    WlanAppCustomEventHandshakeChannelConfirm = 182,
    WlanAppCustomEventHandshakeChannelCancel = 183,
    WlanAppCustomEventHandshakeSettingsOpen = 184,
    WlanAppCustomEventHandshakeSavePathEntered = 185,
    WlanAppCustomEventHandshakeAutoToggle = 188,

    WlanAppCustomEventEvilPortalSsidEntered = 170,
    WlanAppCustomEventEvilPortalCapturedBack = 171,
    WlanAppCustomEventEvilPortalCredCaptured = 172,
    WlanAppCustomEventEvilPortalCredsValid = 173,
    WlanAppCustomEventEvilPortalTogglePause = 174,
    WlanAppCustomEventEvilPortalBridgeSsidEntered = 175,
    WlanAppCustomEventEvilPortalBridgePwdEntered = 176,

    WlanAppCustomEventMitmMenuInjectCode = 191,
    WlanAppCustomEventMitmMenuStart = 193,
    WlanAppCustomEventMitmInjectCodeEntered = 194,
    WlanAppCustomEventMitmMenuPayloadChanged = 195,

    WlanAppCustomEventUpdateSdStart = 200,
    WlanAppCustomEventUpdateSdCancel = 201,
    WlanAppCustomEventUpdateSdFinished = 202,

    WlanAppCustomEventSmartDeauthToggle = 210,
    WlanAppCustomEventSmartDeauthChannelUp = 211,
    WlanAppCustomEventSmartDeauthChannelDown = 212,

    WlanAppCustomEventAirSnitchProbeDone = 220,

    WlanAppCustomEventWebFsSelectWifi = 230,
    WlanAppCustomEventWebFsDedicatedAp = 231,
    WlanAppCustomEventWebFsApSsid = 232,
    WlanAppCustomEventWebFsApPassword = 233,
    WlanAppCustomEventWebFsApStart = 234,
    WlanAppCustomEventWebFsInputDone = 235,
    WlanAppCustomEventWebFsStop = 236,

    WlanAppCustomEventSmbInputDone = 240,
    WlanAppCustomEventSmbRetry = 241,

    WlanAppCustomEventFwUpdateStart = 250,
    WlanAppCustomEventFwUpdateCancel = 251,
    WlanAppCustomEventFwUpdateInstall = 252,
    WlanAppCustomEventFwUpdateFinished = 253,
    WlanAppCustomEventFwUpdateSkip = 254,

    WlanAppCustomEventAndroidTvPinDone = 260,
    WlanAppCustomEventAndroidTvRetry = 261,
} WlanAppCustomEvent;
