// uploader.cpp
// Firebase + WiFi upload module.
// Handles connecting to WiFi, syncing time (NTP), initializing Firebase auth,
// and pushing one JSON snapshot into RTDB when asked.

#include "uploader.h"
#include "types.h"

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <ArduinoJson.h>
#include "time.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// --- WiFi & Firebase credentials ---
#define WIFI_SSID "CapstoneWifi2" //CHANGE IF NEEDED!!!
#define WIFI_PASSWORD "RuleNumber9"

#define Web_API_KEY "AIzaSyCgKeJBId5Ni2kR6hqma8Di08GPwoKtTBk"
#define DATABASE_URL "https://project-cow-database-default-rtdb.firebaseio.com/"
#define USER_EMAIL "Avpe9860@colorado.edu"
#define USER_PASS "Little11Forest12!" //Please do not copy i am trusting you all so much :D
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

static BLECharacteristic *pTxCharacteristic;
static String bleSSID = "";
static String blePassword = "";
static String bleUserId = "";
static bool credentialsReceived = false;

static const char* ntpServer = "pool.ntp.org";

// Firebase objects kept local to this translation unit (so nothing else can touch them directly)
static UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);
static FirebaseApp app;
static WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
static AsyncClient aClient(ssl_client);
static RealtimeDatabase Database;

//static object_t jsonData, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8;
//static JsonWriter writer;

static bool uploadPending = false;
static uint32_t lastUploadDone = 0;
static const uint32_t MIN_UPLOAD_GAP_MS = 200;
// void Uploader::initWiFi() {
//   WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
//   Serial.print("Connecting to WiFi ..");
//   while (WiFi.status() != WL_CONNECTED) {
//     Serial.print('.');
//     delay(500);
//   }
//   Serial.println("Connected!");
// }

// upload pending?
bool Uploader::isUploadPending() const {
  return uploadPending;
}

//Added
void processData(AsyncResult &aResult) {
  if (!aResult.isResult()) return;

  if (aResult.isError()) {
    Serial.printf("❌ Upload FAILED: %s (code: %d)\n",
                  aResult.error().message().c_str(),
                  aResult.error().code());
    uploadPending  = false;
    lastUploadDone = millis();

  } else if (aResult.available()) {
    if (uploadPending) {
      Serial.printf("✅ Upload SUCCESS!\n");
      // Don't release immediately — give async client time to fully close
     // the SSL operation before we allow another request
      lastUploadDone = millis();
      uploadPending  = false;
    }
  }
}

//Added

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer) {
        Serial.println("Phone connected to ESP32 via BLE.");
    }
    void onDisconnect(BLEServer *pServer) {
        pServer->startAdvertising();
        Serial.println("Phone disconnected. Advertising again.");
    }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        String rxValue = pCharacteristic->getValue();
        if (rxValue.length() > 0) {
            int firstComma = rxValue.indexOf(',');
            int secondComma = rxValue.indexOf(',', firstComma + 1);

            if (firstComma != -1 && secondComma != -1) {
                bleSSID     = rxValue.substring(0, firstComma);
                blePassword = rxValue.substring(firstComma + 1, secondComma);
                bleUserId   = rxValue.substring(secondComma + 1);
                bleSSID.trim();
                blePassword.trim();
                bleUserId.trim();

                credentialsReceived = true;

                pTxCharacteristic->setValue("Credentials received. Connecting...");
                pTxCharacteristic->notify();

                Serial.println("BLE credentials received.");
                Serial.println("SSID: " + bleSSID);
                Serial.println("UserID: " + bleUserId);
            } else {
                pTxCharacteristic->setValue("Bad format. Use: SSID,Password,UserID");
                pTxCharacteristic->notify();
            }
        }
    }
};

void Uploader::initBLE() {
    Serial.println("_____BLE Init_____");
    BLEDevice::init("ESP32_C6_Collar");

    BLEServer *pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);

    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE
    );
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    pService->start();
    pServer->getAdvertising()->start();
    Serial.println("BLE started. Waiting for credentials from phone...");
}

