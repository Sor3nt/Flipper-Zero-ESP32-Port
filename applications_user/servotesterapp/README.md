# Servo tester app for Flipper Zero

https://user-images.githubusercontent.com/8887221/228034391-aa9df44c-dcf7-4999-9d22-590cc7ac0392.mp4

This app aims to replicate the behavior of a standard [RC servo](https://en.wikipedia.org/wiki/Servo_(radio_control)) tester, by replicating a [PWM RC servo signal](https://en.wikipedia.org/wiki/Servo_control). You can read more about it in [this blog](https://mhasbini.com/blog/servo-tester-flipper-zero.html)!

## Usage

- Connect the Servo PWM input to **A7** Flipper Zero in.
- Enable 5V output in the GPIO menu or plug-in USB-C charging cable. (If you're supplying power from an external power source, make sure it has common ground with the flipper GND)
- Start the ServoTester app
- Test different servo positions

## Controls

- Left/Right: change angle in manual mode.
- Up/Down: change mode

The app has three modes:

- Manual: left increase 10°, right decrease 10°.
- Center: move to 90°.
- Sweep: move between 0° & 180° every 1 second.

The indicated angle range is: 0° to 180°, but the actual servo angle may differ, because not every servo has a 180° range. 


## Building

Install [uFBT - micro Flipper Build Tool](https://github.com/flipperdevices/flipperzero-ufbt) to get a toolkit for building flipper zero applications. 

- Clone this git repository
- cd to *ServoTesterApp* content.
- run *ufbt && ufbt launch* with flipper connected. It automatically installs the compiled application into Flipper Zero.

## Installing

Drop the compiled *servotesterapp.fap* from *./dist* into */ext/apps/GPIO/* on the flipper. 


## ESP32 port (paradoxt)

Vendored from kaklik/ServoTesterApp v0.2 (commit 7f3eb5f, MIT; app by M. Hasbini), the
`servotesterapp` entry of the Flipper application catalog. Built only for boards with the
external GPIO header (paradoxt); other boards fail at compile time.

- **Signal pin:** header GPIO4 by default (upstream: A7). **OK** moves the signal to the next
  header pin (GPIO4 -> GPIO5 -> GPIO11 -> GPIO12 -> GPIO42); the bottom-right line shows
  `OK>GPIOn`, or `GPIOn busy` when the pin is taken (IR external TX, USB-UART control line).
- **Pulse range:** 0..180 deg = 500..2500 us, so Center (90 deg) is the 1500 us neutral.
  Upstream used 600..2600 us, which put Center at 1600 us. The pulse is shown under the angle.
- **Controls:** Right +10 deg, Left -10 deg (Manual; hold to repeat), Up/Down change mode,
  OK next pin, Back exits. One action per click (the paradoxt driver doubles every click,
  which made the angle jump 20 deg and Up/Down skip a mode).
- **5V:** on battery the app turns the charger boost (OTG) on and off again on exit (upstream
  left 5V to the GPIO menu); on USB the 5V pin already has VBUS. Large servos: use an external
  supply, common GND.
- **Signal:** 50 Hz from LEDC via `furi_hal_pwm_set_pulse()` (needs a firmware with that
  export), 1.22 us resolution. Sweep now drives the pin from the first moment (upstream kept
  the old pulse until the first tick) and the screen follows the sweep.

Build: `./buildFap.sh applications_user/servotesterapp` -> `build/fap/servotesterapp.fap`, copy to
`/ext/apps/GPIO/`.
