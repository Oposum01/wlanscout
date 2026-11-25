// SPDX-License-Identifier: GPL-2.0-only WITH AdditionRef-wlanscout-linking-exception
// SPDX-FileCopyrightText: 2026 Dr.-Ing. Simon Wunderlich Systementwicklung & Beratung

/*
 * Build configuration: what the board has and what is compiled in.
 *
 * The defaults are for the Waveshare ESP32-C5-WIFI6-KIT (N16R8 or
 * N32R8, 8 MB PSRAM) with a W5500 on SPI. Each value can be changed
 * here or given as a compiler flag (-DENABLE_OTA=0), which wins.
 *
 * Other boards, recommended values:
 *
 *   Seeed Studio XIAO ESP32-C5 (8 MB flash, 8 MB PSRAM, no W5500; its pins
 *   23 to 26 carry I2C and the battery divider):
 *
 *     #define BOARD_NAME      "Seeed Studio XIAO ESP32-C5"
 *     #define BOARD_ID        "xiao"
 *     #define ENABLE_ETHERNET 0
 *     #define STATUS_LED_TYPE LED_PLAIN_LOW      // yellow LED, GPIO 27
 *
 *   and partitions_xiao.csv copied over partitions.csv: two firmware
 *   slots of 3.25 MB, updates from the page over USB, 1.4 MB LittleFS.
 *   In the Arduino IDE: board XIAO_ESP32C5 or ESP32C5 Dev Module with
 *   Flash Size 8MB.
 *
 *   Option, one firmware slot for more LittleFS (4.625 MB):
 *
 *     #define ENABLE_OTA      0
 *
 *   and partitions_xiao_1slot.csv instead. No updates from the page
 *   then: new firmware goes in through the chip's download mode,
 *   Settings -> Sensor -> Update in download mode opens the flasher
 *   (flash.html). tools/build_xiao.sh builds either, the option with
 *   XIAO_ONE_SLOT=1.
 *
 *   Waveshare kit without a W5500 (the phone sensor): the defaults.
 *   Without the module the firmware runs on USB alone; ENABLE_ETHERNET
 *   0 only saves about 300 kB of firmware.
 *
 * In the Arduino IDE in every case: PSRAM Enabled, USB CDC On Boot
 * Disabled (the XIAO's menu starts on Enabled). A build with either
 * wrong stops below with an error.
 */

#pragma once

// Shown in Settings -> Sensor and in /api/system
#ifndef BOARD_NAME
#define BOARD_NAME "Waveshare ESP32-C5-WIFI6-KIT"
#endif

/*
 * Which firmware fits: carried in the image, so the flasher can refuse
 * one for another board, and the folder of its releases on GitHub
 * Pages (firmware/ for this one, firmware/xiao/ for the XIAO).
 * Lower case letters and digits; an image without one counts as
 * "kit" (before 2.8.19).
 */
#ifndef BOARD_ID
#define BOARD_ID "kit"
#endif

/*
 * The W5500, DHCP, NTP and the web server on port 80. Without them
 * the sensor answers on USB only, and the page comes from the static
 * site (see docs/usb.md).
 */
#ifndef ENABLE_ETHERNET
#define ENABLE_ETHERNET 1
#endif

/*
 * Firmware updates from the page, over Ethernet or USB. Needs a
 * partition table with two firmware slots (partitions.csv, 16 MB, or
 * partitions_xiao.csv, 8 MB).
 */
#ifndef ENABLE_OTA
#define ENABLE_OTA 1
#endif

/*
 * IEEE 802.15.4. The C5 has the radio, but whether the IEEE802154
 * component is compiled into the Arduino core build is another
 * matter. Set this to 0 if the link step complains about undefined
 * references to esp_ieee802154_*.
 */
#ifndef ENABLE_IEEE802154
#define ENABLE_IEEE802154 1
#endif

// The W5500 on SPI, used with ENABLE_ETHERNET
#ifndef W5500_SCLK
#define W5500_SCLK 23
#define W5500_MOSI 24
#define W5500_MISO 25
#define W5500_CS   26
#define W5500_RST  4
#define W5500_INT  -1      // not connected, the driver polls over SPI
#endif

// The snapshot button; GPIO 28 is BOOT, held at reset it enters the
// download mode
#ifndef BOOT_BUTTON
#define BOOT_BUTTON 28
#endif

/*
 * The LED that answers a snapshot: green twice, or red once, on an
 * RGB LED; on a single-colour one two short blinks or one long.
 */
#define LED_NONE       0
#define LED_RGB        1   // addressable (WS2812-like), red first
#define LED_PLAIN_HIGH 2   // lit when the pin is high
#define LED_PLAIN_LOW  3   // lit when the pin is low

#ifndef STATUS_LED
#define STATUS_LED 27
#endif

#ifndef STATUS_LED_TYPE
#define STATUS_LED_TYPE LED_RGB
#endif

// ---- Checks of the Arduino IDE settings ----

#ifndef BOARD_HAS_PSRAM
#error "PSRAM is off: set Tools -> PSRAM -> Enabled (the tables need about 2 MB of it at boot)"
#endif

#if ARDUINO_USB_CDC_ON_BOOT
#error "USB CDC On Boot is on: set Tools -> USB CDC On Boot -> Disabled (the USB port carries the API)"
#endif
