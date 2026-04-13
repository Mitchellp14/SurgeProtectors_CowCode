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
// #define WIFI_SSID "CapstoneWifi2" //CHANGE IF NEEDED!!!
// #define WIFI_PASSWORD "RuleNumber9"

#define Web_API_KEY "AIzaSyCgKeJBId5Ni2kR6hqma8Di08GPwoKtTBk"
#define DATABASE_URL "https://project-cow-database-default-rtdb.firebaseio.com/"
#define USER_EMAIL "Avpe9860@colorado.edu"
#define USER_PASS "Little11Forest12!" // Please do not copy i am trusting you all so much :D

static const char *ntpServer = "pool.ntp.org";

String currentSSID = "";
String currentPassword = "";
String currentUserID = "";

#define SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID_RX "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID_TX "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Firebase objects kept local to this translation unit (so nothing else can touch them directly)
static UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);
static FirebaseApp app;
static WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
static AsyncClient aClient(ssl_client);
static RealtimeDatabase Database;

// #define USER "AcceptanceTest" //CHANGE THIS TO YOUR USERNAME OR WHATEVER YOU WANT!!!

// static object_t jsonData, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8;
// static JsonWriter writer;

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

class MyCallbacks : public BLECharacteristicCallbacks
{

  void onWrite(BLECharacteristic *pCharacteristic)
  {
    std::string rxValue = pCharacteristic->getValue();

    if (rxValue.length() > 0)
    {

      String payload = String(rxValue.c_str());

      payload.trim();

      int firstComma = payload.indexOf(',');
      int secondComma = payload.indexOf(',', firstComma + 1);

      if (firstComma > 0 && secondComma > firstComma)
      {

        currentSSID = payload.substring(0, firstComma);
        currentPassword = payload.substring(firstComma + 1, secondComma);
        currentUserID = payload.substring(secondComma + 1);

        Serial.println("Received SSID: " + currentSSID);
        Serial.println("Received Password: " + currentPassword);
        Serial.println("Received UserID: " + currentUserID);
      }
    }
  }
};

void setupBLE()
{
  BLEDevice::init("ProjectCow_ESP32");

  BLEServer *pServer = BLEDevice::createServer();

  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pTxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_TX,
      BLECharacteristic::PROPERTY_NOTIFY);

  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE);

  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("BLE Started. Waiting for Web Browser connection...");
}

// upload pending?
bool Uploader::isUploadPending() const
{
  return uploadPending;
}

// Added
void processData(AsyncResult &aResult)
{
  if (!aResult.isResult())
    return;

  if (aResult.isError())
  {
    Serial.printf("❌ Upload FAILED: %s (code: %d)\n",
                  aResult.error().message().c_str(),
                  aResult.error().code());
    uploadPending = false;
    lastUploadDone = millis();
  }
  else if (aResult.available())
  {
    if (uploadPending)
    {
      Serial.printf("✅ Upload SUCCESS!\n");
      // Don't release immediately — give async client time to fully close
      // the SSL operation before we allow another request
      lastUploadDone = millis();
      uploadPending = false;
    }
  }
}

// Added

