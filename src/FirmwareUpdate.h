// FirmwareUpdate.h - Over-the-air firmware update received from the mobile app over BLE.
//
// Protocol (text commands go to the existing RX characteristic ...0002 or Serial,
// binary firmware bytes go to the OTA characteristic ...0004):
//
//   App -> Board  "otaStart:<sizeInBytes>"            begin an update, size is the .bin length
//                 "otaStart:<sizeInBytes>:<md5hex>"   same, and verify the MD5 when finished
//   Board -> App  "ota:ready:<maxChunkBytes>"         board is ready, send chunks now
//                 "ota:error:<reason>"                could not start
//
//   App -> Board  <raw bytes>  written to characteristic 6E400004-... (write or write-no-response),
//                 each write is appended to flash in order. Max chunk = MTU - 3 bytes.
//   Board -> App  "ota:<received>/<total>"            progress, roughly every 5 %
//
//   App -> Board  "otaEnd"                            all bytes sent
//   Board -> App  "ota:done"                          image verified, board reboots ~0.5 s later
//                 "ota:error:<reason>"                image rejected, old firmware keeps running
//
//   App -> Board  "otaAbort"                          cancel, board answers "ota:aborted"
//   App -> Board  "otaStatus"                         board answers "ota:<received>/<total>" or "ota:idle"
//
// The update is aborted automatically if no chunk arrives for OTA_TIMEOUT_MS
// or the BLE client disconnects. While an update is running all other commands
// are answered with "ota:busy".
//
// The sketch must be compiled with a partition scheme that has two app slots
// (Arduino IDE: Tools -> Partition Scheme -> "Default 4MB with spiffs" or
// "Minimal SPIFFS (1.9MB APP with OTA)" if the .bin is larger than ~1.2 MB).

#include <Update.h>
#include <esp_ota_ops.h>

#define CHARACTERISTIC_UUID_OTA "6E400004-B5A3-F393-E0A9-E50E24DCCA9E"
#define OTA_BLE_MTU 517                 // largest MTU the ESP32 stack accepts
#define OTA_TIMEOUT_MS 30000UL          // abort if the app stops sending for this long

// Defined in the .ino
void sendNotification(String message);

bool otaInProgress = false;
bool otaRebootPending = false;
unsigned long otaRebootAt = 0;
size_t otaTotalSize = 0;
size_t otaReceived = 0;
size_t otaLastReportedPercent = 0;
unsigned long otaLastChunkMillis = 0;

BLECharacteristic *pOtaCharacteristic = NULL;

void otaSendProgress() {
  if (otaTotalSize > 0) {
    sendNotification("ota:" + String(otaReceived) + "/" + String(otaTotalSize));
  } else {
    sendNotification("ota:" + String(otaReceived) + "/?");
  }
}

// Reset all state; if Update is active it is aborted and the partition left untouched.
void otaReset() {
  if (Update.isRunning()) {
    Update.abort();
  }
  otaInProgress = false;
  otaTotalSize = 0;
  otaReceived = 0;
  otaLastReportedPercent = 0;
  otaLastChunkMillis = 0;
}

void otaAbort(String reason) {
  bool wasRunning = otaInProgress;
  otaReset();
  if (wasRunning) {
    sendNotification(reason.length() > 0 ? "ota:error:" + reason : "ota:aborted");
    if (debug) {
      Serial.println("OTA aborted: " + reason);
    }
  }
}