void Uploader::initWiFi() {
    Serial.println("______WiFi Init______");

    // Wait for credentials to arrive over BLE
    while (!credentialsReceived) {
        delay(100);
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(bleSSID.c_str(), blePassword.c_str());

    Serial.print("Connecting to WiFi");
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(500);
        retry++;
        if (retry > 40) {
            Serial.println("\nWiFi FAILED to connect!");
            pTxCharacteristic->setValue("Failed to connect. Check credentials.");
            pTxCharacteristic->notify();
            return;
        }
    }

    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    String successMsg = "Success! IP: " + WiFi.localIP().toString();
    pTxCharacteristic->setValue(successMsg.c_str());
    pTxCharacteristic->notify();
}


// void Uploader::initWiFi() {
//   Serial.println("---- WiFi Init ----");

//   WiFi.mode(WIFI_STA);
//   WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

//   Serial.print("Connecting to WiFi");

//   int retry = 0;

//   while (WiFi.status() != WL_CONNECTED) {
//     Serial.print(".");
//     delay(500);
//     retry++;

//     if (retry > 40) {   // ~20 seconds
//       Serial.println("\nWiFi FAILED to connect!");
//       return;
//     }
//   }

//   Serial.println("\nWiFi connected!");
//   Serial.print("IP address: ");
//   Serial.println(WiFi.localIP());
// }

// void Uploader::begin() {
//   initWiFi();
//   configTime(0, 0, ntpServer);

//   ssl_client.setInsecure();
//   ssl_client.setConnectionTimeout(1000);
//   ssl_client.setHandshakeTimeout(5);

//   initializeApp(aClient, app, getAuth(user_auth), nullptr, "authTask");
//   app.getApp<RealtimeDatabase>(Database);
//   Database.url(DATABASE_URL);

//   Serial.println("Uploader initialized (WiFi + NTP + Firebase).");
// }

//void Uploader::begin() {
//
//  Serial.println("---- Uploader Begin ----");
//
//  initWiFi();
//
//  Serial.println("Starting NTP sync...");
//  configTime(0, 0, ntpServer);
//
//  delay(2000);
//
//  time_t now = time(nullptr);
//  Serial.print("Epoch after NTP sync attempt: ");
//  Serial.println((uint32_t)now);
//
//  ssl_client.setInsecure();
//  ssl_client.setConnectionTimeout(1000);
//  ssl_client.setHandshakeTimeout(5);
//
//  Serial.println("Initializing Firebase...");
//
//  initializeApp(aClient, app, getAuth(user_auth), nullptr, "authTask");
//
//  app.getApp<RealtimeDatabase>(Database);
//  Database.url(DATABASE_URL);
//
//  Serial.println("Firebase initialized.");
//}

void Uploader::begin() {
  Serial.println("---- Uploader Begin ----");
  // initWiFi();

  initBLE();   
  initWiFi();

  Serial.println("Starting NTP sync...");
  configTime(0, 0, ntpServer);
  
  // Wait for valid NTP instead of fixed 2s delay
  time_t now = 0;
  uint32_t start = millis();
  while (now < 1000000000UL && millis() - start < 15000) {
    time(&now);
    Serial.print(".");
    delay(500);
  }
  Serial.print("\nEpoch: ");
  Serial.println((uint32_t)now);

  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);

  Serial.println("Initializing Firebase...");
  initializeApp(aClient, app, getAuth(user_auth), nullptr, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  // Wait for auth to complete before returning
  Serial.print("Waiting for Firebase auth");
  uint32_t authStart = millis();
  while (!app.ready()) {
    app.loop();
    Serial.print(".");
    delay(100);
    if (millis() - authStart > 30000) {
      Serial.println("\nFirebase auth timeout!");
      return;
    }
  }
  Serial.println("\nFirebase ready!");
}

 //void Uploader::tick() {
 //  // Keep Firebase background state machine moving
 //  app.loop();
 //}

void Uploader::tick() {

  app.loop();

  //static uint32_t lastPrint = 0;

  // if (millis() - lastPrint > 5000) { //Prints if firebase is ready every 5 seconds. Annoying!!!
  //   lastPrint = millis();

  //   Serial.print("Firebase ready: ");
  //   Serial.println(app.ready());
  // }
}

