/*
   Video: https://www.youtube.com/watch?v=oCMOYS71NIU
   Based on Neil Kolban example for IDF: https://github.com/nkolban/esp32-snippets/blob/master/cpp_utils/tests/BLE%20Tests/SampleNotify.cpp
   Ported to Arduino ESP32 by Evandro Copercini, with some additional code by pcbreflux

   Create a BLE server that, once we receive a connection, will send periodic notifications.
   The service advertises itself as: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
   Has a characteristic of: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E - used for receiving data with "WRITE" 
   Has a characteristic of: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E - used to send data with  "NOTIFY"

   The design of creating the BLE server is:
   1. Create a BLE Server
   2. Create a BLE Service
   3. Create a BLE Characteristic on the Service
   4. Create a BLE Descriptor on the characteristic
   5. Start the service.
   6. Start advertising.

   Other Libreries:
   Included in the GitHub repository https://github.com/diybar/firmware/Libraries
*/
#include <Arduino.h>

int boardOrder = 1;
const int motorsPerBoard = 9;
int connectedMotors = 9;
bool bluetooth = true;
int maxMotorsRunning = 2;
const String firmwareVersion = "1.96";
const double boardVersion = 1.92;
bool debug = false;
bool debugLoop = false;
bool serialConnectionEnable = true;
bool distanceSensor = true;
bool reverseMotors = false;
bool isCommercial = false;
bool qrCodeReader = false;
const int minGlassDistance = 780; // 900 for wine glass.
const int blinkTimes = 10; //Number if blinks if there is no glass
int fadeAmount = 10;    // how many points to fade the LED by
String readValue;
int btBuffer = 99; // Original value = 22

#include "EEPROM.h"
#define EEPROM_SIZE 6

// use 13 bit precission for LEDC timer
#define LEDC_TIMER_13_BIT 13

// use 5000 Hz as a LEDC base frequency
#define LEDC_BASE_FREQ 5000

// fade LED PIN (replace with LED_BUILTIN constant for built-in LED)
#define LED_PIN 12

//HardwareSerial Serial(1);
//#define Serial_Txd_pin 1
//#define Serial_Rxd_pin 3

bool inProgress = false;
bool backwardsOn = false;
int blinkedTimes = 0;
int brightness = 0; 
bool ledOn = false;
bool turnLedOn = false;
bool turnLedOff = false;
bool ledBlink = false;

// Arduino like analogWrite
// value has to be between 0 and valueMax
void ledcAnalogWrite(uint8_t channel, uint32_t value, uint32_t valueMax = 255) {
  // calculate duty, 8191 from 2 ^ 13 - 1
  uint32_t duty = (8191 / valueMax) * min(value, valueMax);

  // write duty to LEDC
  ledcWrite(channel, duty);
}

//Multiple cores
#include <Streaming.h>      // Ref: http://arduiniana.org/libraries/streaming/
#include "Workload.h"
#include "Task1.h"
TaskHandle_t TaskA;
#include <QueueList.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "L9110Driver.h"
#include <vector>
#include "esp_bt_device.h"
#include "FirmwareUpdate.h"   // OTA update over BLE (see protocol notes inside)

QueueList <String> motorsQueue;

std::vector<L9110_Motor> motor(motorsPerBoard);
int timeToCompletion[50]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
int motorsRunning = 0;

BLEServer* pServer = NULL;
BLECharacteristic *pCharacteristic;
BLEDescriptor *pDescriptor;
bool deviceConnected = false;
bool deviceNotifying = false;
bool bluetoothAdvertising = true;
String notification;
uint8_t value = 0;

// Reset board
void(* resetFunc) (void) = 0;
 
void sendDeviceAddress() {
  const uint8_t* point = esp_bt_dev_get_address();
  String btId = "";
  for (int i = 0; i < 6; i++) {
    char str[3];
    sprintf(str, "%02X", (int)point[i]);
    btId+=str; 
    if (i < 5){
      btId+=":";
    }
  }
  sendNotification("b*:"+btId);
}

