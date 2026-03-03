#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <Wire.h>
#include "SparkFun_SCD30_Arduino_Library.h"
#include <Rfid134.h>
#include "time.h"

// --- WiFi & Firebase credentials ---
#define WIFI_SSID "CapstoneWifi2"
#define WIFI_PASSWORD "RuleNumber9"

#define Web_API_KEY "AIzaSyCgKeJBId5Ni2kR6hqma8Di08GPwoKtTBk"
#define DATABASE_URL "https://project-cow-database-default-rtdb.firebaseio.com"
#define USER_EMAIL "thgh9905@colorado.edu"
#define USER_PASS "1%AW&H9Wr!g!gvQ0m8gX"

// --- MQ-4 Parameters ---
#define MQ4_PIN 1
#define RL_VALUE 10000.0
#define VCC 3.3
#define ADC_RES 4095.0
#define R0 9000.0

// --- RFID Pins ---
#define RFID_RX_PIN 2
#define RFID_TX_PIN -1

// --- Sensor Instances ---
SCD30 airSensor;
HardwareSerial RFIDSerial(1);

// --- Firebase setup ---
UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);
FirebaseApp app;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
RealtimeDatabase Database;
object_t jsonData, obj1, obj2, obj3, obj4, obj5;
JsonWriter writer;

String uid;
String databasePath;
String parentPath;
int timestamp;
const char* ntpServer = "pool.ntp.org";

// --- Upload control ---
bool uploading = false;
unsigned long uploadStart = 0;
const unsigned long uploadDuration = 20000; // 20 seconds
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 5000; // every 5 seconds

// --- Current RFID ID ---
String currentRFID = "";

// --- MQ-4 helper ---
float getMethanePPM(int rawADC, float tempC) {
  float vOut = (rawADC / ADC_RES) * VCC;
  float rs = RL_VALUE * (VCC - vOut) / vOut;
  float ratio = rs / R0;
  float m = -0.38;
  float b = 1.5;
  float logPPM = m * log10(ratio) + b;
  float ppm = pow(10, logPPM);
  float alpha = 0.01;
  ppm *= (1.0 + alpha * (tempC - 20.0));
  return ppm;
}

// --- RFID Notification Handler ---
class RfidNotification {
public:
  static void OnPacketRead(const Rfid134Reading& reading) {
    Serial.print("RFID Tag ID: ");
    Serial.println(reading.id, DEC);

    // Start new upload session
    currentRFID = String(reading.id);
    uploading = true;
    uploadStart = millis();
  }

  static void OnError(Rfid134_Error error) {
    Serial.print("RFID Error: ");
    Serial.println(error);
  }
};

Rfid134<HardwareSerial, RfidNotification> rfid(RFIDSerial);

// --- WiFi ---
void initWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(1000);
  }
  Serial.println("Connected!");
}

// --- Time ---
unsigned long getTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return 0;
  time(&now);
  return now;
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(22, 23); // SDA=22, SCL=23
  if (!airSensor.begin()) {
    Serial.println("SCD30 not detected. Check wiring.");
    while (1);
  }

  pinMode(MQ4_PIN, INPUT);

  RFIDSerial.begin(9600, SERIAL_8N2, RFID_RX_PIN, RFID_TX_PIN);
  rfid.begin();

  initWiFi();
  configTime(0, 0, ntpServer);

  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);

  initializeApp(aClient, app, getAuth(user_auth), nullptr, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  Serial.println("All sensors initialized.");
}

// --- Loop ---
void loop() {
  rfid.loop();
  app.loop();

  if (uploading && app.ready()) {
    unsigned long now = millis();

    // Stop after 20 seconds
    if (now - uploadStart > uploadDuration) {
      uploading = false;
      Serial.println("Upload session ended.");
      return;
    }

    // Upload every 5 seconds
    if (now - lastSendTime >= sendInterval) {
      lastSendTime = now;

      uid = app.getUid().c_str();
      databasePath = "/UsersData/" + uid + "/RFID_" + currentRFID;
      timestamp = getTime();
      parentPath = databasePath + "/" + String(timestamp);

      // Read sensors
      int mq4Value = analogRead(MQ4_PIN);
      float tempC = airSensor.getTemperature();
      float co2 = airSensor.getCO2();
      float rh = airSensor.getHumidity();
      float methanePPM = getMethanePPM(mq4Value, tempC);

      // Print locally
      Serial.print("Uploading to RFID ");
      Serial.print(currentRFID);
      Serial.print(" at ");
      Serial.println(parentPath);

      // Build JSON
      writer.create(obj1, "/temperature", tempC);
      writer.create(obj2, "/humidity", rh);
      writer.create(obj3, "/co2", co2);
      writer.create(obj4, "/methane", methanePPM);
      writer.create(obj5, "/timestamp", timestamp);
      writer.join(jsonData, 5, obj1, obj2, obj3, obj4, obj5);

      Database.set<object_t>(aClient, parentPath, jsonData, nullptr, "RTDB_Send_Data");
    }
  }
}