// args = "<size>" or "<size>:<md5>" (the text after "otaStart:"), may be empty.
void otaBegin(String args) {
  if (otaInProgress) {
    otaReset();
  }

  size_t size = UPDATE_SIZE_UNKNOWN;
  String md5 = "";
  int sep = args.indexOf(':');
  String sizeStr = sep >= 0 ? args.substring(0, sep) : args;
  if (sep >= 0) {
    md5 = args.substring(sep + 1);
    md5.trim();
  }
  sizeStr.trim();
  if (sizeStr.length() > 0) {
    size = (size_t) strtoul(sizeStr.c_str(), NULL, 10);
    if (size == 0) {
      sendNotification("ota:error:badSize");
      return;
    }
  }

  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  if (next == NULL) {
    sendNotification("ota:error:noOtaPartition");
    return;
  }
  if (size != UPDATE_SIZE_UNKNOWN && size > next->size) {
    sendNotification("ota:error:tooBig:" + String(next->size));
    return;
  }

  if (!Update.begin(size, U_FLASH)) {
    sendNotification("ota:error:" + String(Update.errorString()));
    Update.clearError();
    return;
  }
  if (md5.length() == 32) {
    if (!Update.setMD5(md5.c_str())) {
      Update.abort();
      sendNotification("ota:error:badMd5");
      return;
    }
  }

  otaInProgress = true;
  otaTotalSize = size == UPDATE_SIZE_UNKNOWN ? 0 : size;
  otaReceived = 0;
  otaLastReportedPercent = 0;
  otaLastChunkMillis = millis();

  int maxChunk = BLEDevice::getMTU() - 3;
  if (maxChunk < 20) maxChunk = 20;
  sendNotification("ota:ready:" + String(maxChunk));
  if (debug) {
    Serial.printf("OTA started, expecting %u bytes into %s\n", (unsigned) size, next->label);
  }
}

// Called from the BLE write callback with a raw chunk of the .bin file.
void otaWriteChunk(uint8_t *data, size_t len) {
  if (!otaInProgress || len == 0) {
    return;
  }
  if (otaTotalSize > 0 && otaReceived + len > otaTotalSize) {
    otaAbort("overflow");
    return;
  }

  size_t written = Update.write(data, len);
  if (written != len) {
    String err = String(Update.errorString());
    otaAbort(err.length() > 0 ? err : "writeFailed");
    return;
  }

  otaReceived += written;
  otaLastChunkMillis = millis();

  if (otaTotalSize > 0) {
    size_t percent = (otaReceived * 100) / otaTotalSize;
    if (percent >= otaLastReportedPercent + 5 || otaReceived == otaTotalSize) {
      otaLastReportedPercent = percent;
      otaSendProgress();
    }
  }
}

void otaEnd() {
  if (!otaInProgress) {
    sendNotification("ota:error:notStarted");
    return;
  }
  if (otaTotalSize > 0 && otaReceived != otaTotalSize) {
    otaAbort("incomplete:" + String(otaReceived) + "/" + String(otaTotalSize));
    return;
  }

  // end(true) accepts an image shorter than the declared size (only relevant when size unknown)
  bool ok = Update.end(true);
  if (!ok || !Update.isFinished()) {
    String err = String(Update.errorString());
    otaReset();
    sendNotification("ota:error:" + (err.length() > 0 ? err : String("verifyFailed")));
    return;
  }

  otaInProgress = false;
  sendNotification("ota:done");
  if (debug) {
    Serial.println("OTA finished, rebooting");
  }
  // Give the notification time to leave before restarting; loop() performs the reboot.
  otaRebootPending = true;
  otaRebootAt = millis() + 500;
}

void otaStatus() {
  if (otaInProgress) {
    otaSendProgress();
  } else {
    sendNotification("ota:idle");
  }
}

// Handles the text control commands. Returns true if the command was an OTA command.
bool otaHandleCommand(String command) {
  if (command.startsWith("otaStart")) {
    int colon = command.indexOf(':');
    otaBegin(colon >= 0 ? command.substring(colon + 1) : "");
  } else if (command == "otaEnd") {
    otaEnd();
  } else if (command == "otaAbort") {
    otaAbort("");
  } else if (command == "otaStatus") {
    otaStatus();
  } else {
    return false;
  }
  return true;
}

// Call from loop(): watchdog for a stalled transfer and deferred reboot.
void otaLoop() {
  if (otaRebootPending && (long)(millis() - otaRebootAt) >= 0) {
    ESP.restart();
  }
  if (otaInProgress && millis() - otaLastChunkMillis > OTA_TIMEOUT_MS) {
    otaAbort("timeout");
  }
}

class OtaCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *characteristic) {
      otaWriteChunk(characteristic->getData(), characteristic->getLength());
    }
};

// Creates the binary OTA characteristic on the existing UART service.
void otaSetupCharacteristic(BLEService *service) {
  pOtaCharacteristic = service->createCharacteristic(
                         CHARACTERISTIC_UUID_OTA,
                         BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
                       );
  pOtaCharacteristic->setCallbacks(new OtaCharacteristicCallbacks());
}