void sendBTNotification(String message) {
    if (deviceConnected && deviceNotifying) {
        char charBuf[btBuffer];
        String(message).toCharArray(charBuf, btBuffer);
        pCharacteristic->setValue(charBuf);
        pCharacteristic->notify();
    }
}

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/

#define SERVICE_UUID               "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" // UART service UUID
#define CHARACTERISTIC_UUID_RX     "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX     "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

void startLedBlink() {
    brightness = 0;
    blinkedTimes = 0;
    ledBlink = true;
}

void initializeMotors() {
  if (!reverseMotors) {
    motor[0].initialize(32, 33);
    motor[1].initialize(25, 26);
    motor[2].initialize(27, 14);
    motor[3].initialize(13, 23);
    if (!serialConnectionEnable && boardVersion <= 1.91) {
      if (boardVersion == 1.91) {
        motor[4].initialize(21, 3);
      } else {
        motor[4].initialize(1, 3);
      }
    } else if (boardVersion > 1.91){
      motor[4].initialize(21, 22);
    }
    motor[5].initialize(19, 18);
    motor[6].initialize( 5, 17);
    motor[7].initialize(16, 4);
    motor[8].initialize(15, 2);
  } else {
    motor[0].initialize(33, 32);
    motor[1].initialize(26, 25);
    motor[2].initialize(14, 27);
    motor[3].initialize(23, 13);
    if (!serialConnectionEnable && boardVersion <= 1.91) {
      if (boardVersion == 1.91) {
        motor[4].initialize(3, 21);
      } else {
        motor[4].initialize(3, 1);
      }
    } else if (boardVersion > 1.91){
      motor[4].initialize(22, 21);
    }
    motor[5].initialize(18, 19);
    motor[6].initialize(17, 5);
    motor[7].initialize(4, 16);
    motor[8].initialize(2, 15);
  }
}

// setMotors() Command String = motor_number-dirrection-duration 
// motor 0 - 8 for pumps.
// direction f (forward) or b (backward).
// duration in miliseconds, if duration = 0 it will run until a stop command is send.  

void setMotors(String command) {    
    bool motorStarted = false;
    int index = command.indexOf('-');
    int secondIndex = command.indexOf('-', index + 1);

    char firstChar = command.charAt(0);
    int motorNumber = atoi(command.substring(0, index).c_str());
    String motorDirection = command.substring(index + 1, secondIndex);
    long duration = secondIndex > 0 ? atoi(command.substring(secondIndex + 1).c_str()) : 0; 

    if (debug) {
      Serial.printf("Motor %d\n", motorNumber);
      Serial.printf("Motor Direction %s\n", motorDirection);
      Serial.printf("Second Index %d\n", secondIndex);
      Serial.printf("Motor duration %d\n", duration);
    }

    if (motorNumber < connectedMotors && isDigit(firstChar) && !(serialConnectionEnable && boardVersion <= 1.91 && motorNumber == 4)) {  
      if (motorDirection == "s") {
        if (isMotorInBoard(motorNumber)) {   
          motor[motorNumber].run (BRAKE);
        } else {
          Serial.println("^" + String(motorNumber) + "-" + motorDirection);
        }
        if (motorsRunning > 0) {
          motorsRunning = motorsRunning - 1;
        }
        sendNotification("stop:" + String(motorNumber));
        if (debug) {
            Serial.printf("Motor %d stopped\n", motorNumber);
        }
      } else if (motorsRunning < maxMotorsRunning) {
          if (motorDirection == "b") {
              if (isMotorInBoard(motorNumber)) {
                motor[motorNumber].run (BACKWARD | RELEASE);
              } else {
                Serial.println("^" + String(motorNumber) + "-" + motorDirection);
                Serial.println("^" + String(motorNumber) + "-" + motorDirection);
              }
              motorStarted = true;
              backwardsOn = true;  
          } else {   
              if ((glassDistance < minGlassDistance && glassDistance > 0) || !distanceSensor) {
                  if (isMotorInBoard(motorNumber)) {
                    motor[motorNumber].run (FORWARD | RELEASE);
                  } else {
                    Serial.println("^" + String(motorNumber) + "-" + motorDirection);
                    Serial.println("^" + String(motorNumber) + "-" + motorDirection);
                  }
                  motorStarted = true;
              } else {
                  sendNotification("noGlass");
                  if (debug) {
                      Serial.println("Glass not ready");
                  }
                  if (!ledBlink) {
                      startLedBlink();
                  }
              } 
          }
          if (motorStarted) {
              if (motorsRunning == 0 && motorsQueue.isEmpty() && debug) {
                  sendNotification("start");
              }
              motorsRunning += 1; 
              if (duration > 0) {
                unsigned long currentMillis = millis();
                timeToCompletion[motorNumber] = duration + currentMillis; 
              }
              if (debug) {
                  Serial.printf("Motor %d started\n", motorNumber);
              }
           }  
        } else {
            //Add pending command to a queue
            motorsQueue.push(command);
        }
    } else {
       if (debug) {
          sendNotification("noMotorNum");
          Serial.println("The motor number doesn't exist");
       }
    }     
}

