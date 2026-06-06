#pragma once

typedef enum {
    DesktopMainEventLock,
    DesktopMainEventOpenLockMenu,
    DesktopMainEventOpenArchive,
    DesktopMainEventOpenFavoriteLeftShort,
    DesktopMainEventOpenFavoriteLeftLong,
    DesktopMainEventOpenFavoriteRightShort,
    DesktopMainEventOpenFavoriteRightLong,
    DesktopMainEventOpenFavoriteOkLong,
    DesktopMainEventOpenMenu,
    DesktopMainEventOpenDebug,
    DesktopMainEventOpenPowerOff,

    DesktopDummyEventOpenLeft,
    DesktopDummyEventOpenDown,
    DesktopDummyEventOpenOk,
    DesktopDummyEventOpenUpLong,
    DesktopDummyEventOpenDownLong,
    DesktopDummyEventOpenLeftLong,
    DesktopDummyEventOpenRightLong,
    DesktopDummyEventOpenOkLong,

    DesktopLockedEventUnlocked,
    DesktopLockedEventUpdate,
    DesktopLockedEventShowPinInput,
    DesktopLockedEventDoorsClosed,

    DesktopPinInputEventResetWrongPinLabel,
    DesktopPinInputEventUnlocked,
    DesktopPinInputEventUnlockFailed,
    DesktopPinInputEventBack,

    DesktopPinTimeoutExit,

    DesktopDebugEventToggleDebugMode,
    DesktopDebugEventExit,

    DesktopLockMenuEventQflipperToggle,
    DesktopLockMenuEventUsbStorage,
    DesktopLockMenuEventBluetoothToggle,
    DesktopLockMenuEventBruce,
    DesktopLockMenuEventMeshModeToggle,
    DesktopLockMenuEventMeshClients,

    DesktopMeshClientsEventPair,
    DesktopMeshClientsEventRemove,
    DesktopMeshClientsEventOpenAction, /* OK kurz auf gepairtem Client → Client-Menü */
    DesktopMeshClientsEventBack,

    /* Client-Menü (mesh_action): Kategorie wählen. */
    DesktopMeshActionEventDevice, /* OK auf "Device" → Device-Scene */
    DesktopMeshActionEventWifi,   /* OK auf "Wifi"   → Wifi-Scene   */
    DesktopMeshActionEventBack,

    /* Device-Scene: Identify start/stop + Disconnect. */
    DesktopMeshDeviceEventIdentifyStart,
    DesktopMeshDeviceEventIdentifyStop,
    DesktopMeshDeviceEventDisconnect,
    DesktopMeshDeviceEventBack,

    /* Wifi-Scene: Capture-Handshake öffnen. */
    DesktopMeshWifiEventCaptureHs,
    DesktopMeshWifiEventBack,

    /* Handshake-Scene: Start/Stop-Button. */
    DesktopMeshHandshakeEventToggle,
    DesktopMeshHandshakeEventBack,

    DesktopUsbStorageEventExit,

    /* Phase-1 Mesh-Events: gefeuert vom Mesh-Service (sowohl Master- als auch
     * Client-Service) und im jeweils zuständigen Scene-Handler verarbeitet.
     * Der Service legt die zugehörigen Daten vorher in desktop->mesh_pending
     * ab. */
    DesktopMeshEventClientPairRequest,    /* Main-Scene  (Client) */
    DesktopMeshEventClientDisconnect,     /* Main-Scene  (Client) */
    DesktopMeshEventMasterDiscoverRsp,    /* Mesh-Clients-Scene (Master) */
    DesktopMeshEventMasterPairRsp,        /* Mesh-Clients-Scene (Master) */
    DesktopMeshEventMasterFeatureList,    /* Mesh-Action-Scene (Master) */
    DesktopMeshEventMasterFeatureStatus,  /* global + Mesh-Action-Scene (Master) */
    DesktopMeshEventMasterResult,         /* global: Result vom Buddy → Overlay + Ack */
    DesktopMeshEventOverlayExpire,        /* global: Overlay-Timer abgelaufen → ausblenden */

    DesktopAnimationEventCheckAnimation,
    DesktopAnimationEventNewIdleAnimation,
    DesktopAnimationEventInteractAnimation,

    DesktopSlideshowCompleted,
    DesktopSlideshowPoweroff,

    DesktopHwMismatchExit,

    DesktopEnclaveExit,

    // Global events
    DesktopGlobalBeforeAppStarted,
    DesktopGlobalAfterAppFinished,
    DesktopGlobalAutoLock,
    DesktopGlobalApiUnlock,
    DesktopGlobalSaveSettings,
    DesktopGlobalReloadSettings,
} DesktopEvent;