void Uploader::initWiFi()
{
  Serial.println("---- WiFi Init ----");

  WiFi.mode(WIFI_STA);
  // WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.begin(currentSSID.c_str(), currentPassword.c_str());

  Serial.print("Connecting to WiFi");

  int retry = 0;

  while (WiFi.status() != WL_CONNECTED)
  {
    Serial.print(".");
    delay(500);
    retry++;

    if (retry > 40)
    { // ~20 seconds
      Serial.println("\nWiFi FAILED to connect!");
      return;
    }
  }

  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

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

// void Uploader::begin() {
//
//   Serial.println("---- Uploader Begin ----");
//
//   initWiFi();
//
//   Serial.println("Starting NTP sync...");
//   configTime(0, 0, ntpServer);
//
//   delay(2000);
//
//   time_t now = time(nullptr);
//   Serial.print("Epoch after NTP sync attempt: ");
//   Serial.println((uint32_t)now);
//
//   ssl_client.setInsecure();
//   ssl_client.setConnectionTimeout(1000);
//   ssl_client.setHandshakeTimeout(5);
//
//   Serial.println("Initializing Firebase...");
//
//   initializeApp(aClient, app, getAuth(user_auth), nullptr, "authTask");
//
//   app.getApp<RealtimeDatabase>(Database);
//   Database.url(DATABASE_URL);
//
//   Serial.println("Firebase initialized.");
// }

void Uploader::begin()
{
  Serial.println("---- Uploader Begin ----");
  setupBLE();

  Serial.println("Waiting for credentials from Web Bluetooth...");
  while (currentSSID == "" || currentPassword == "" || currentUserID == "")
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nCredentials received! Starting Wi-Fi...");
  initWiFi();

  Serial.println("Starting NTP sync...");
  configTime(0, 0, ntpServer);

  // Wait for valid NTP instead of fixed 2s delay
  time_t now = 0;
  uint32_t start = millis();
  while (now < 1000000000UL && millis() - start < 15000)
  {
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
  while (!app.ready())
  {
    app.loop();
    Serial.print(".");
    delay(100);
    if (millis() - authStart > 30000)
    {
      Serial.println("\nFirebase auth timeout!");
      return;
    }
  }
  Serial.println("\nFirebase ready!");
}

// void Uploader::tick() {
//   // Keep Firebase background state machine moving
//   app.loop();
// }

void Uploader::tick()
{

  app.loop();

  // static uint32_t lastPrint = 0;

  // if (millis() - lastPrint > 5000) { //Prints if firebase is ready every 5 seconds. Annoying!!!
  //   lastPrint = millis();

  //   Serial.print("Firebase ready: ");
  //   Serial.println(app.ready());
  // }
}

bool Uploader::ready() const
{
  return app.ready();
}

// uint32_t Uploader::epochNow() {
//   time_t now;
//   struct tm timeinfo;
//   if (!getLocalTime(&timeinfo)) return 0;
//   time(&now);
//   return (uint32_t)now;
// }

uint32_t Uploader::epochNow()
{

  time_t now;
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo))
  {
    Serial.println("NTP time not ready yet");
    return 0;
  }

  time(&now);

  // Serial.print("Epoch time: "); This just prints the epoch time once a second, very annoying
  // Serial.println((uint32_t)now);

  return (uint32_t)now;
}

bool Uploader::uploadGasSnapshot(const String &rfidTag, const GasReading &gas, uint32_t epoch)
{
  if (uploadPending)
  {
    Serial.println("Upload pending, skipping");
    return false;
  }
  uint32_t msSinceLast = millis() - lastUploadDone;
  if (msSinceLast < MIN_UPLOAD_GAP_MS)
  {
    Serial.printf("Too soon (%lums since last), skipping\n", msSinceLast);
    return false;
  }

  if (!app.ready())
  {
    Serial.println("Firebase NOT ready - skipping upload");
    return false;
  }
  if (epoch == 0)
  {
    Serial.println("Epoch invalid (0) - skipping upload");
    return false;
  }

  String parentPath = "/UsersData/" + currentUserID + "/" + rfidTag + "/" + String(epoch);

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
  if (isnan(gas.inir_temp_c))
  {
    writer.create(obj6, "inir_temp_c", 0); // or use null if library supports it
  }
  else
  {
    writer.create(obj6, "inir_temp_c", gas.inir_temp_c);
  }

  writer.create(obj7, "inir_valid", gas.ch4_valid);
  writer.create(obj8, "timestamp", (int)epoch);

  // Join all objects into one JSON structure
  writer.join(jsonData, 8, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8);

  // Debug: Print what we're sending
  Serial.print("JSON Payload: ");
  Serial.println(jsonData.c_str());

  // Serial.printf("Heap before upload: %lu, min ever: %lu\n",
  //             ESP.getFreeHeap(), ESP.getMinFreeHeap());
  uploadPending = true;
  // Use object_t type - this is the CORRECT way for FirebaseClient
  Database.set<object_t>(aClient, parentPath, jsonData, processData, "RTDB_Send_Data");
  delay(50);
  return true;
}

