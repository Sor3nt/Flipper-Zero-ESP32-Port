# Espressif's additions to TinyUSB

[![Component Registry](https://components.espressif.com/components/espressif/esp_tinyusb/badge.svg)](https://components.espressif.com/components/espressif/esp_tinyusb)

This component adds features to TinyUSB that help users with integrating TinyUSB with their ESP-IDF application.

It contains:
* Configuration of USB device and string descriptors
* USB Serial Device (CDC-ACM) with optional Virtual File System support
* Input and output streams through USB Serial Device. This feature is available only when Virtual File System support is enabled.
* Other USB classes (MIDI, MSC, HID…) support directly via TinyUSB
* VBUS monitoring for self-powered devices
* SPI Flash or sd-card access via MSC USB device Class.

## How to use?

This component is distributed via [IDF component manager](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-component-manager.html). Just add `idf_component.yml` file to your main component with the following content:

``` yaml
## IDF Component Manager Manifest File
dependencies:
  esp_tinyusb: "~1.0.0"
```

Or simply run:
```
idf.py add-dependency esp_tinyusb~1.0.0
```

## Documentation

Hardware-related documentation could be found in [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-reference/peripherals/usb_device.html).

### Device Stack Structure

The Device Stack is built on top of TinyUSB and provides:

- Custom USB descriptor support
- Serial device (CDC-ACM) support
- Standard stream redirection through the serial device
- Storage media support (SPI-Flash and SD-Card) for USB MSC Class
- A dedicated task for TinyUSB servicing

### Configuration Options

Configure the Device Stack using `menuconfig`:

- TinyUSB log verbosity
- Device Stack task options
- Default device/string descriptor options
- Class-specific options

### Descriptor Configuration

Configure USB descriptors using the `tinyusb_config_t` structure:

- `device_descriptor`
- `string_descriptor`
- `configuration_descriptor` (full-speed)
- For high-speed devices: `fs_configuration_descriptor`, `hs_configuration_descriptor`, `qualifier_descriptor`

If any descriptor field is set to `NULL`, default descriptors (based on menuconfig) are used.

### Installation

Install the Device Stack by calling `tinyusb_driver_install` with a `tinyusb_config_t` structure. Members set to `0` or `NULL` use default values.

```c
const tinyusb_config_t partial_init = {
  .device_descriptor = NULL,
  .string_descriptor = NULL,
  .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
  .fs_configuration_descriptor = NULL,
  .hs_configuration_descriptor = NULL,
  .qualifier_descriptor = NULL,
#else
  .configuration_descriptor = NULL,
#endif
};
```

### Self-Powered Device

Self-powered devices must monitor VBUS voltage. Use a GPIO pin with a voltage divider or comparator to detect VBUS state. Set `self_powered = true` and assign the VBUS monitor GPIO in `tinyusb_config_t`.

### USB Serial Device (CDC-ACM)

If enabled, initialize the USB Serial Device with `tusb_cdc_acm_init` and a `tinyusb_config_cdcacm_t` structure:

```c
const tinyusb_config_cdcacm_t acm_cfg = {
  .usb_dev = TINYUSB_USBDEV_0,
  .cdc_port = TINYUSB_CDC_ACM_0,
  .rx_unread_buf_sz = 64,
  .callback_rx = NULL,
  .callback_rx_wanted_char = NULL,
  .callback_line_state_changed = NULL,
  .callback_line_coding_changed = NULL
};
tusb_cdc_acm_init(&acm_cfg);
```

Redirect standard I/O streams to USB with `esp_tusb_init_console` and revert with `esp_tusb_deinit_console`.

### USB Mass Storage Device (MSC)

If enabled, initialize storage media for MSC:

**SPI-Flash Example:**
```c
static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle) {
  // ... partition and mount logic ...
}
storage_init_spiflash(&wl_handle);

const tinyusb_msc_spiflash_config_t config_spi = {
  .wl_handle = wl_handle
};
tinyusb_msc_storage_init_spiflash(&config_spi);
```

**SD-Card Example:**
```c
static esp_err_t storage_init_sdmmc(sdmmc_card_t **card) {
  // ... SDMMC host and slot config ...
}
storage_init_sdmmc(&card);

const tinyusb_msc_sdmmc_config_t config_sdmmc = {
  .card = card
};
tinyusb_msc_storage_init_sdmmc(&config_sdmmc);
```

### MSC Performance Optimization

- **Single-buffer approach:** Buffer size is set via `CONFIG_TINYUSB_MSC_BUFSIZE`.
- **Performance:** SD cards offer higher throughput than internal SPI flash due to architectural constraints.

**Performance Table (ESP32-S3):**

| FIFO Size | Read Speed | Write Speed |
|-----------|------------|-------------|
| 512 B     | 0.566 MB/s | 0.236 MB/s  |
| 8192 B    | 0.925 MB/s | 0.928 MB/s  |

**Performance Table (ESP32-P4):**

| FIFO Size | Read Speed | Write Speed |
|-----------|------------|-------------|
| 512 B     | 1.174 MB/s | 0.238 MB/s  |
| 8192 B    | 4.744 MB/s | 2.157 MB/s  |
| 32768 B   | 5.998 MB/s | 4.485 MB/s  |

**Performance Table (ESP32-S2, SPI Flash):**

| FIFO Size | Write Speed |
|-----------|-------------|
| 512 B     | 5.59 KB/s   |
| 8192 B    | 21.54 KB/s  |

**Note:** Internal SPI flash is for demonstration only; use SD cards or external flash for higher performance.

## Examples
You can find examples in [ESP-IDF on GitHub](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/device).