void stopMotors(bool Notification=false) { 
  if (motorsRunning > 0) {
    // engage the motor's brake
    for (int i = 0; i < connectedMotors; ++i) {
      if (isMotorInBoard(i)) {   
        motor[i].run (BRAKE);
      } else {
        Serial.println("^" + String(i) + "-s");
        Serial.println("^" + String(i) + "-s");
      }
      timeToCompletion[i] = 0;
    }
    motorsRunning = 0;
    if (Notification) {
      sendNotification("motorsStp");
    }
  }
  while (!motorsQueue.isEmpty())
    motorsQueue.pop();
}

bool isMotorInBoard(int motorNumber) {
  return (motorNumber < (motorsPerBoard*boardOrder) && motorNumber >= (motorsPerBoard*(boardOrder-1)));
}

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };

    void onDisconnect(BLEServer* pServer) {
      if (debug) {
          Serial.println("Device disconnected");
      }
      deviceConnected = false;
      deviceNotifying = false;
      bluetoothAdvertising = false;
      // A half-received firmware image is useless; drop it so the old firmware stays intact.
      if (otaInProgress) {
        otaAbort("disconnected");
      }
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string rxValue = pCharacteristic->getValue().c_str();
        if (rxValue.length() > 0) {
            notification = "";
            for (int i = 0; i < rxValue.length(); i++) {
                notification = notification + rxValue[i];
            }
            processNotification(notification);
        }
    }
};

class MyDisCallbacks: public BLEDescriptorCallbacks {
    void onWrite(BLEDescriptor *pDescriptor) {
      uint8_t* rxValue = pDescriptor->getValue();
      if (pDescriptor->getLength() > 0) {
        if (rxValue[0]==1) {
          deviceNotifying=true;
          returnSettings(); 
          if (debug) {
            Serial.println("Notifications enabled");
          }
        } else {
          deviceNotifying=false;
          if (debug) {
            Serial.println("Notifications disabled");
          }
        }
      }
    }
};

