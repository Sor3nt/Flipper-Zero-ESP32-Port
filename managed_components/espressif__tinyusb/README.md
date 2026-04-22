# Espressif TinyUSB component

[![Component Registry](https://components.espressif.com/components/espressif/tinyusb/badge.svg)](https://components.espressif.com/components/espressif/tinyusb)
![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)
[![ESP-IDF USB Device examples](https://github.com/espressif/tinyusb/actions/workflows/build_and_run_idf_examples.yml/badge.svg?branch=release%2Fv0.19)](https://github.com/espressif/tinyusb/actions/workflows/build_and_run_idf_examples.yml)

## Overview

This repository is Espressif’s maintained fork of [TinyUSB](https://github.com/hathach/tinyusb), integrated with the ESP-IDF build system. It exists to provide ESP users with timely TinyUSB updates, as upstream releases occur too infrequently to match Espressif’s hardware cadence. All fixes and features developed in this repository are submitted upstream to maintain long-term alignment with the official TinyUSB project and minimize divergence between the two codebases.

Only Device part of TinyUSB stack is supported. For Host mode, please refer to [espressif/usb](https://components.espressif.com/components/espressif/usb) component.

#### Versioning and branching

TinyUSB has not yet reached its first stable release (v1.0.0), so breaking changes may still occur in any version. In practice, the API remains relatively stable, and when incompatible changes arise, we aim to preserve backward compatibility where feasible.

For each upstream release tag (e.g., `0.18.0`), a corresponding branch is created (e.g., `release/v0.18`). Ongoing development continues on this branch, occasionally synchronized with upstream’s `master` branch. Espressif release versions follow a revision-based suffix scheme: for example, `0.18.0~2` represents an intermediate commit derived from the upstream 0.18.0 release.

#### Examples

USB Device examples based on TinyUSB are present in [esp-idf](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/device).

## How to use

There are two options of using TinyUSB component with Espressif's SoCs:

### 1. Use component via [esp_tinyusb](https://components.espressif.com/components/espressif/esp_tinyusb)

[Espressif TinyUSB additions](https://github.com/espressif/esp-usb/tree/master/device/esp_tinyusb) (esp_tinyusb) provide several preconfigured features to use benefits of TinyUSB stack faster.

To use [Espressif TinyUSB additions](https://github.com/espressif/esp-usb/tree/master/device/esp_tinyusb), add ``idf_component.yml`` to your main component with the following content::

```yaml
## IDF Component Manager Manifest File
dependencies:
  esp_tinyusb: "^2.0.0" # Automatically update minor releases
```

Or simply run:
```sh
idf.py add-dependency "esp_tinyusb^2.0.0"
```

Then, the Espressif TinyUSB component will be added automatically during resolving dependencies by the component manager.

### 2. Using standalone TinyUSB

Use this option for custom TinyUSB applications.
In this case you will have to provide configuration header file ``tusb_config.h``. More information about TinyUSB configuration can be found [in official TinyUSB documentation](https://docs.tinyusb.org/en/latest/reference/getting_started.html).

You will also have to tell TinyUSB where to find the configuration file. This can be achieved by adding following CMake snippet to your main component's ``CMakeLists.txt``:

```cmake
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE path_to_your_tusb_config)
```

Again, you can add this component to your project by adding ``idf_component.yml`` file:

```yaml
## IDF Component Manager Manifest File
dependencies:
  tinyusb: "~0.19.0" # TinyUSB does not guarantee backward compatibility
```

Or simply run:
```sh
idf.py add-dependency "tinyusb~0.19.0"
```

README from the upstream TinyUSB can be found in [hathach/tinyusb/README](https://github.com/hathach/tinyusb/blob/master/README.rst).
