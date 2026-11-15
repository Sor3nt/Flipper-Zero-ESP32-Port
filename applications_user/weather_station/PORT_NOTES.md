

## Fix 2026-11-15 -- internal radio
`cc1101_ext` on this firmware is only an alias for the internal chip and always reports connected,
so "prefer external" always chose it (OTG boost on, `furi_hal_subghz_init()` re-run, and
`furi_hal_subghz_shutdown()` on the internal radio at exit). Now `SubGhzRadioDeviceTypeInternal`.
`WeatherStationApp` and `txrx` are zeroed after `malloc` (ESP-IDF malloc does not zero; upstream
passed `txrx->radio_device` to the loader before setting it).
