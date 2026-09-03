# MyBar Over-the-Air Firmware Update

Starting with firmware 1.96 the MyBar board can receive a new firmware image from the mobile app over Bluetooth Low Energy (BLE), verify it, and reboot into it. No USB cable is needed after the first flash.

This document covers:

1. How the update works on the board
2. The BLE protocol the mobile app must implement
3. How to build and publish a new firmware image
4. Error codes and troubleshooting
5. Implementation notes for firmware developers

---

## 1. How it works

The ESP32 flash is split into two application slots (`app0` and `app1`) plus a small `otadata` record that says which slot boots. The running firmware always lives in one slot. During an update the incoming image is written into the *other* slot. Only when the whole image has arrived and passed verification is `otadata` switched and the board rebooted. If anything fails at any point, the current firmware is untouched and keeps running.

```
  App (BLE central)                          MyBar board (BLE peripheral)
  ─────────────────                          ───────────────────────────
  otaStart:<size>[:<md5>]   ──────────────►  erase the inactive slot
                            ◄──────────────  ota:ready:<maxChunk>
  raw bytes (chunk 1)       ──────────────►  Update.write()
  raw bytes (chunk 2)       ──────────────►  Update.write()
  ...                       ◄──────────────  ota:<received>/<total>   (every ~5 %)
  raw bytes (last chunk)    ──────────────►  Update.write()
  otaEnd                    ──────────────►  Update.end()  → verify image (+MD5)
                            ◄──────────────  ota:done
                                             ESP.restart() after 0.5 s
```

The whole transfer runs over the existing MyBar BLE service. Text control commands use the same RX characteristic as every other MyBar command, so the app can reuse its command channel. Firmware bytes go to a new, dedicated binary characteristic so they never collide with the text parser.

---

## 2. BLE protocol

### 2.1 GATT layout

| Item | UUID | Properties | Used for |
|---|---|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | | MyBar UART service (unchanged) |
| RX characteristic | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write | Text commands, including the `ota*` commands |
| TX characteristic | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify | Text replies, including `ota:*` replies |
| **OTA characteristic** | `6E400004-B5A3-F393-E0A9-E50E24DCCA9E` | Write, Write Without Response | **Raw firmware bytes** (new in 1.96) |

Notifications must be enabled on the TX characteristic (write `0x01` to its CCCD descriptor) before starting an update, otherwise the app receives no replies.

### 2.2 MTU and chunk size

The board requests an MTU of 517 bytes, the largest the ESP32 stack supports. The app should negotiate the largest MTU the phone allows (on Android call `requestMtu(517)`; iOS negotiates automatically). The usable payload per write is `MTU - 3`.

The board reports the exact value to use in the `ota:ready:<maxChunk>` reply. The app must never write more than `maxChunk` bytes in a single write. Chunks may be smaller. The last chunk is usually short.

With the default 23 byte MTU the payload is only 20 bytes and a 1.1 MB image takes several minutes. With 517 bytes it takes roughly 20 to 40 seconds.

### 2.3 Commands and replies

All commands are plain ASCII with no terminator over BLE (over the serial port they end with `\n`). All replies arrive as notifications on the TX characteristic and start with `ota:`.

| App sends | Board replies | Meaning |
|---|---|---|
| `otaStart:<size>` | `ota:ready:<maxChunk>` | Start an update. `size` is the exact byte length of the `.bin` file. The board erases the inactive slot, which can take up to a few seconds before the reply arrives. |
| `otaStart:<size>:<md5>` | `ota:ready:<maxChunk>` | Same, and the board also checks the MD5 (32 hex characters) of the received image before accepting it. **Recommended.** |
| `otaStart` | `ota:ready:<maxChunk>` | Size unknown. Allowed but progress replies show `?` as the total and the overflow check is disabled. Avoid in production. |
| *(raw bytes on OTA characteristic)* | `ota:<received>/<total>` | Progress. Sent roughly every 5 % and on the final byte. Not sent for every chunk. |
| `otaEnd` | `ota:done` | Image complete and verified. The board reboots about 500 ms later; expect a BLE disconnect. |
| `otaEnd` | `ota:error:<reason>` | Image rejected. Old firmware keeps running, state is reset, the app may retry from `otaStart`. |
| `otaAbort` | `ota:aborted` | Cancel a running update. |
| `otaStatus` | `ota:<received>/<total>` or `ota:idle` | Query progress. |
| *any other command during an update* | `ota:busy` | Motor and settings commands are refused while flash is being written. |

### 2.4 Automatic aborts

The board cancels the update by itself and replies `ota:error:<reason>` when:

