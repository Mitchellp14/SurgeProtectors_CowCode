#include <Arduino.h>
#include <Rfid134.h>

// --- RFID Pins ---
#define RFID_RX_PIN 14
#define RFID_TX_PIN -1   // leave TX unused if your reader is output‑only

// Use Serial0 instead of Serial1
HardwareSerial& RFIDSerial = Serial0;

// --- RFID Notification Handler ---
class RfidNotification {
public:
  static void OnPacketRead(const Rfid134Reading& reading) {
    Serial.print("RFID Tag ID: ");
    Serial.println(reading.id, DEC);
  }

  static void OnError(Rfid134_Error error) {
    Serial.print("RFID Error: ");
    Serial.println(error);
  }
};

// Create RFID instance
Rfid134<HardwareSerial, RfidNotification> rfid(RFIDSerial);

void setup() {
  Serial.begin(115200);
  delay(500);

  // Start RFID UART on Serial0
  RFIDSerial.begin(9600, SERIAL_8N2, RFID_RX_PIN, RFID_TX_PIN);

  rfid.begin();

  Serial.println("RFID reader ready on Serial0.");
}

void loop() {
  rfid.loop();
}
