# ESP32-P4 C6 firmware installer

A one-time installer for the firmware two ESP32-P4 touch panels need besides your own
sketch. It is made for the boards I publish Arduino support for:

| Board | Its libraries | What this installs |
|---|---|---|
| **Guition JC8012P4A1\*** (10.1", 800×1280) | [JC8012P4A1C_display](https://github.com/chipguyhere/JC8012P4A1C_display), [JC8012P4A1C_board](https://github.com/chipguyhere/JC8012P4A1C_board) | Touch firmware, and the ESP32-C6 radio's firmware |
| **Waveshare ESP32-P4-86-Panel-ETH-2RO** or **ESP32-P4-WIFI6-Touch-LCD-4B** (4", 720×720, "P486") | [P486Panel_display](https://github.com/chipguyhere/P486Panel_display), [P486Panel_board](https://github.com/chipguyhere/P486Panel_board) | The ESP32-C6 radio's firmware |

On those two boards it shows what it's doing on the screen. It recognizes them by chips
that answer on their I²C bus (the JC8012's clock chip, the P486's touch chip). On
anything else it doesn't touch a display: it runs as a serial-only updater for the C6.

### Other boards: at your own risk

Some other ESP32-P4 boards with an ESP32-C6 radio, wired to it the same way, may work
with the serial-only mode. That is a coincidence, not a feature. **This utility comes with
no warranty whatsoever. Running it on a board it wasn't made for is at your own risk.**
A failed or wrong radio update can leave Wi-Fi unusable until the C6 is reflashed through
its own programming header.

For your own board, the real sources are its maker's documentation and Espressif's:
[ESP-Hosted](https://github.com/espressif/esp-hosted-mcu) (the C6 firmware and how to
update it), [ESP-IDF](https://docs.espressif.com/projects/esp-idf/), and the Arduino
core's own updater example (**File → Examples → ESP_HostedOTA** with an ESP32-P4
selected).

## What it does

- **Touch firmware (JC8012 only).** The JC8012's touch controller (a GSL3680) has no
  memory of its own: every time the board powers up, it needs about 18 KB of firmware
  sent to it. The display libraries send it, but they don't contain it. They read it
  from the board's NVS (a small storage area in flash). The installer puts it there if
  it's missing, and touch starts working. The P486's touch chip needs nothing.
- **Radio firmware (both).** The radio is a separate chip, an ESP32-C6, running
  Espressif's ESP-Hosted firmware, which has to match the Arduino core you build with.
  Boards often arrive with an older version. If the C6's is older than the one this
  installer carries, it offers to update it: tap **Update C6**. A progress bar runs for
  about a minute, then the board restarts and checks again. If the C6 has something
  *newer*, see [below](#which-radio-firmware).

Both stay put through later uploads and power cycles. Only erasing the whole flash
removes the touch firmware - the Arduino IDE's **Erase All Flash Before Sketch Upload**
option does that - and then you run the installer again.

## Installing from the web: one click

No Arduino IDE needed. In **Chrome** or **Edge** on a computer:

1. Plug the board's USB-to-serial port into the computer: on the JC8012 the
   **USB_UART** port next to the CN2 header; on the P486 its programming port.
2. Open **[the ESP32-P4 C6 firmware installer](https://chipguyhere.github.io/esp-launchpad/minimal-launchpad/?flashConfigURL=https://raw.githubusercontent.com/chipguyhere/ESP32-P4-C6-firmware-installer/main/launchpad.toml)**.
3. Click **Connect**. It asks before it writes anything; then pick the board's port.
4. When it's done, the board restarts into the installer. Follow the screen.

It writes only the installer itself. What the board has saved is kept, including the
touch firmware if it's already there.

## Running it from the Arduino IDE

1. Install this library, [LVGL](https://lvgl.io) 9 (from the Library Manager), and
   [**chipguy_P486_or_JC8012_display**](https://github.com/chipguyhere/P486_or_JC8012_display), which drives both boards' screens.
2. Open **File → Examples → chipguy_ESP32-P4-C6-firmware-installer → InstallFirmware**.
3. Board settings: **ESP32P4 Dev Module**, **PSRAM: Enabled**, **Flash Size: 16 MB**,
   **Partition Scheme: Custom**. (Custom uses the `partitions.csv` in the sketch folder,
   the same layout as the display libraries' examples.) Upload.
4. Follow the screen. Everything on it is also printed to the Serial Monitor at 115200
   baud, where you can type instead of tapping: `u` takes the offer, `s` skips it, `a`
   opens Advanced (`y` installs, `n` goes back).
5. When it says **All done**, upload your own sketch.

The touch firmware uses about 10 KB of the 20 KB NVS area, so sketches that store a lot
in NVS themselves (with `Preferences`, for example) could run short of room.

## Which radio firmware

The installer carries two versions of ESP-Hosted for the C6:

- **2.12.13, the default.** It's the version the Arduino ESP32 core 3.3.12 is built
  against: Wi-Fi and Bluetooth in Arduino sketches. ESP-IDF projects built with
  ESP-Hosted 3 work with it too.
- **3.1.0, under Advanced** (the button at the top right). For ESP-IDF projects built with
  ESP-Hosted 3, which want the matching version. Wi-Fi and Bluetooth. **Arduino sketches
  can't use it**: with 3.1.0 on the C6, Wi-Fi and Bluetooth stop working in every Arduino
  sketch. Run the installer again and it offers **Restore 2.12.13** - tested, there and
  back.

What the screen offers depends on what the C6 has now:

| On the C6 | Offered |
|---|---|
| older than 2.12.13, or too old to say | **Update C6** to 2.12.13 |
| 2.12.13 | nothing: up to date |
| 3.1.0 (from Advanced) | **Restore 2.12.13** for Arduino |
| anything else newer than 2.12.13 | **Accept risk** or **Leave it** (below) |

**A newer version this installer doesn't carry.** If the C6 has, say, ESP-Hosted 3.2,
installing 2.12.13 (or 3.1.0 from Advanced) replaces it, and this installer has no copy of
3.2 to put back. Going back from 3.x to 2.x was tested only from 3.1.0. The screen says so,
and you choose: accept the risk, or leave it and use an installer that carries your
version.

ESP-NOW isn't available on these boards either way: ESP-Hosted doesn't carry it to the P4,
and the Arduino core's ESP-NOW library is for chips with their own Wi-Fi.
[extras/c6/](extras/c6/README.md) says exactly how both images were built.

## Why the touch firmware is separate

It's Silead's, from their gslX680 touchscreen driver, and Silead licenses it under the
**GNU GPL version 2**. A program that contains it is covered by the GPL. Keeping it in
this installer means the display libraries stay MIT-licensed, and so can your sketches:
they only read the firmware from NVS, the way an operating system loads a device's
firmware file. The installer itself is GPL because it carries the firmware.

## License

Mixed; see [LICENSE](LICENSE). In every part: **no warranty**, as each license says.

- Touch firmware (`examples/InstallFirmware/gsl3680_firmware_blob.h`, source form in
  [extras/](extras/README.md)): GPL-2.0, [LICENSE-GPL-2.0.txt](LICENSE-GPL-2.0.txt).
- ESP32-C6 firmware (`examples/InstallFirmware/c6_firmware.h`): Espressif's ESP-Hosted,
  Apache-2.0, [LICENSE-Apache-2.0.txt](LICENSE-Apache-2.0.txt).
- The installer code: MIT.
