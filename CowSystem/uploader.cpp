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

// --- WiFi & Firebase credentials ---
#define WIFI_SSID "CapstoneWifi2" //CHANGE IF NEEDED!!!
#define WIFI_PASSWORD "RuleNumber9"

#define Web_API_KEY "AIzaSyCgKeJBId5Ni2kR6hqma8Di08GPwoKtTBk"
#define DATABASE_URL "https://project-cow-database-default-rtdb.firebaseio.com/"
#define USER_EMAIL "Avpe9860@colorado.edu"
#define USER_PASS "Little11Forest12!" //Please do not copy i am trusting you all so much :D

static const char* ntpServer = "pool.ntp.org";

// Firebase objects kept local to this translation unit (so nothing else can touch them directly)
static UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);
static FirebaseApp app;
static WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
static AsyncClient aClient(ssl_client);
static RealtimeDatabase Database;

static object_t jsonData, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8;
static JsonWriter writer;

// void Uploader::initWiFi() {
//   WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
//   Serial.print("Connecting to WiFi ..");
//   while (WiFi.status() != WL_CONNECTED) {
//     Serial.print('.');
//     delay(500);
//   }
//   Serial.println("Connected!");
// }

//Added
void processData(AsyncResult &aResult) {
  if (!aResult.isResult()) return;
  
  if (aResult.isError()) {
    Serial.printf("❌ Upload FAILED: %s (code: %d)\n", 
                  aResult.error().message().c_str(), 
                  aResult.error().code());
  } else if (aResult.available()) {
    // Only print success when we have actual payload response, not just queue confirmation
    Serial.printf("✅ Upload SUCCESS! Task: %s, Response: %s\n", 
                  aResult.uid().c_str(), 
                  aResult.c_str());
  }
}
//Added

void Uploader::initWiFi() {
  Serial.println("---- WiFi Init ----");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");

  int retry = 0;

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
    retry++;

    if (retry > 40) {   // ~20 seconds
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

void Uploader::begin() {

  Serial.println("---- Uploader Begin ----");

  initWiFi();

  Serial.println("Starting NTP sync...");
  configTime(0, 0, ntpServer);

  delay(2000);

  time_t now = time(nullptr);
  Serial.print("Epoch after NTP sync attempt: ");
  Serial.println((uint32_t)now);

  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);

  Serial.println("Initializing Firebase...");

  initializeApp(aClient, app, getAuth(user_auth), nullptr, "authTask");

  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  Serial.println("Firebase initialized.");
}

// void Uploader::tick() {
//   // Keep Firebase background state machine moving
//   app.loop();
// }

void Uploader::tick() {

  app.loop();

  static uint32_t lastPrint = 0;

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
  if (!app.ready()) {
    Serial.println("Firebase NOT ready - skipping upload");
    return false;
  }
  if (epoch == 0) {
    Serial.println("Epoch invalid (0) - skipping upload");
    return false;
  }

  String parentPath = "/UsersData/IntegrationTest2/IntegrationTest2_Gas/" + String(epoch);

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

  // Use object_t type - this is the CORRECT way for FirebaseClient
  Database.set<object_t>(aClient, parentPath, jsonData, processData, "RTDB_Send_Data");
  
  return true;
}