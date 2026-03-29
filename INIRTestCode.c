#include <HardwareSerial.h>

HardwareSerial& INIR = Serial0; // Creates a reference to UART 0

void sendCmd(String cmd) {
  INIR.print(cmd);
  Serial.print("Sent: ");
  Serial.println(cmd);
  delay(250);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 38400 baud, 8 data bits, 1 stop bit to prevent framing errors
   // Use UART1 on pins RX=4, TX=2
  INIR.begin(38400, SERIAL_8N1, 17, 16);


  Serial.println("Initializing Sensor...");
  
  // Wake in Config mode, then switch to Normal mode
  sendCmd("[C]"); 
  delay(500);
  sendCmd("[A]"); 
  
  Serial.println("Waiting for 45-second Warm-Up...");
}

void loop() {
  if (INIR.available()) {
    // Read the incoming text until the Line Feed (0x0A / '\n')
    String line = INIR.readStringUntil('\n');
    line.trim(); // This strips off the lingering Carriage Return (0x0D) and any spaces

    // Check if the sensor acknowledged our command
    if (line.equalsIgnoreCase("5b414b5d")) {
      Serial.println("Sensor Acknowledged Command: [AK]");
    }
    // Check if we hit the Start Character "0000005b"
    else if (line.equalsIgnoreCase("0000005b") || line.equalsIgnoreCase("5b")) {
      
      // We found the start of a frame! Now we just read the next 6 lines.
      String gasStr = INIR.readStringUntil('\n'); gasStr.trim();
      String faultStr = INIR.readStringUntil('\n'); faultStr.trim();
      String tempStr = INIR.readStringUntil('\n'); tempStr.trim();
      String crcStr = INIR.readStringUntil('\n'); crcStr.trim();
      String invCrcStr = INIR.readStringUntil('\n'); invCrcStr.trim();
      String endStr = INIR.readStringUntil('\n'); endStr.trim();

      // Convert the Hex Strings into actual integers
      uint32_t gasConc = strtoul(gasStr.c_str(), NULL, 16);
      uint32_t faults = strtoul(faultStr.c_str(), NULL, 16);
      uint32_t tempRaw = strtoul(tempStr.c_str(), NULL, 16);

      // Convert temperature to Celsius
      float tempC = (tempRaw / 10.0) - 273.15;

      // Print the final, clean data!
      Serial.println("\n--- INIR2-ME5% Reading ---");
      Serial.print("Methane (ppm) : "); Serial.println(gasConc);
      Serial.print("Temperature   : "); Serial.print(tempC); Serial.println(" °C");
      Serial.print("Fault Code    : 0x"); Serial.println(faults, HEX);
      Serial.println("--------------------------");
    }
  }
}
