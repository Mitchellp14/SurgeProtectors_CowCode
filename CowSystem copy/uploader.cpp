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
#include "time.h"

// --- WiFi & Firebase credentials ---
#define WIFI_SSID "CapstoneWifi2" //CHANGE IF NEEDED!!!
#define WIFI_PASSWORD "RuleNumber9"

#define Web_API_KEY "AIzaSyCgKeJBId5Ni2kR6hqma8Di08GPwoKtTBk"
#define DATABASE_URL "https://project-cow-database-default-rtdb.firebaseio.com"
#define USER_EMAIL "thgh9905@colorado.edu"
#define USER_PASS "1%AW&H9Wr!g!gvQ0m8gX"

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

void Uploader::initWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(500);
  }
  Serial.println("Connected!");
}

void Uploader::begin() {
  initWiFi();
  configTime(0, 0, ntpServer);

  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);

  initializeApp(aClient, app, getAuth(user_auth), nullptr, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  Serial.println("Uploader initialized (WiFi + NTP + Firebase).");
}

void Uploader::tick() {
  // Keep Firebase background state machine moving
  app.loop();
}

bool Uploader::ready() const {
  return app.ready();
}

uint32_t Uploader::epochNow() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return 0;
  time(&now);
  return (uint32_t)now;
}

bool Uploader::uploadGasSnapshot(const String &rfidTag, const GasReading &gas, uint32_t epoch) { //NOTE: If there is a runtime error, get rid of String RFIDtag datatype
  if (!app.ready()) return false;
  if (epoch == 0) return false;

  //String uid = app.getUid().c_str();
  String databasePath = "/UsersData/IntegrationTest2"; //Using in Integration test 2, so we're bypassing the uid bit.
  String parentPath = databasePath + "/" + String(epoch);

  Serial.print("Uploading to ");
  Serial.println(parentPath);

  // Build JSON payload
  writer.create(obj1, "/temperature", gas.tempC);
  writer.create(obj2, "/humidity", gas.humidity);
  writer.create(obj3, "/co2", gas.co2ppm);

  writer.create(obj4, "/methane_ppm", gas.methane_ppm);
  writer.create(obj5, "/inir_faults_hex", String(gas.inir_faults, HEX));
  writer.create(obj6, "/inir_temp_c", gas.inir_temp_c);
  writer.create(obj7, "/inir_valid", gas.ch4_valid);

  writer.create(obj8, "/timestamp", (int)epoch);
  writer.join(jsonData, 8, obj1, obj2, obj3, obj4, obj5, obj6, obj7, obj8);

  Database.set<object_t>(aClient, parentPath, jsonData, nullptr, "RTDB_Send_Data");
  return true;
}