void processNotification(String notification) {
  if (debug) {
    Serial.print("Received Value: ");
    Serial.print(notification);
    Serial.println();
  }

  // Firmware update commands (otaStart / otaEnd / otaAbort / otaStatus)
  if (otaHandleCommand(notification)) {
    return;
  }
  // Refuse to run motors or change settings while flash is being rewritten
  if (otaInProgress) {
    sendNotification("ota:busy");
    return;
  }

  int numOfCommands = 1;
  // Check if there is more than 1 command and it ends on "lockOn"
  int lastIndex = notification.lastIndexOf('-');  
  String firstChar = notification.substring(0,1);
  String lastCommand = notification.substring(lastIndex + 1, notification.length());
  if (firstChar == "*") {
    sendNotification(notification);
  } else if (firstChar == "&") {
    if (serialConnectionEnable) {
      boardOrder = atoi(notification.substring(1).c_str());
      String boardReport = "#";
      boardReport += boardOrder;
      connectedMotors = motorsPerBoard*boardOrder;
      distanceSensor = false;
      storeInEEPROM(0, 0);  
      bluetooth = false;    
      storeInEEPROM(2, 0);
      Serial.println(boardReport);
    } 
  } else if (firstChar == "!") {
    boardOrder = 1;
    connectedMotors = motorsPerBoard * boardOrder;
    distanceSensor = true;
    storeInEEPROM(0, 1);  
    bluetooth = true;    
    storeInEEPROM(2, 1); 
    storeInEEPROM(5, connectedMotors);
    returnSettings();
  } else if (firstChar == "#") {
    int numberOfBoards = atoi(notification.substring(1).c_str());
    connectedMotors = motorsPerBoard*numberOfBoards; 
    storeInEEPROM(5, connectedMotors); 
    returnSettings();
  } else if (firstChar == "^") {
    int index = notification.indexOf('-');
    int motorNumber = atoi(notification.substring(1, index).c_str());
    String motorDirection = notification.substring(index + 1);
    if (isMotorInBoard(motorNumber)) {
       motorNumber = motorNumber - ((boardOrder -1 ) * motorsPerBoard);  
       if (motorDirection == "b") {
          motor[motorNumber].run (BACKWARD | RELEASE);
        } else if (motorDirection == "f") {  
          motor[motorNumber].run (FORWARD | RELEASE);
        } else if (motorDirection == "s") {  
          motor[motorNumber].run (BRAKE);
        } 
        if (debug) {
          String motorReport = "Motor ";
          motorReport += motorNumber;
          motorReport += " ";
          motorReport += motorDirection;
          motorReport += " from board ";
          motorReport += boardOrder;
          Serial.println(motorReport);
        } 
    }
  } else if (notification != "lockOn" && lastCommand == "lockOn") {
    if (inProgress == false) {
      int charCount = 0;
      int firstChar = 0;
      int lastChar = 0;
      for (int i=0; i <= notification.length(); i++) {
        if (notification[i] == '-') { 
          charCount++; 
          if (charCount % 3 == 0) {
            lastChar = i;
            setMotors(notification.substring(firstChar, lastChar));
            firstChar = lastChar + 1;
          }
        }
      }  
      inProgress = true;
    } else {
      sendNotification("lockOn");
      if (debug) {
        Serial.println("Lock is on!");
      } 
    }
  } else if (notification == "stopMotors") {
    stopMotors();
  } else if (notification == "firmwareVersion") {
    sendNotification(firmwareVersion);
  } else if (notification == "settings") {
    returnSettings(); 
  } else if (notification == "bluetoothID") {
    sendDeviceAddress();     
  } else if (notification == "lockOn"){
    inProgress = true;
  } else if (notification == "lockOff"){
    inProgress = false;    
  } else if (notification == "backwardsOn"){
    backwardsOn = true;
  } else if (notification == "enableDebug"){
    debug = true;
    Serial.println("MyBar v" + firmwareVersion + " - Debug Enabled");
  } else if (notification == "disableDebug"){
    debug = false;
  } else if (notification == "enableDebugLoop"){
    debugLoop = true;
  } else if (notification == "disableDebugLoop"){
    debugLoop = false;        
  } else if (notification == "backwardsOn"){
    backwardsOn = true;
  } else if (notification == "distanceSensorOn"){
    distanceSensor = true;
    storeInEEPROM(0, 1);
  } else if (notification == "distanceSensorOff"){
    distanceSensor = false;
    storeInEEPROM(0, 0);    
  } else if (notification == "reverseMotorsOn"){
    reverseMotors = true;
    storeInEEPROM(1, 1);
    initializeMotors();
  } else if (notification == "reverseMotorsOff"){
    reverseMotors = false;
    storeInEEPROM(1, 0);
    initializeMotors();
 } else if (notification == "bluetoothOn"){
    bluetooth = true;
    storeInEEPROM(2, 1);
  } else if (notification == "bluetoothOff"){
    bluetooth = false;    
    storeInEEPROM(2, 0);
  } else if (notification == "serialOn"){
    serialConnectionEnable = true;
    storeInEEPROM(3, 1);
  } else if (notification == "serialOff"){
    serialConnectionEnable = false;
    storeInEEPROM(3, 0);
  } else if (notification == "qrOn"){
    qrCodeReader = true;
    storeInEEPROM(4, 1);
  } else if (notification == "qrOff"){
    qrCodeReader = false;
    storeInEEPROM(4, 0);    
  } else {
    if (inProgress == false) {
      setMotors(notification);
    } else {
      sendNotification("lockOn");
      if (debug) {
        Serial.println("Lock is on!");
      } 
    }
  }  
}

