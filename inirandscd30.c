#include <Wire.h>
#include <HardwareSerial.h>
#include <SparkFun_SCD30_Arduino_Library.h>

HardwareSerial INIR(1);
SCD30 airSensor;

uint32_t latestMethane = 0;
float latestCO2 = 0.0;
float latestTemp = 0.0;

unsigned long lastPrintTime = 0;

void sendCmd(String cmd) {
  INIR.print(cmd);
  Serial.print("Sent to INIR: ");
  Serial.println(cmd);
  delay(250);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("--- System Initializing ---");

  Wire.begin();  
  if (!airSensor.begin(Wire)) {
    Serial.println("SCD30 not detected. Check wiring!");
    while (1) { delay(1000); }
  }
  Serial.println("SCD30 initialized successfully!");

  INIR.begin(38400, SERIAL_8N1, 17, 16); 
  Serial.println("INIR2-ME5% initialized!");

  sendCmd("[C]"); 
  delay(500);
  sendCmd("[A]"); 

  Serial.println("Initialized");

}

void loop() {
  if (INIR.available()) {
    String line = INIR.readStringUntil('\n');
    line.trim();

    if (line.equalsIgnoreCase("0000005b") || line.equalsIgnoreCase("5b")) {
      String gasStr = INIR.readStringUntil('\n'); gasStr.trim();
      String faultStr = INIR.readStringUntil('\n'); faultStr.trim();
      String tempStr = INIR.readStringUntil('\n'); tempStr.trim();
      String crcStr = INIR.readStringUntil('\n'); crcStr.trim();
      String invCrcStr = INIR.readStringUntil('\n'); invCrcStr.trim();
      String endStr = INIR.readStringUntil('\n'); endStr.trim();

      latestMethane = strtoul(gasStr.c_str(), NULL, 16);
    }
  }

  if (airSensor.dataAvailable()) {
    latestCO2 = airSensor.getCO2();
    latestTemp = airSensor.getTemperature(); 
  }

  if (millis() - lastPrintTime >= 5000) {
    lastPrintTime = millis(); // Reset the timer

    Serial.print("Methane - ");
    Serial.print(latestMethane);
    Serial.print(" ppm   CO2 - ");
    Serial.print(latestCO2);
    Serial.print(" ppm   Temperature - ");
    Serial.print(latestTemp);
    Serial.println(" °C");
  }
}
