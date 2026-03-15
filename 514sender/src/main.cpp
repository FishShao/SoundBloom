/*
 * ============================================
 *   BLE Sender — 3x FSR 压力控制器
 *   Board: XIAO ESP32-C3
 * ============================================
 *
 * FSR 接线 (每个 FSR + 10kΩ 下拉电阻):
 *   FSR1 → D0 (GPIO2)  — Guitar
 *   FSR2 → D1 (GPIO3)  — Piano
 *   FSR3 → D2 (GPIO4)  — Flute
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

#define FSR_GUITAR  D0
#define FSR_PIANO   D1
#define FSR_FLUTE   D2

#define SERVICE_UUID   "12345678-1234-1234-1234-123456789abc"
#define CHAR_FSR_UUID  "abcd1234-5678-9012-3456-789abcdef012"

#define SMOOTH_SAMPLES  8
#define SEND_INTERVAL   50

NimBLEServer         *pServer  = nullptr;
NimBLECharacteristic *pCharFSR = nullptr;
bool deviceConnected = false;

// ── BLE 连接/断开回调 (兼容 NimBLE 1.x 和 2.x) ──
class ServerCallbacks : public NimBLEServerCallbacks {
  // NimBLE 1.x 签名
  void onConnect(NimBLEServer* s) {
    deviceConnected = true;
    Serial.println(">> Client Connected!");
  }
  void onDisconnect(NimBLEServer* s) {
    deviceConnected = false;
    Serial.println(">> Disconnected, restarting adv...");
    NimBLEDevice::startAdvertising();
  }
};

uint16_t readFSR_smooth(int pin) {
  uint32_t sum = 0;
  for (int i = 0; i < SMOOTH_SAMPLES; i++) sum += analogRead(pin);
  return (uint16_t)(sum / SMOOTH_SAMPLES);
}

uint8_t mapPressure(uint16_t raw) {
  if (raw < 50) return 0;
  int val = map(raw, 50, 3500, 0, 100);
  return (uint8_t)constrain(val, 0, 100);
}

void setup() {
  delay(2000);
  Serial.begin(115200);
  Serial.println("\n=== FSR BLE Sender ===");

  analogReadResolution(12);
  pinMode(FSR_GUITAR, INPUT);
  pinMode(FSR_PIANO,  INPUT);
  pinMode(FSR_FLUTE,  INPUT);

  NimBLEDevice::init("FSR_MusicCtrl");

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);
  pCharFSR = pService->createCharacteristic(
    CHAR_FSR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  pService->start();

  NimBLEAdvertising *pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->start();

  Serial.println("Advertising... waiting for connection");
}

void loop() {
  static unsigned long lastSend = 0;
  if (millis() - lastSend < SEND_INTERVAL) return;
  lastSend = millis();

  uint8_t pctG = mapPressure(readFSR_smooth(FSR_GUITAR));
  uint8_t pctP = mapPressure(readFSR_smooth(FSR_PIANO));
  uint8_t pctF = mapPressure(readFSR_smooth(FSR_FLUTE));

  Serial.printf("G:%3d%% P:%3d%% F:%3d%%\n", pctG, pctP, pctF);

  if (deviceConnected) {
    uint8_t data[3] = { pctG, pctP, pctF };
    pCharFSR->setValue(data, 3);
    pCharFSR->notify();
  }
}