void storeInEEPROM(int address, int value) {
   EEPROM.write(address, value);
   EEPROM.commit();  
}

// Return settings (Number of motors - Firmware Version - Distance Sensor - Reverse Motors - Bluetooth - Serial Connection)
// 1 - Enable || 0 - Disable
void returnSettings() {
    String settings = ":";
    settings += connectedMotors;
    settings += "-";
    settings += boardVersion;
    settings += "-";
    settings += firmwareVersion;
    settings += "-";
    settings += distanceSensor ? "1" : "0";
    settings += "-";
    settings += reverseMotors ? "1" : "0";
    settings += "-";
    settings += bluetooth ? "1" : "0";
    settings += "-";
    settings += serialConnectionEnable ? "1" : "0";    
    sendNotification(settings);
}

void sendNotification(String message) {
    if (deviceConnected && deviceNotifying) {
        sendBTNotification(message);
    } 
    if (serialConnectionEnable) {
        Serial.println(message);
    }
}

void setup() {
  if (isCommercial == true) {
    qrCodeReader = true; 
    maxMotorsRunning = motorsPerBoard;
  }
  // Distance Sensor PIN
  if (boardVersion < 1.92) {
    PWM_PIN = 22;
  } else if (qrCodeReader) {
    bool debug = false;
    bool debugLoop = false;
    PWM_PIN = 1;
  }
  
  // Ref: http://esp32.info/docs/esp_idf/html/db/da4/task_8h.html#a25b035ac6b7809ff16c828be270e1431
  xTaskCreatePinnedToCore(
     Task1,                  /* pvTaskCode */
     "Workload1",            /* pcName */
     1000,                   /* usStackDepth */
     NULL,                   /* pvParameters */
     1,                      /* uxPriority */
     &TaskA,                 /* pxCreatedTask */
     0);                     /* xCoreID */


  Serial.begin(115200);
  //Serial.begin(115200, SERIAL_8N1, Serial_Txd_pin, Serial_Rxd_pin);
  
  // wait until serial port opens for native USB devices
  while (! Serial) {
    delay(1);
  } 

  
  if (debug) {
    Serial.println("MyBar v" + firmwareVersion + " - Clockwise motors");
  }
  
  while (!EEPROM.begin(EEPROM_SIZE)) {
    delay(10);
  }

  // Restore options from EEPROM
  uint16_t distanceSensorEEPPROM = byte(EEPROM.read(0));
  uint16_t reverseMotorsEEPPROM = byte(EEPROM.read(1));
  uint16_t bluetoothEEPPROM = byte(EEPROM.read(2));
  uint16_t serialEEPPROM = byte(EEPROM.read(3));
  uint16_t qrEEPPROM = byte(EEPROM.read(4));
  uint16_t motorsEEPPROM = byte(EEPROM.read(5));
  
  if (debug) {
    Serial.println("EEPROM distanceSensor: " + String(distanceSensorEEPPROM));
    Serial.println("EEPROM reverseMotors: " + String(reverseMotorsEEPPROM));
    Serial.println("EEPROM bluetooth: " + String(bluetoothEEPPROM));
    Serial.println("EEPROM serial: " + String(serialEEPPROM));   
    Serial.println("EEPROM QR Code: " + String(qrEEPPROM)); 
    Serial.println("EEPROM Motors: " + String(motorsEEPPROM)); 
  }
  if (distanceSensorEEPPROM == 1) {
    distanceSensor = true;
  } else if (distanceSensorEEPPROM == 0) {
    distanceSensor = false;
  }
  if (reverseMotorsEEPPROM == 1) {
    reverseMotors = true;
  } else if (distanceSensorEEPPROM == 0) {
    reverseMotors = false;
  }
  if (bluetoothEEPPROM == 1) {
    bluetooth = true;
  } else if (bluetoothEEPPROM == 0) {
    bluetooth = false;
  }
  if (serialEEPPROM == 1) {
    serialConnectionEnable = true;
  } else if (serialEEPPROM == 0) {
    serialConnectionEnable = false;
  }
  if (qrEEPPROM == 1) {
    qrCodeReader = true;
  } else if (qrEEPPROM == 0) {
    qrCodeReader = false;
  }
  if (qrEEPPROM == 255) {
    storeInEEPROM(5, connectedMotors);
  } else {
    connectedMotors = motorsEEPPROM;
  }
  
  if (debug) {
    Serial.print("distanceSensor: ");
    Serial.println( distanceSensor ? "true" : "false");
    Serial.print("reverseMotors: ");
    Serial.println( reverseMotors ? "true" : "false");
    Serial.print("bluetooth: ");
    Serial.println( bluetooth ? "true" : "false");
    Serial.print("serialConnectionEnable: ");
    Serial.println( serialConnectionEnable ? "true" : "false");
    Serial.print("qrCodeReader: ");
    Serial.println( qrCodeReader ? "true" : "false");  
    Serial.print("Motors: ");
    Serial.println(motorsEEPPROM);    
  }

  initializeMotors();
  ledcAttach(LED_PIN, LEDC_BASE_FREQ, LEDC_TIMER_13_BIT);

  for (int bright = 255; bright >= 0; bright--) {
      ledcAnalogWrite(LED_PIN, bright);
      delay(10);
  }
  
  // engage the motor's brake
  for (int i = 0; i <= motorsPerBoard - 1; ++i) {
    if (!serialConnectionEnable || (serialConnectionEnable && i != 4)) {  
      motor[i].run (BRAKE);
    }
  }

  if (bluetooth) {
    // Create the BLE Device
    BLEDevice::init("MyBar");
    // Larger MTU lets the app push firmware in up to 514-byte chunks instead of 20
    BLEDevice::setMTU(OTA_BLE_MTU);

    // Create the BLE Server
    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
  
    // Create the BLE Service
    BLEService *pService = pServer->createService(SERVICE_UUID);
  
    // Create a BLE Characteristic
    pCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY                
                      );
  
    pDescriptor = new BLE2902();
    pCharacteristic->addDescriptor(pDescriptor);
  
    BLECharacteristic *pCharacteristic = pService->createCharacteristic(
                                           CHARACTERISTIC_UUID_RX,
                                           BLECharacteristic::PROPERTY_WRITE
                                         );
  
    pCharacteristic->setCallbacks(new MyCallbacks());
    pDescriptor->setCallbacks(new MyDisCallbacks());

    // Binary characteristic the app writes the firmware .bin chunks to
    otaSetupCharacteristic(pService);

    // Start the service
    pService->start();
  
    // Start advertising
    pServer->getAdvertising()->start();
    if (debug) {
      Serial.println("Waiting a client connection to notify...");
    }  
  }
  returnSettings();
}

