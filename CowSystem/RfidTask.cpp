// RfidTask.cpp
// RFID reader wrapper.
// Uses UART1 + the Rfid134 library. The library calls a static callback when a tag is read,
// so this file keeps a pointer to the active RfidTask instance and forwards events into it.

#include "RfidTask.h"
#include <Rfid134.h>

// UART1 for RFID (change the UART index if you ever need a different one)
static HardwareSerial RFIDSerial(1);

// Used to forward the static library callback into the class instance
static RfidTask* g_rfidTask = nullptr;

class RfidNotification {
public:
  static void OnPacketRead(const Rfid134Reading& reading) {
    if (g_rfidTask) g_rfidTask->onTagRead(reading.id);
  }

  static void OnError(Rfid134_Error error) {
    Serial.print("RFID Error: ");
    Serial.println(error);
  }
};

static Rfid134<HardwareSerial, RfidNotification> rfid(RFIDSerial);

void RfidTask::begin(int rxPin, int txPin, uint32_t baud) {
  g_rfidTask = this;

  // RFID module uses 8N2 framing
  RFIDSerial.begin(baud, SERIAL_8N2, rxPin, txPin);
  rfid.begin();
}

void RfidTask::tick() {
  // Keep feeding bytes to the RFID parser
  rfid.loop();
}

void RfidTask::onTagRead(uint32_t id) {
  pendingTag = String(id);
  hasTag = true;

  Serial.print("RFID Tag ID: ");
  Serial.println(id, DEC);
}

bool RfidTask::consumeTag(String &tagOut) {
  if (!hasTag) return false;

  hasTag = false;
  tagOut = pendingTag;
  pendingTag = "";
  return true;
}