#include "RfidTask.h"
#include <Rfid134.h>

static HardwareSerial& RFIDSerial = Serial1;
static RfidTask* g_rfidTask = nullptr;

class RfidNotification {
public:
  static void OnPacketRead(const Rfid134Reading& reading) {
    // Guard: don't call into the object until it's fully initialised
    if (g_rfidTask && g_rfidTask->initialized) {
      g_rfidTask->onTagRead(reading.id);
    }
  }

  static void OnError(Rfid134_Error error) {
    Serial.print("RFID Error: ");
    Serial.println(error);
  }
};

static Rfid134<HardwareSerial, RfidNotification> rfid(RFIDSerial);

void RfidTask::begin(int rxPin, int txPin, uint32_t baud) {
  g_rfidTask = this;
  hasTag      = false;
  pendingId   = 0;

  RFIDSerial.begin(baud, SERIAL_8N1, rxPin, txPin);
  rfid.begin();

  initialized = true;   // ← only set AFTER everything is ready
}

void RfidTask::tick() {
  rfid.loop();
}

void RfidTask::onTagRead(uint32_t id) {
  // No String / heap allocation here — just store the raw id
  pendingId = id;
  hasTag    = true;

  Serial.print("RFID Tag ID: ");
  Serial.println(id, DEC);
}

bool RfidTask::consumeTag(String& tagOut) {
  if (!hasTag) return false;

  // String construction happens here, in the main loop, never in the callback
  hasTag   = false;
  tagOut   = String(pendingId);   // safe — we're in loop(), not a callback
  pendingId = 0;

  Serial.print("[RFID] Tag consumed: ");
  Serial.println(tagOut);
  return true;
}
