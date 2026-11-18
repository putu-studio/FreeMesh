# FreeMesh

FreeMesh is a small battery-powered box that lets a group of people chat where there is no phone signal. Turn it on, join its Wi-Fi network from any phone, and open the chat page in the browser. Boxes pass messages to each other over LoRa radio.

No SIM card, subscription or internet connection is needed, and there is nothing to install.

```
  phones ── Wi-Fi ──►  ┌─────────┐   LoRa 915 MHz   ┌─────────┐  ◄── Wi-Fi ── phones
     chat page in      │  Box A  │ ◄──────────────► │  Box B  │      chat page in
     the browser       └─────────┘                  └─────────┘      the browser
                    ESP32-S3 + SX1262            ESP32-S3 + SX1262
```

> **Prototype.** This device isn't certified, and it isn't for sale. Radio rules differ by country, so check the rules where you are before you transmit.

> **Version 0.10.0 builds, but it hasn't been tested on hardware yet.** The previous version, 0.9.0, ran on two prototype boxes.

Project page with pictures: [putu-studio.github.io/projects/freemesh](https://putu-studio.github.io/projects/freemesh/)

## Features

- **Chat page served by the box.** It runs in the phone's browser, in light and dark mode.
- **Text and voice.** Send text to everyone on the network, to a group, to one friend box, or to one person. Voice clips can be up to 10 seconds long and are compressed with Codec2.
- **Friend boxes.** Two boxes become friends after both sides check the same 4-digit code. Each pair of friends gets its own key.
- **Network lock.** Boxes that share a passphrase lock every message with AES-GCM and ignore other boxes.
- **SOS button.** Holding it for 5 seconds sends an alert to other boxes. It is off by default (`SOS_BUTTON_ON` in `firmware/main/board.h`). The alert is not private: anyone in range can read it. It is a prototype feature, so don't rely on it in an emergency.
- **Three programs on one box.** Switch between FreeMesh, Meshtastic and MeshCore from the chat page or with the mode button.
- **Updates over Wi-Fi.** The box downloads a signed update and goes back to the last working version if the new one fails to start.

## Repository layout

| Folder | What's inside |
|---|---|
| [`firmware/main/`](firmware/main/) | The firmware, one file per job, and the chat page (`webapp/index.html`) |
| [`firmware/components/`](firmware/components/) | Codec2 (voice) and a small DNS server |
| [`firmware/tools/`](firmware/tools/) | Flash scripts, the update maker, and the patches for Meshtastic and MeshCore |

## Hardware

| Part | Notes |
|---|---|
| Seeed XIAO ESP32-S3 + Wio-SX1262 kit | The radio board stacks on top of the XIAO |
| 915 MHz antenna | |
| TP4056 charger module + LiPo cell | Optional, for battery use. Use a cell and a charger module that have a protection circuit |
| 2 × 100 kΩ resistors | Optional. A divider from the battery to pin D0 lets the page show the battery level |

Every pin is defined in `firmware/main/board.h`.

## Build and flash

You need [ESP-IDF 6.1](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/).

First copy `firmware/main/secrets.example.h` to `firmware/main/secrets.h`. The build needs that file.

```
cd firmware
idf.py set-target esp32s3
idf.py build
```

Flash FreeMesh to its slot. Replace `PORT` with your serial port.

```
python -m esptool --chip esp32s3 -p PORT write-flash 0x20000 build/firmware.bin
```

The first flash of a new board must also write the bootloader and the flash layout. `idf.py -p PORT flash` does that.

Then join the box's Wi-Fi network and open the chat page in the browser. The box prints the page address on its serial log when it starts.

The Meshtastic and MeshCore programs are not in this repository. `firmware/tools/patches/` holds the patches and the steps to build them from their own source.

## Third-party code

| Part | License |
|---|---|
| [Codec2](https://github.com/drowe67/codec2) in `firmware/components/codec2/` | LGPL-2.1 |
| DNS server component from the ESP-IDF examples in `firmware/components/dns_server/` | Unlicense OR CC0-1.0 |
| Meshtastic patch in `firmware/tools/patches/` | GPL-3.0, like Meshtastic |
| MeshCore patch in `firmware/tools/patches/` | MIT, like MeshCore |

Meshtastic and MeshCore are separate open-source projects. This project isn't affiliated with them. Meshtastic® is a registered trademark of Meshtastic LLC.

## License

[MIT](LICENSE) for the FreeMesh firmware, chat page and tools. The third-party parts above keep their own licenses.
