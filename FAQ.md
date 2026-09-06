**All information here are valid for Firmware >= 2.0.0, if you are using an older version you need to update the firmware first via WebFlasher!**

# FAQ
## Common issues
* The original **qFlipper** only gives you the remote display and file access. It cannot flash this firmware; use qT-Embed for updates.
* The **Flipper mobile app** (Android/iOS) works over Bluetooth, but apps from the official app store cannot be installed. They are built for the Flipper's ARM chip, not for the ESP32.
* **Bluetooth and WiFi are mutually exclusive.** While WiFi is enabled, Bluetooth is off (and vice versa). Toggle them in the desktop menu (rotate the wheel to the left).
* While qFlipper mode or USB storage is active, the device is not visible as a serial/esptool port. This is expected. qT-Embed leaves qFlipper mode automatically when you close the app.


## SD card not detected
* Make sure the card is inserted correctly (contacts facing the display).
* The card must be formatted as **FAT32** (exFAT and NTFS are not supported).
* Eject the card, reinsert it and reboot the device.
* If it still fails, try a different card. Some Aliexpress/Cheap cards are not recognized reliably.

## "Database not found"
> The firmware needs the contents of `sdcard.zip` on the SD card. You can get them there in several ways.

**SD card reader on your PC**
> Download `sdcard.zip` from the releases page (https://github.com/Sor3nt/Flipper-Zero-ESP32-Port/releases) and extract it to the **root** of the SD card.

**On the device**
> Go to *Settings → Update Firmware*. This updates the firmware to the latest version **and** copies all required files to the SD card. The device needs a WiFi connection for this.

**qT-Embed / qFlipper**
> Open the file manager, go to *SD Card* and copy the contents of `sdcard.zip` there.
> qT-Embed switches the device into qFlipper mode automatically. If you use the original qFlipper, enable it manually first: go to the desktop, rotate the wheel to the left and select *Enable qFlipper*.

**USB storage**
> Go to the desktop, rotate the wheel to the left and select *USB-Storage*. The SD card shows up as a drive on your PC; copy the contents of `sdcard.zip` there. Leave the mode again when you are done.

**Web filesystem**
> Go to the desktop, rotate the wheel to the left and select *Web-Filesystem*. Open the URL shown on the display in your browser and drop the contents of `sdcard.zip` onto the page.

### Common mistakes
* The files belong directly in the root of the SD card. Do not create an extra `sdcard` folder and do not copy the zip file itself.
* After copying, reboot the device once.

## A feature does not work
> Some features may still have bugs. To track them down we need logs.

**How to get logs**
1. Make sure the device is in normal mode (qFlipper mode and USB storage off, otherwise the serial port is not available). Just plugging it in after a reboot is fine.
2. Open the ESPConnect web app: https://thelastoutpostworkshop.github.io/ESPConnect/ (Chrome or Edge).
3. Click *Connect* and choose the *USB JTAG/serial debug unit*.
4. Open *Serial Monitor* and reproduce the problem.

> Save the output to a file and post it together with a short description of what you did and your firmware version (*Settings → About*).


## Updating the firmware
There are three ways:
* **On the device:** *Settings → Update Firmware* (needs WiFi). Updates the firmware and the SD card files.
* **qT-Embed:** https://github.com/Sor3nt/qT-Embed — connect the device and press *Update*. The firmware is uploaded over USB and installed by the device. *Install from file* flashes a local `furi_esp32.bin`.
* **Web flasher:** https://sor3nt.github.io/interface.html — flashes bootloader, partition table and firmware over USB in the browser (Chrome or Edge). Use this for the first installation or when the device does not boot anymore. If the device is not detected, hold the BOOT button while plugging it in.

> If qT-Embed shows *Full flash* instead of *Update*, your device still uses the old single-app partition layout. Run *Full flash* once (or use the web flasher); after that regular OTA updates work.


## Firmware Offsets
* 0x0: Bootloader (bootloader.bin)
* 0x8000: Partition table (partition-table.bin)
* 0x10000: Firmware (furi_esp32.bin)


## Resources
* Firmware: https://github.com/Sor3nt/Flipper-Zero-ESP32-Port
* qT-Embed: https://github.com/Sor3nt/qT-Embed
* Web flasher: https://sor3nt.github.io/interface.html
* Releases / `sdcard.zip` / BIN Files: https://github.com/Sor3nt/Flipper-Zero-ESP32-Port/releases
