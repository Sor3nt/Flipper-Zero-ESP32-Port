#pragma once

typedef enum {
    // Events with _ are unused in firmware, kept for compatibility
    DesktopMainEventLockWithPin,
    DesktopMainEventOpenLockMenu,
    DesktopMainEventOpenArchive,
    DesktopMainEventOpenFavoriteLeftShort,
    DesktopMainEventOpenFavoriteLeftLong,
    DesktopMainEventOpenFavoriteRightShort,
    DesktopMainEventOpenFavoriteRightLong,
    DesktopMainEventOpenMenu,
    DesktopMainEventOpenDebug,
    DesktopMainEventOpenPowerOff,
    DesktopMainEventOpenFavoriteOkLong,

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
    DesktopLockedEventCoversClosed,
    DesktopLockedEventDoorsClosed = DesktopLockedEventCoversClosed,

    DesktopPinInputEventResetWrongPinLabel,
    DesktopPinInputEventUnlocked,
    DesktopPinInputEventUnlockFailed,
    DesktopPinInputEventBack,

    DesktopPinTimeoutExit,

    DesktopDebugEventDeed,
    DesktopDebugEventWrongDeed,
    DesktopDebugEventSaveState,
    DesktopDebugEventExit,
    DesktopDebugEventToggleDebugMode,

    DesktopLockMenuEventLockPinCode,
    _DesktopLockMenuEventDummyModeOn,
    _DesktopLockMenuEventDummyModeOff,
    DesktopLockMenuEventStealthModeOn,
    DesktopLockMenuEventStealthModeOff,
    DesktopLockMenuEventQflipperToggle,
    DesktopLockMenuEventUsbStorage,
    DesktopLockMenuEventBluetoothToggle,
    DesktopLockMenuEventBruce,

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

    DesktopMainEventLockKeypad,
    DesktopLockedEventOpenPowerOff,
    DesktopLockMenuEventSettings,
    DesktopLockMenuEventLockKeypad,
    DesktopLockMenuEventLockPinOff,
    DesktopLockMenuEventMomentum,
    DesktopLockMenuEventScreenSettings,

    DesktopUsbStorageEventExit,
} DesktopEvent;