// the loop function runs over and over again forever
void loop() {
  //glassDistance = pulseIn(PWM_PIN, HIGH);
  unsigned long currentMillis = millis();

  // Firmware update housekeeping: stalled-transfer timeout and reboot after a successful update
  otaLoop();

  if (debug && debugLoop) {
     Serial.printf("Motors running: %d\n", motorsRunning);
     if (distanceSensor) {
        Serial.printf("Glass Distance: %d\n", glassDistance);
     }
  }
  
  if (Serial.available() > 0) {
    readValue = Serial.readStringUntil('\n');
    processNotification(readValue);
  } 

  if (Serial.available() > 0) {
    readValue = Serial.readStringUntil('\n');
    processNotification(readValue);
  } 
  
  // disconnecting
  if (!deviceConnected && !bluetoothAdvertising && bluetooth) {
      delay(50); // give the bluetooth stack the chance to get things ready
      pServer->startAdvertising(); // restart advertising
      bluetoothAdvertising = true;
      if (debug) {
          Serial.println("Start advertising");
      }
  }

  // If motors are in queue, start one at a time
  if ((motorsRunning < maxMotorsRunning) && (!motorsQueue.isEmpty())) {
    setMotors(motorsQueue.pop());
  }
  
  if (turnLedOn) {
    ledcAnalogWrite(LED_PIN, 255);
    ledOn = true;
    turnLedOn = false;
  }
  
  if (turnLedOff) {
    ledcAnalogWrite(LED_PIN, 0);
    ledOn = false;
    turnLedOff = false;
    if (!backwardsOn) {
      stopMotors(true);
    }
  }    
  
  if (ledBlink) {
    // change the brightness for next time through the loop:
    brightness = brightness + fadeAmount;
    ledcAnalogWrite(LED_PIN, brightness);
    // reverse the direction of the fading at the ends of the fade:
    if (brightness <= 0 || brightness >= 255) {
      fadeAmount = -fadeAmount;
      blinkedTimes += 1;
    }
    if (blinkedTimes >= blinkTimes) {
      ledBlink = false;
      ledOn = false;
      ledcAnalogWrite(LED_PIN, 0);
      brightness = 0;
    }
    delay(15);
  } 

  if ((glassDistance < minGlassDistance && glassDistance > 0) || !distanceSensor) {
    if (!(ledOn)) turnLedOn = true;
  } else {
    if (ledOn) turnLedOff = true;    
  }

  if (motorsRunning <= 0 && motorsQueue.isEmpty()) {
    inProgress = false;
  } else { 
    // Stop motors as time is up
    for (int thisMotor = 0; thisMotor < connectedMotors; thisMotor++) {
      bool lastMotor = false;
      if (timeToCompletion[thisMotor] > 0 && timeToCompletion[thisMotor] <= currentMillis) {
        if (isMotorInBoard(thisMotor)) {   
          motor[thisMotor].run (BRAKE);
        } else {
          Serial.println("^" + String(thisMotor) + "-s");
        }
        if (motorsRunning > 0) {
          motorsRunning = motorsRunning - 1;
          if (motorsRunning <= 0 && motorsQueue.isEmpty()) {
            lastMotor = true; 
          }
        }  
        timeToCompletion[thisMotor] = 0; 
        if (debug) {
          sendNotification("stop:" + String(thisMotor));
          Serial.printf("Motor %d stopped\n", thisMotor);
        }
        if (lastMotor && inProgress) {
          inProgress = false;
          backwardsOn = false; 
          startLedBlink();
          if (debug) {
            Serial.println("All Motors stopped");
          }
          delay(50);
          sendNotification("finish");
        }      
      }
    }
  }
}
