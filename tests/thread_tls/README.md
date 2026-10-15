# Furi / ESP-IDF pthread TLS regression

Run from the repository with ESP-IDF 5.4.1 available at `$IDF_PATH` (or the
adjacent `esp-idf` directory):

```sh
python3 tests/thread_tls/test_pthread_coexistence.py
```

Requires a host C compiler supporting AddressSanitizer and UndefinedBehaviorSanitizer.
The test compiles ESP-IDF's real pthread TLS implementation with a small task
storage shim and the production Furi TLS helpers/current-thread getter. It
reproduces the old slot-0 failure in a child process, checks independent Furi
and pthread state, foreign tasks and destruction, and rejects an undersized
SDK configuration. It does not emulate ESP32 scheduling or replace device testing.

## Crash diagnosis and pi3 fix

Both supplied pi2 panic traces stopped in `find_value()` / `pthread_getspecific()`
in `components/pthread/pthread_local_storage.c`, reached through:

`sys_thread_sem_get -> netconn_gethostbyname_addrtype -> lwip_getaddrinfo ->
esp_tls_hostname_to_fd -> esp_http_client_perform -> Flip Pi worker`.

ESP-IDF owns FreeRTOS TLS slot 0 and expects a pthread values list there. Furi
previously stored its thread pointer in that same slot. The invalid dereferences
in the two traces were 0x1400 and 0x1. pi3 moves all Furi identity access to
slot 1, configures two slots, and removes WLAN workarounds that cleared slot 0
around pthread creation. The pthread deletion callback retains ownership of
slot 0. This is a base firmware change: updating the FAP alone is insufficient.

On a device, confirm firmware 2.0.0-pi3, then exercise Connect Wi-Fi, Terminal,
repeated reconnect/exit, and the WLAN scan applications. Preserve a new serial
panic log if a failure remains. The firmware and host regression can be checked
here; physical T-Embed and Pi validation must still be performed on hardware.
