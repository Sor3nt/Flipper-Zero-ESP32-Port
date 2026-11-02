# Flipper servotester application

![Servo tester application screenshot](servotester/assets/Servo_Tester_Manual_mode.png)

This application could be used for testing devices containing standard [RC servo](https://en.wikipedia.org/wiki/Servo_(radio_control)), by generating a [PWM RC servo signal](https://en.wikipedia.org/wiki/Servo_control). 

## Installation instructions

Please install the application from [Flipper Zero application catalog](https://docs.flipper.net/apps), or go to the releases, download apps.zip, and extract it to your SD card.

## Usage

- Connect the Servo PWM input to **A7** Flipper Zero in.
- Enable 5V output in the GPIO menu or plug-in USB-C charging cable. (If you're supplying power from an external power source, make sure it has common ground with the flipper GND)
- Start the Servo Tester app
- Move servo required positions using controls bellow

## Controls

| Button | Action                       |
| :----- | :--------------------------- |
| 🔼     | Up button increases pulse width by 10 us.|
| 🔽     | The down button decreases pulse width by 10 us.   |
| ◀️     | Left button decreases pulse width by 1 us.   |
| ▶️     | The right button increases pulse width by 1 us.  |
| ↩️     | Back button exit application.   |
| 🔵     | Center button change mode. |

## Build instructions

Install [uFBT - micro Flipper Build Tool](https://github.com/flipperdevices/flipperzero-ufbt) to get a toolkit for building flipper zero applications. 

- Clone this git repository
- cd to *ServoTesterApp* content.
- run *ufbt && ufbt launch* with flipper connected. It automatically installs the compiled application into Flipper Zero.


## ESP32 port (paradoxt)

Vendored from ThunderFly-aerospace/flipper-servotester v1.3 (commit 0b89aa9, GPL-3.0), the
`servotester` entry of the Flipper application catalog. Built only for boards with the
external GPIO header (paradoxt); other boards fail at compile time.

- **Signal pin:** header GPIO4 by default (upstream: A7). **Hold OK** (>= 0.6 s) to move the
  signal to the next header pin: GPIO4 -> GPIO5 -> GPIO11 -> GPIO12 -> GPIO42. "Pin busy!" means
  the pin is taken (IR external TX, USB-UART control line): pick another one.
- **5V:** on battery the app turns the charger boost (OTG) on and off again on exit; on USB
  the 5V pin already has VBUS (turning OTG on would stop charging). Status in the bottom-right
  corner: `5V:OTG`, `5V:USB`, `5V:off`. Large servos: use an external supply, common GND.
- **Controls** are the ones documented above, now one step per click (the paradoxt driver
  doubles every click, which made Up/Down jump 50 us and OK skip a mode). Holding an arrow
  repeats its step. Back exits.
- **Signal:** 50 Hz from LEDC via `furi_hal_pwm_set_pulse()` (needs a firmware with that
  export). Resolution 1.22 us (14-bit), so a 1 us step sometimes rounds to the same output.
  Width changes start with the next period; no pulse is cut or stretched.
- Auto mode redraws every 3rd tick (the PWM still moves every 30 ms): a full LCD frame costs
  20-35 ms on the shared SPI bus.

Build: `./buildFap.sh applications_user/servotester` -> `build/fap/servotester.fap`, copy to
`/ext/apps/GPIO/`.