bool Uploader::ready() const {
  return app.ready();
}

// uint32_t Uploader::epochNow() {
//   time_t now;
//   struct tm timeinfo;
//   if (!getLocalTime(&timeinfo)) return 0;
//   time(&now);
//   return (uint32_t)now;
// }

uint32_t Uploader::epochNow() {

  time_t now;
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    Serial.println("NTP time not ready yet");
    return 0;
  }

  time(&now);

  //Serial.print("Epoch time: "); This just prints the epoch time once a second, very annoying
  //Serial.println((uint32_t)now);

  return (uint32_t)now;
}

bool Uploader::uploadGasSnapshot(const String &rfidTag, const GasReading &gas, uint32_t epoch) {
  if (uploadPending) {                              
    Serial.println("Upload pending, skipping");     
    return false;                                   
  }
  uint32_t msSinceLast = millis() - lastUploadDone;
  if (msSinceLast < MIN_UPLOAD_GAP_MS) {
    Serial.printf("Too soon (%lums since last), skipping\n", msSinceLast);
    return false;
  }

  if (!app.ready()) {
    Serial.println("Firebase NOT ready - skipping upload");
    return false;
  }
  if (epoch == 0) {
    Serial.println("Epoch invalid (0) - skipping upload");
    return false;
  }

  String parentPath = "/UsersData/DepartmentExpo/" + rfidTag + "/" + String(epoch);

  Serial.println("---- Upload Attempt ----");
  Serial.print("Path: ");
  Serial.println(parentPath);

  // Create JSON objects for storing data - RECREATE them each time to avoid stale data!
  object_t jsonData, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8;
  JsonWriter writer;

  // Build JSON payload using FirebaseClient's native JsonWriter
  // Note: The path in create() is RELATIVE to where you join them
  writer.create(obj1, "temperature", gas.tempC);
  writer.create(obj2, "humidity", gas.humidity);
  writer.create(obj3, "co2", gas.co2ppm);
  writer.create(obj4, "methane_ppm", gas.methane_ppm);
  writer.create(obj5, "inir_faults_hex", String(gas.inir_faults, HEX));
  
  // Handle NaN properly - send 0 or null
  if (isnan(gas.inir_temp_c)) {
    writer.create(obj6, "inir_temp_c", 0);  // or use null if library supports it
  } else {
    writer.create(obj6, "inir_temp_c", gas.inir_temp_c);
  }
  
  writer.create(obj7, "inir_valid", gas.ch4_valid);
  writer.create(obj8, "timestamp", (int)epoch);

  // Join all objects into one JSON structure
  writer.join(jsonData, 8, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8);

  // Debug: Print what we're sending
  Serial.print("JSON Payload: ");
  Serial.println(jsonData.c_str());

  //Serial.printf("Heap before upload: %lu, min ever: %lu\n", 
  //            ESP.getFreeHeap(), ESP.getMinFreeHeap());
  uploadPending = true;
  // Use object_t type - this is the CORRECT way for FirebaseClient
  Database.set<object_t>(aClient, parentPath, jsonData, processData, "RTDB_Send_Data");
  delay(50);
  return true;
}

