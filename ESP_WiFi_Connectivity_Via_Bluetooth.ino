#include <BLEDevice.h>  // Imports the core Bluetooth Low Energy (BLE) device library.
#include <BLEServer.h>  // Imports the library needed to make the ESP32 act as a BLE Server.
#include <BLEUtils.h>   // Imports utility functions required for BLE operations.
#include <BLE2902.h>    // Imports the descriptor library required for sending data back to the phone.
#include <WiFi.h>       // Imports the standard Wi-Fi library to connect to your router.

BLEServer *pServer = NULL;             // Creates a global pointer for the BLE Server.
BLECharacteristic *pTxCharacteristic;  // Creates a global pointer for the Transmitting (TX) channel.
String networkData = "";               // Initializes an empty string to hold incoming Wi-Fi credentials.

// Standard UUIDs for the Nordic UART Service
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"            // Main UART service UUID.
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // Receive (RX) channel UUID.
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // Transmit (TX) channel UUID.

class MyServerCallbacks : public BLEServerCallbacks {           // Handles BLE connection events.
  void onConnect(BLEServer *pServer) {                          // Triggered when a phone connects.
    Serial.println("Device connected via BLE.");                // Logs connection to PC.
  };                                                            // Closes the onConnect function.
  void onDisconnect(BLEServer *pServer) {                       // Triggered when a phone disconnects.
    pServer->startAdvertising();                                // Restarts broadcasting so the phone can reconnect.
    Serial.println("Device disconnected. Advertising again.");  // Logs disconnection.
  }                                                             // Closes the onDisconnect function.
};                                                              // Closes the server callbacks class.

class MyCallbacks : public BLECharacteristicCallbacks {  // Handles incoming data from the phone.
  void onWrite(BLECharacteristic *pCharacteristic) {     // Triggered when the phone sends text.

    // *THIS IS THE FIX:* Grabs the data directly as an Arduino String instead of std::string.
    String rxValue = pCharacteristic->getValue();

    if (rxValue.length() > 0) {  // Ensures the message is not empty.
      networkData = rxValue;     // Simply assigns the received string to our global variable directly!

      int commaIndex = networkData.indexOf(',');                  // Finds the comma separating SSID and Password.
      if (commaIndex != -1) {                                     // If a comma is found...
        String ssid = networkData.substring(0, commaIndex);       // Extracts the Wi-Fi name.
        String password = networkData.substring(commaIndex + 1);  // Extracts the Wi-Fi password.
        ssid.trim();                                              // Cleans up accidental spaces.
        password.trim();                                          // Cleans up accidental spaces.

        pTxCharacteristic->setValue("Connecting to WiFi...\n");  // Prepares a status message.
        pTxCharacteristic->notify();                             // Pushes the message to the phone over BLE.

        WiFi.begin(ssid.c_str(), password.c_str());  // Starts the Wi-Fi connection attempt.

        int attempts = 0;                                         // Counter to prevent infinite loops.
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {  // Tries for 10 seconds.
          delay(500);                                             // Waits half a second.
          pTxCharacteristic->setValue(".");                       // Prepares a loading dot.
          pTxCharacteristic->notify();                            // Sends the dot to the phone.
          attempts++;                                             // Increases the counter.
        }                                                         // Closes the while loop.

        if (WiFi.status() == WL_CONNECTED) {                                         // If connection succeeds.
          String successMsg = "\nSuccess! IP: " + WiFi.localIP().toString() + "\n";  // Builds success string.
          pTxCharacteristic->setValue(successMsg.c_str());                           // Prepares the message.
          pTxCharacteristic->notify();                                               // Sends the IP address to the phone.
        } else {                                                                     // If connection fails.
          pTxCharacteristic->setValue("\nFailed to connect. Check credentials.\n");  // Prepares failure message.
          pTxCharacteristic->notify();                                               // Sends failure message to phone.
        }                                                                            // Closes the if/else block.
      }                                                                              // Closes the comma check block.
    }                                                                                // Closes the length check block.
  }                                                                                  // Closes the onWrite function.
};                                                                                   // Closes the characteristic callbacks class.

void setup() {           // Runs once on boot.
  Serial.begin(115200);  // Starts PC serial monitor.

  // Serial.println("Device MAC Address: " + WiFi.macAddress());
  BLEDevice::init("ESP32_C6_Setup");  // Turns on BLE and sets the broadcast name.

  String bleAddress = BLEDevice::getAddress().toString().c_str();
  Serial.println("BLE MAC Address:  " + bleAddress);

  pServer = BLEDevice::createServer();             // Creates the BLE Server.
  pServer->setCallbacks(new MyServerCallbacks());  // Attaches connection rules.

  BLEService *pService = pServer->createService(SERVICE_UUID);  // Creates the UART service.

  pTxCharacteristic = pService->createCharacteristic(  // Sets up the Transmit channel.
    CHARACTERISTIC_UUID_TX,                            // Assigns the TX UUID.
    BLECharacteristic::PROPERTY_NOTIFY                 // Allows pushing alerts to phone.
  );                                                   // Closes TX characteristic setup.
  pTxCharacteristic->addDescriptor(new BLE2902());     // Required descriptor for notifications.

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(  // Sets up the Receive channel.
    CHARACTERISTIC_UUID_RX,                                               // Assigns the RX UUID.
    BLECharacteristic::PROPERTY_WRITE                                     // Allows phone to send data.
  );                                                                      // Closes RX characteristic setup.
  pRxCharacteristic->setCallbacks(new MyCallbacks());                     // Attaches data-handling rules.

  pService->start();                              // Turns on the service.
  pServer->getAdvertising()->start();             // Starts broadcasting the BLE signal.
  Serial.println("BLE Started! Ready to pair.");  // Prints confirmation to PC.
}  // Closes the setup function.

void loop() {   // Runs continuously.
  delay(1000);  // Pauses for 1 second continuously to save power (BLE runs in the background).
}  // Closes the loop function.