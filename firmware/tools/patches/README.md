# Meshtastic + MeshCore: our changes, and how to build them yourself

This folder has **every change we made** to Meshtastic and MeshCore, as patch files.
The folder `tools\bins\` is not part of this repository: build the two programs with these patches, as described below,
and put the results in `tools\bins\` under the names the flash script expects (`meshtastic-freemesh.bin`, `meshcore-freemesh.bin`).
A patch is a plain text file. Lines starting with `-` were removed, and lines starting with `+` were added.
Open it in VS Code to read it.

## All changes
**Meshtastic 2.8.3** (commit b013fab): `meshtastic-2.8.3-b013fab-freemesh.patch`
| File | Change | Why |
|---|---|---|
| `variants/esp32s3/seeed_xiao_s3/platformio.ini` | new env `freemesh-xiao-s3` with `-D FREEMESH` | our own build. Every change below only happens when FREEMESH is set |
| `freemesh_build_mt.csv` | partition table with a 2.5 MB app slot | Meshtastic is 2.29 MB and doesn't fit in 2 MB |
| `src/FSCommon.h` | storage = the "mtfs" area | its settings survive mode switches, and don't mix with MeshCore's |
| `src/platform/esp32/MeshtasticOTA.cpp` | OTA returns "no slot" | the phone app can't overwrite our other modes |
| `src/platform/esp32/FreeMeshReturn.cpp/.h` (new) | hold D1 5 s: LED pulses, then restart in FreeMesh | the way back |
| `src/main.cpp` | calls `freemeshReturnPoll()` in `loop()` | runs the check above |
| `src/modules/StatusLEDModule.cpp` | doesn't blink LED 48 | the LED only pulses while switching |

**MeshCore** (commit d2096bd): `meshcore-d2096bd-freemesh.patch`
| File | Change | Why |
|---|---|---|
| `variants/xiao_s3_wio/platformio.ini` | new env `FreeMesh_Xiao_S3_WIO_companion_radio_ble`, `-D FREEMESH`, no `PIN_STATUS_LED` | our build, no status blinking |
| `freemesh_8MB.csv` | our partition table | same flash layout as FreeMesh |
| `examples/companion_radio/main.cpp` | storage = the "mcfs" area. Hold D1 5 s: LED pulses, then FreeMesh | same reasons as Meshtastic |

## Build them yourself (once: install VS Code + the PlatformIO extension, and git)
`<work folder>` is any empty folder outside this project. `<firmware>` is the full path of this firmware folder.
Turn off line-ending conversion for the two clones (`git config core.autocrlf false` before the checkout), or the patch may not apply.
```
cd <work folder>
git clone https://github.com/meshtastic/firmware.git meshtastic-src
cd meshtastic-src
git checkout b013fab239e2ebfb80e0e611825c3bcba5663ba5
git submodule update --init
git apply <firmware>\tools\patches\meshtastic-2.8.3-b013fab-freemesh.patch
pio run -e freemesh-xiao-s3
```
The result is `.pio\build\freemesh-xiao-s3\firmware-freemesh-xiao-s3-2.8.3.b013fab.bin`. Copy it to `tools\bins\` under the name `meshtastic-freemesh.bin`.

```
cd <work folder>
git clone https://github.com/meshcore-dev/MeshCore.git meshcore-src
cd meshcore-src
git checkout d2096bd90e65cd5213e612811eda0bc214e1ab1c
git apply <firmware>\tools\patches\meshcore-d2096bd-freemesh.patch
pio run -e FreeMesh_Xiao_S3_WIO_companion_radio_ble
```
The result is `.pio\build\FreeMesh_Xiao_S3_WIO_companion_radio_ble\firmware.bin`. Copy it to `tools\bins\` under the name `meshcore-freemesh.bin`.

Then run `.\tools\flash-all.ps1 -Port COMx` (your box's port). It checks that each file fits its slot.

To change something yourself, edit the file in `meshtastic-src` or `meshcore-src` and run `pio run` again.
`git diff` shows all your changes compared to the original, and `git diff > my.patch` saves them as a new patch.
The first build downloads a lot, so it takes 10 to 20 minutes.
