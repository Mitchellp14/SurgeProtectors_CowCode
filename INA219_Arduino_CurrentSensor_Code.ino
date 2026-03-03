#include <WiFi.h>
#include <ThingSpeak.h>
#include <Wire.h>
#include <Adafruit_INA219.h>

///////////////////////CODE FOR SENSOR 1///////////////////

// --------- WiFi settings ----------
const char* WIFI_SSID     = "kaywalks";
const char* WIFI_PASS = "Kwalker9789";

// --------- ThingSpeak settings ----------
unsigned long CHANNEL_ID = 3269120;   // your channel number
const char*  WRITE_KEY = "U5K6GT5VBKIDUOV6";
//unsigned long CHANNEL_ID = 3272725;   // your channel number
//const char*  WRITE_KEY = "JFCN6YF43FUDK1AK";

WiFiClient client;
Adafruit_INA219 ina219;

void connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected!");
}
bool ina_ok = false;

void setup() {
  Serial.begin(115200);
  delay(2000);

  // On QT Py ESP32-S3, typical I2C pins are SDA=7, SCL=6
  Wire.begin(7, 6);

  ina_ok = ina219.begin();  // DON'T while(1) if it fails

  connectWiFi();
  ThingSpeak.begin(client);
}

void loop() {
  connectWiFi();

  // If WiFi isn’t connected, don’t block forever—just retry
  if (WiFi.status() != WL_CONNECTED) {
    delay(2000);
    return;
  }

  if (ina_ok) {
    float current_mA = ina219.getCurrent_mA();
    float voltage_V = ina219.getBusVoltage_V();
     ThingSpeak.setField(1, current_mA);
    ThingSpeak.setField(2, voltage_V);

    ThingSpeak.setStatus("OK");

    Serial.print("Current: ");
    Serial.print(current_mA);
    Serial.print(" mA   Voltage: ");
    Serial.print(voltage_V);
    Serial.println(" V");;
    } else {
    // Send a clear “sensor missing” signal but keep uploading
    ThingSpeak.setField(1, -9999);
    ThingSpeak.setStatus("INA219 not found");
  }

  ThingSpeak.writeFields(CHANNEL_ID, WRITE_KEY);
  delay(20000);
}