bool Uploader::uploadLoadCellSnapshot(const String &rfidTag, const LoadCellReading &load, uint32_t epoch) {
  if (uploadPending) {
    Serial.println("Upload pending, skipping");
    return false;
  }
  uint32_t msSinceLast = millis() - lastUploadDone;
  if (msSinceLast < MIN_UPLOAD_GAP_MS) {
    Serial.printf("Too soon (%lums since last), skipping\n", msSinceLast);
    return false;
  }
  if (!app.ready()) {
    Serial.println("Firebase NOT ready - skipping load cell upload");
    return false;
  }
  if (epoch == 0) {
    Serial.println("Epoch invalid (0) - skipping load cell upload");
    return false;
  }
 
  String parentPath = "/UsersData/DepartmentExpo/" + rfidTag + "/" + String(epoch);
 
  Serial.println("---- Load Cell Upload Attempt ----");
  Serial.print("Path: ");
  Serial.println(parentPath);
 
  // 8 channels × 4 fields (valid, raw, voltage_mv, kg) + timestamp = 33 objects
  object_t jsonData,
           obj1,  obj2,  obj3,  obj4,   // ch0: valid, raw, voltage, kg
           obj5,  obj6,  obj7,  obj8,   // ch1
           obj9,  obj10, obj11, obj12,  // ch2
           obj13, obj14, obj15, obj16,  // ch3
           obj17, obj18, obj19, obj20,  // ch4
           obj21, obj22, obj23, obj24,  // ch5
           obj25, obj26, obj27, obj28,  // ch6
           obj29, obj30, obj31, obj32,  // ch7
           obj33;                       // timestamp
  JsonWriter writer;
 
  writer.create(obj1,  "lc0_valid",      load.valid[0]);
  writer.create(obj2,  "lc0_raw",        (int)load.raw[0]);
  writer.create(obj3,  "lc0_voltage_mv", load.voltage[0]);
  writer.create(obj4,  "lc0_kg",         load.kg[0]);
 
  writer.create(obj5,  "lc1_valid",      load.valid[1]);
  writer.create(obj6,  "lc1_raw",        (int)load.raw[1]);
  writer.create(obj7,  "lc1_voltage_mv", load.voltage[1]);
  writer.create(obj8,  "lc1_kg",         load.kg[1]);
 
  writer.create(obj9,  "lc2_valid",      load.valid[2]);
  writer.create(obj10, "lc2_raw",        (int)load.raw[2]);
  writer.create(obj11, "lc2_voltage_mv", load.voltage[2]);
  writer.create(obj12, "lc2_kg",         load.kg[2]);
 
  writer.create(obj13, "lc3_valid",      load.valid[3]);
  writer.create(obj14, "lc3_raw",        (int)load.raw[3]);
  writer.create(obj15, "lc3_voltage_mv", load.voltage[3]);
  writer.create(obj16, "lc3_kg",         load.kg[3]);
 
  writer.create(obj17, "lc4_valid",      load.valid[4]);
  writer.create(obj18, "lc4_raw",        (int)load.raw[4]);
  writer.create(obj19, "lc4_voltage_mv", load.voltage[4]);
  writer.create(obj20, "lc4_kg",         load.kg[4]);
 
  writer.create(obj21, "lc5_valid",      load.valid[5]);
  writer.create(obj22, "lc5_raw",        (int)load.raw[5]);
  writer.create(obj23, "lc5_voltage_mv", load.voltage[5]);
  writer.create(obj24, "lc5_kg",         load.kg[5]);
 
  writer.create(obj25, "lc6_valid",      load.valid[6]);
  writer.create(obj26, "lc6_raw",        (int)load.raw[6]);
  writer.create(obj27, "lc6_voltage_mv", load.voltage[6]);
  writer.create(obj28, "lc6_kg",         load.kg[6]);
 
  writer.create(obj29, "lc7_valid",      load.valid[7]);
  writer.create(obj30, "lc7_raw",        (int)load.raw[7]);
  writer.create(obj31, "lc7_voltage_mv", load.voltage[7]);
  writer.create(obj32, "lc7_kg",         load.kg[7]);
 
  writer.create(obj33, "timestamp",      (int)epoch);
 
  writer.join(jsonData, 33,
              obj1,  obj2,  obj3,  obj4,
              obj5,  obj6,  obj7,  obj8,
              obj9,  obj10, obj11, obj12,
              obj13, obj14, obj15, obj16,
              obj17, obj18, obj19, obj20,
              obj21, obj22, obj23, obj24,
              obj25, obj26, obj27, obj28,
              obj29, obj30, obj31, obj32,
              obj33);
 
  Serial.print("Load Cell JSON Payload: ");
  Serial.println(jsonData.c_str());
 
  uploadPending = true;
  Database.set<object_t>(aClient, parentPath, jsonData, processData, "RTDB_Send_LoadCell");
  delay(50);
  return true;
}