bool Uploader::uploadLoadCellSnapshot(const String &rfidTag, const LoadCellReading &load, uint32_t epoch)
{
  if (uploadPending)
  {
    Serial.println("Upload pending, skipping");
    return false;
  }
  uint32_t msSinceLast = millis() - lastUploadDone;
  if (msSinceLast < MIN_UPLOAD_GAP_MS)
  {
    Serial.printf("Too soon (%lums since last), skipping\n", msSinceLast);
    return false;
  }

  if (!app.ready())
  {
    Serial.println("Firebase NOT ready - skipping load cell upload");
    return false;
  }
  if (epoch == 0)
  {
    Serial.println("Epoch invalid (0) - skipping load cell upload");
    return false;
  }

  String parentPath = "/UsersData/" + String(USER) + "/" + rfidTag + "/" + String(epoch);

  Serial.println("---- Load Cell Upload Attempt ----");
  Serial.print("Path: ");
  Serial.println(parentPath);

  // Create JSON objects - need many for 8 load cells × (raw, voltage, valid) + timestamp
  object_t jsonData, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8, obj9, obj10,
      obj11, obj12, obj13, obj14, obj15, obj16, obj17, obj18, obj19, obj20,
      obj21, obj22, obj23, obj24, obj25;
  JsonWriter writer;

  // For each load cell
  writer.create(obj1, "lc0_valid", load.valid[0]);
  writer.create(obj2, "lc0_raw", (int)load.raw[0]);
  writer.create(obj3, "lc0_voltage_mv", load.voltage[0]);
  writer.create(obj4, "lc1_valid", load.valid[1]);
  writer.create(obj5, "lc1_raw", (int)load.raw[1]);
  writer.create(obj6, "lc1_voltage_mv", load.voltage[1]);
  writer.create(obj7, "lc2_valid", load.valid[2]);
  writer.create(obj8, "lc2_raw", (int)load.raw[2]);
  writer.create(obj9, "lc2_voltage_mv", load.voltage[2]);
  writer.create(obj10, "lc3_valid", load.valid[3]);
  writer.create(obj11, "lc3_raw", (int)load.raw[3]);
  writer.create(obj12, "lc3_voltage_mv", load.voltage[3]);
  writer.create(obj13, "lc4_valid", load.valid[4]);
  writer.create(obj14, "lc4_raw", (int)load.raw[4]);
  writer.create(obj15, "lc4_voltage_mv", load.voltage[4]);
  writer.create(obj16, "lc5_valid", load.valid[5]);
  writer.create(obj17, "lc5_raw", (int)load.raw[5]);
  writer.create(obj18, "lc5_voltage_mv", load.voltage[5]);
  writer.create(obj19, "lc6_valid", load.valid[6]);
  writer.create(obj20, "lc6_raw", (int)load.raw[6]);
  writer.create(obj21, "lc6_voltage_mv", load.voltage[6]);
  writer.create(obj22, "lc7_valid", load.valid[7]);
  writer.create(obj23, "lc7_raw", (int)load.raw[7]);
  writer.create(obj24, "lc7_voltage_mv", load.voltage[7]);
  writer.create(obj25, "timestamp", (int)epoch);

  // Join all objects
  writer.join(jsonData, 25, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8, obj9, obj10,
              obj11, obj12, obj13, obj14, obj15, obj16, obj17, obj18, obj19, obj20,
              obj21, obj22, obj23, obj24, obj25);

  // Debug: Print what we're sending
  Serial.print("Load Cell JSON Payload: ");
  Serial.println(jsonData.c_str());

  uploadPending = true;
  // Upload
  Database.set<object_t>(aClient, parentPath, jsonData, processData, "RTDB_Send_LoadCell");
  delay(50);
  return true;
}