| Reason | Trigger |
|---|---|
| `timeout` | No chunk received for 30 seconds |
| `disconnected` | The BLE client dropped the connection (reply cannot be delivered, but the state is reset) |
| `overflow` | More bytes arrived than the declared size |

After any abort the app simply starts again with `otaStart`.

### 2.5 Recommended app flow

1. Connect, discover the service, enable notifications on TX.
2. Request a large MTU and wait for the negotiation to complete.
3. Compute the MD5 of the `.bin` file and send `otaStart:<size>:<md5>`.
4. Wait for `ota:ready:<maxChunk>`. If you get `ota:error:...` show it and stop.
5. Loop over the file in `maxChunk` slices and write each one to the OTA characteristic.
   * With *Write Without Response* you can stream fast, but keep a small in-flight window (for example wait for the BLE stack's "ready to write" callback) so you do not overrun the phone's buffer.
   * With *Write With Response* the flow control is built in but the transfer is slower.
6. Show progress from the `ota:<received>/<total>` notifications, or from your own byte counter.
7. Send `otaEnd` and wait for `ota:done`.
8. Expect the connection to drop. Reconnect after 3 to 5 seconds and send `firmwareVersion` to confirm the new version is running.

### 2.6 Example session

```
→ otaStart:1121312:be65d3ace555077fba1de039f60b73a7
← ota:ready:514
→ [514 bytes] × 2181, then [126 bytes]
← ota:56064/1121312
← ota:112128/1121312
   ...
← ota:1121312/1121312
→ otaEnd
← ota:done
   (disconnect, reboot, reconnect)
→ firmwareVersion
← 1.97
```

---

## 3. Building and publishing a firmware image

### 3.1 Prerequisites

* Arduino IDE 2.x or `arduino-cli` with the **esp32** core 3.x installed (the sketch uses `ledcAttach`, which needs core 3).
* Libraries: `Streaming`, `QueueList`, `L9110 Motor driver`. `BLE`, `EEPROM` and `Update` ship with the core.
* Board: **ESP32 Dev Module** (`esp32:esp32:esp32`).
* Partition scheme: **Default 4MB with spiffs**. This gives two 1.25 MB app slots. If the sketch grows past 1,310,720 bytes switch to **Minimal SPIFFS (1.9MB APP with OTA)**. Any scheme without two app slots makes `otaStart` fail with `noOtaPartition`.

### 3.2 Build

**Continuous integration (normal path).** Every push and pull request is compiled by `.github/workflows/firmware.yml`. When a change lands on `main` the workflow creates the git tag `v<firmwareVersion>`, publishes a GitHub release with the binaries and their MD5/SHA-256 sums, and redeploys the web flasher at https://diybar.github.io/mybar_firmware/ (the page also serves `version.json` with the size and MD5 the app needs for `otaStart`). If the version was not bumped the release step is skipped.

Command line (same script CI uses, output in `dist/`):

```
scripts/build.sh
```

Or directly (the sketch folder must carry the sketch name):

```
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=default --libraries Libraries/library --output-dir ./build MyBar_1.96
```

Arduino IDE: *Sketch → Export Compiled Binary*. The files land in the sketch folder under `build/esp32.esp32.esp32/`.

### 3.3 Which file to ship

Release assets are named `mybar-<version>.*`; a local IDE build names them `MyBar_1.96.ino.*`.

| File | Use |
|---|---|
| `mybar-<version>.bin` | **This is the OTA image.** Send it through the app. |
| `mybar-<version>.merged.bin` | Complete flash image (bootloader + partitions + app). Flash it over USB at offset `0x0` for a brand new board, or use the web flasher. |
| `mybar-<version>.bootloader.bin`, `.partitions.bin`, `boot_app0.bin` | Parts of the merged image (offsets `0x1000`, `0x8000`, `0xE000`). Not needed for OTA. |
| `version.json` | Version, byte size and MD5 of the OTA image. Also served next to the web flasher so the app can discover the latest release. |
| `.elf`, `.map` | Debug symbols. Not needed on the device. |

Do **not** send the merged image over OTA. It is 4 MB and would be rejected as `tooBig`.

### 3.4 Release checklist

1. Bump `firmwareVersion` in the sketch.
2. Open a pull request. CI compiles the sketch and attaches the binaries to the workflow run; download them and test on one board if the change touches OTA or BLE.
3. Merge to `main`. CI creates the tag and the GitHub release, and updates the web flasher.
4. Point the app at the new release: read `size` and `md5` from `version.json` (or the release notes) so it can send `otaStart:<size>:<md5>`.
5. Test on one board: update, reconnect, `firmwareVersion` returns the new number.

### 3.5 First installation

The firmware already running on a board must contain the OTA receiver for the app to update it. Boards on 1.95 or earlier have to be flashed once over USB, either with the web flasher at https://diybar.github.io/mybar_firmware/ or with the merged image. From 1.96 onward every later version can go through the app.

---

## 4. Error reference

| Reply | Cause | What to do |
|---|---|---|
| `ota:error:badSize` | Size in `otaStart` was not a positive number | Fix the command string |
| `ota:error:badMd5` | MD5 was not 32 hex characters or was rejected | Send lowercase hex, 32 chars, or omit it |
| `ota:error:noOtaPartition` | Board was flashed with a partition table that has only one app slot | Reflash over USB with an OTA-capable partition scheme |
| `ota:error:tooBig:<slotSize>` | The image is larger than the free app slot | Build with *Minimal SPIFFS* scheme, or reduce the sketch |
| `ota:error:Not Enough Space` and other `Update` library texts | Reported directly by the ESP32 `Update` library | See the text; usually partition size or a flash write failure |
| `ota:error:overflow` | App sent more bytes than declared | Check the size you passed matches the file |
| `ota:error:incomplete:<got>/<total>` | `otaEnd` was sent before all bytes arrived | Make sure every write completed before sending `otaEnd` |
| `ota:error:timeout` | No data for 30 s | Check the app's write loop and BLE connection |
| `ota:error:disconnected` | Connection dropped mid-transfer | Reconnect and restart |
| `ota:error:notStarted` | `otaEnd` sent without an active update | Send `otaStart` first |
| `ota:error:verifyFailed` or MD5 mismatch text | Image did not pass the ESP32 image check or the MD5 | The file was corrupted in transit or is not an ESP32 app image; rebuild and resend |
| `ota:busy` | A non-OTA command was sent during an update | Wait for `ota:done` or send `otaAbort` |

### Troubleshooting

* **Very slow transfer**: the MTU negotiation did not happen. Confirm the app requests a larger MTU and honors the `maxChunk` value.
* **Board never sends `ota:ready`**: notifications on TX are not enabled, or the erase is still running. Erasing 1.25 MB can take 2 to 4 seconds on some flash chips. Wait up to 10 s before giving up.
* **`ota:done` received but the board still reports the old version**: the reboot was interrupted or the app reconnected too fast and read a cached value. Wait 5 s and query again.
* **Board bootloops after an update**: this cannot happen from a verified image, but it can from a `.bin` built for a different partition layout or board. Flash the merged image over USB to recover.

---

## 5. Implementation notes (firmware side)

Files:

* `FirmwareUpdate.h` — all OTA logic. Included from the sketch after the BLE headers.
* `MyBar_1.96.ino` — five small hooks: include, `otaHandleCommand` + `ota:busy` guard at the top of `processNotification`, `otaAbort` in `onDisconnect`, `otaSetupCharacteristic` + `BLEDevice::setMTU` in `setup`, `otaLoop` in `loop`.

Design points:

* **Flash writes happen in the BLE callback.** `otaWriteChunk` is called from `OtaCharacteristicCallbacks::onWrite` on the Bluetooth task. `Update.write` buffers internally and writes whole 4 KB sectors, which is fast enough to keep up with BLE.
* **Reboot is deferred to `loop()`.** `otaEnd` only sets `otaRebootPending`; `otaLoop` calls `ESP.restart()` 500 ms later so the `ota:done` notification has time to leave the radio.
* **State reset is centralized** in `otaReset`, which also calls `Update.abort()` if the library is still active. Every failure path goes through it, so a failed attempt never blocks the next one.
* **Motor commands are blocked** during the update. Flash writes disable the instruction cache briefly; running motors and the queue logic on top of that is avoidable risk. The distance sensor task on core 0 keeps running unaffected.
* **Serial also works** for the text commands (`otaStart`, `otaEnd`, ...) because they pass through `processNotification`, but firmware bytes are only accepted over the BLE characteristic. A serial transport for the bytes would need its own binary framing and is not implemented.
* **Constants** you may want to tune live at the top of `FirmwareUpdate.h`: `OTA_BLE_MTU` (517) and `OTA_TIMEOUT_MS` (30000).

Known issue outside the OTA code: `setup()` declares a local `BLEServer *pServer` that shadows the global one, so the re-advertising path in `loop()` dereferences a null pointer after a disconnect. Remove the type from that line so the global is assigned. This matters after an update because the reboot forces a reconnect.
