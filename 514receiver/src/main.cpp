/*
 * ======================================================
 *   BLE Receiver — 3-Track PCM Mixer + Motor + OLED
 *   Board: XIAO ESP32-C3
 * ======================================================
 *
 * 功能:
 *   1. BLE 接收 3 个 FSR 压力值 (0-100%)
 *   2. 同时播放 3 首 WAV，各自音量由 FSR 控制
 *   3. 步进电机根据总音量左右摆动
 *   4. OLED 显示乐器音量条和百分比
 *
 * WAV: 8000Hz, 8-bit unsigned, Mono
 *
 * 接线:
 *   I2S:     BCLK=D10(GPIO10), LRC=D9(GPIO9), DOUT=D8(GPIO8)
 *   OLED:    SDA=D4(GPIO6), SCL=D5(GPIO7)
 *   Stepper: IN1=D0(GPIO2), IN2=D1(GPIO3), IN3=D2(GPIO4), IN4=D3(GPIO5)
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPIFFS.h>
#include <driver/i2s.h>

// ── 引脚 ──
#define I2S_BCLK  10
#define I2S_LRC    9
#define I2S_DOUT   8

#define OLED_SDA    6
#define OLED_SCL    7
#define OLED_WIDTH  128
#define OLED_HEIGHT  64
#define OLED_ADDR   0x3C

#define MTR_IN1  2
#define MTR_IN2  3
#define MTR_IN3  4
#define MTR_IN4  5

// ── BLE UUIDs ──
#define SERVICE_UUID   "12345678-1234-1234-1234-123456789abc"
#define CHAR_FSR_UUID  "abcd1234-5678-9012-3456-789abcdef012"

// ── 音频 ──
#define SAMPLE_RATE   8000
#define MIX_BUF_SIZE  256

// ── 全局变量 ──
volatile uint8_t volGuitar = 0;
volatile uint8_t volPiano  = 0;
volatile uint8_t volFlute  = 0;
volatile bool    bleConnected = false;

// 平滑后的显示/控制用音量 (缓慢跟随 BLE 值)
float smoothG = 0, smoothP = 0, smoothF = 0;
#define SMOOTH_FACTOR 0.08f  // 越小越平滑 (0.01~0.3)

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

File wavGuitar, wavPiano, wavFlute;
uint32_t dataStartG = 44, dataStartP = 44, dataStartF = 44;

uint8_t bufG[MIX_BUF_SIZE];
uint8_t bufP[MIX_BUF_SIZE];
uint8_t bufF[MIX_BUF_SIZE];
int16_t mixBuf[MIX_BUF_SIZE * 2];

// ── Stepper ──
const int stepSeq[8][4] = {
  {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
  {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1}
};
int stepperPos = 0, stepperTarget = 0, stepPhase = 0;
unsigned long lastStepTime = 0;
#define STEP_DELAY_MS 3

// ═══════════════════════════════
//  BLE
// ═══════════════════════════════

static void notifyCallback(
  NimBLERemoteCharacteristic* pChar,
  uint8_t* pData, size_t length, bool isNotify)
{
  if (length >= 3) {
    volGuitar = pData[0];
    volPiano  = pData[1];
    volFlute  = pData[2];
  }
}

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) {
    bleConnected = true;
    Serial.println(">> Connected to Sender!");
  }
  void onDisconnect(NimBLEClient* c) {
    bleConnected = false;
    Serial.println(">> Disconnected from Sender");
  }
};

NimBLEClient *pClient = nullptr;

bool connectToSender() {
  Serial.println("Scanning for FSR_MusicCtrl...");

  NimBLEScan *pScan = NimBLEDevice::getScan();
  pScan->setActiveScan(true);
  pScan->start(5, false);  // 扫描 5 秒，非阻塞=false(阻塞等待)
  NimBLEScanResults results = pScan->getResults();

  NimBLEAdvertisedDevice *target = nullptr;
  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice dev = results.getDevice(i);
    if (dev.getName() == "FSR_MusicCtrl") {
      target = new NimBLEAdvertisedDevice(dev);
      Serial.println("Found FSR_MusicCtrl!");
      break;
    }
  }
  if (!target) {
    Serial.println("Sender not found.");
    return false;
  }

  pClient = NimBLEDevice::createClient();
  pClient->setClientCallbacks(new ClientCallbacks());

  if (!pClient->connect(target)) {
    Serial.println("Connection failed!");
    delete target;
    return false;
  }
  delete target;

  NimBLERemoteService *pSvc = pClient->getService(SERVICE_UUID);
  if (!pSvc) { Serial.println("Service not found!"); pClient->disconnect(); return false; }

  NimBLERemoteCharacteristic *pChr = pSvc->getCharacteristic(CHAR_FSR_UUID);
  if (!pChr) { Serial.println("Char not found!"); pClient->disconnect(); return false; }

  if (pChr->canNotify()) {
    pChr->subscribe(true, notifyCallback);
    Serial.println("Subscribed to FSR notifications!");
  }
  return true;
}

// ═══════════════════════════════
//  I2S
// ═══════════════════════════════

void initI2S() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;

  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);

  i2s_pin_config_t pins = {};
  pins.bck_io_num   = I2S_BCLK;
  pins.ws_io_num    = I2S_LRC;
  pins.data_out_num = I2S_DOUT;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

// ═══════════════════════════════
//  WAV
// ═══════════════════════════════

bool openWAV(File &f, const char* path) {
  f = SPIFFS.open(path, "r");
  if (!f) { Serial.printf("FAILED: %s\n", path); return false; }
  f.seek(44);
  Serial.printf("OK: %s (%d bytes)\n", path, f.size());
  return true;
}

void readWAV_loop(File &f, uint8_t *buf, size_t len) {
  size_t got = f.read(buf, len);
  if (got < len) {
    f.seek(44);  // 回到 PCM 数据起点循环播放
    f.read(buf + got, len - got);
  }
}

// ═══════════════════════════════
//  Stepper
// ═══════════════════════════════

void stepperWrite(int phase) {
  digitalWrite(MTR_IN1, stepSeq[phase][0]);
  digitalWrite(MTR_IN2, stepSeq[phase][1]);
  digitalWrite(MTR_IN3, stepSeq[phase][2]);
  digitalWrite(MTR_IN4, stepSeq[phase][3]);
}

void stepperOff() {
  digitalWrite(MTR_IN1, LOW);
  digitalWrite(MTR_IN2, LOW);
  digitalWrite(MTR_IN3, LOW);
  digitalWrite(MTR_IN4, LOW);
}

void stepperUpdate() {
  if (millis() - lastStepTime < STEP_DELAY_MS) return;
  if (stepperPos < stepperTarget) {
    stepPhase = (stepPhase + 1) & 7;
    stepperWrite(stepPhase);
    stepperPos++;
    lastStepTime = millis();
  } else if (stepperPos > stepperTarget) {
    stepPhase = (stepPhase - 1 + 8) & 7;
    stepperWrite(stepPhase);
    stepperPos--;
    lastStepTime = millis();
  } else {
    stepperOff();
  }
}

// ═══════════════════════════════
//  OLED
// ═══════════════════════════════

void drawBar(int y, uint8_t pct, const char* label) {
  oled.setTextSize(1);
  // 乐器名 (左边固定 6 字符宽 = 36px)
  oled.setCursor(0, y);
  oled.print(label);

  // 进度条 (x=38 开始, 宽 68, 高 8)
  int barX = 38;
  int barW = 68;
  int barH = 8;
  oled.drawRect(barX, y, barW, barH, SSD1306_WHITE);
  int fillW = (int)((barW - 2) * pct / 100.0f);
  if (fillW > 0) oled.fillRect(barX + 1, y + 1, fillW, barH - 2, SSD1306_WHITE);

  // 百分比数字 (右边)
  char buf[5];
  snprintf(buf, sizeof(buf), "%3d", pct);
  oled.setCursor(109, y);
  oled.print(buf);
}

void updateOLED() {
  uint8_t dG = (uint8_t)smoothG;
  uint8_t dP = (uint8_t)smoothP;
  uint8_t dF = (uint8_t)smoothF;

  oled.clearDisplay();
  oled.setTextSize(1);

  // 标题行 (y=0)
  oled.setCursor(28, 0);
  oled.print("SoundBloom");
  oled.setCursor(108, 0);
  oled.print(bleConnected ? "BT" : "--");

  // 三个音量条 (y=12, 26, 40)，间距 14px，不会重叠
  drawBar(12, dG, "Guitar");
  drawBar(26, dP, "Piano");
  drawBar(40, dF, "Pad");

  // 总音量 (y=54)
  int totalPct = ((int)dG + dP + dF) / 3;
  oled.setCursor(0, 54);
  oled.print("Total: ");
  oled.print(totalPct);
  oled.print("%");

  oled.display();
}

// ═══════════════════════════════
//  音符位图 (8x8)
// ═══════════════════════════════
// ♪ 音符图案
static const uint8_t noteIcon[] PROGMEM = {
  0b00000011,
  0b00000101,
  0b00001001,
  0b00001001,
  0b00001000,
  0b01111000,
  0b11111000,
  0b01110000
};

// ♫ 双音符图案
static const uint8_t noteIcon2[] PROGMEM = {
  0b01100110,
  0b01011010,
  0b01001010,
  0b01001010,
  0b01000010,
  0b11100111,
  0b11100111,
  0b01100110
};

void bootAnimation() {
  // 3 个音符从不同位置弹跳
  float noteX[3] = {20, 56, 92};   // 水平位置
  float noteY[3] = {10, 30, 20};   // 初始高度
  float noteV[3] = {0, 0, 0};      // 速度
  float gravity  = 0.6f;
  float bounce   = -0.7f;          // 弹力系数
  int   ground   = 48;             // 地面 y

  for (int frame = 0; frame < 60; frame++) {  // 约 2 秒动画
    oled.clearDisplay();

    // 标题
    oled.setTextSize(1);
    oled.setCursor(20, 0);
    oled.print("Music Mixer");

    // 更新 3 个音符的物理
    for (int n = 0; n < 3; n++) {
      noteV[n] += gravity;
      noteY[n] += noteV[n];

      // 碰到地面弹起
      if (noteY[n] >= ground) {
        noteY[n] = ground;
        noteV[n] *= bounce;
        if (noteV[n] > -1.0f) noteV[n] = -5.0f - n * 1.5f;  // 保持弹跳
      }

      // 交替画两种音符
      if (n == 1) {
        oled.drawBitmap((int)noteX[n], (int)noteY[n], noteIcon2, 8, 8, SSD1306_WHITE);
      } else {
        oled.drawBitmap((int)noteX[n], (int)noteY[n], noteIcon, 8, 8, SSD1306_WHITE);
      }
    }

    // 底部进度条
    int barW = map(frame, 0, 59, 0, 128);
    oled.fillRect(0, 60, barW, 3, SSD1306_WHITE);

    oled.display();
    delay(33);  // ~30fps
  }
}

// ═══════════════════════════════
//  Setup & Loop
// ═══════════════════════════════

void setup() {
  delay(2000);
  Serial.begin(115200);
  Serial.println("\n=== BLE Music Receiver ===");

  // Motor
  pinMode(MTR_IN1, OUTPUT); pinMode(MTR_IN2, OUTPUT);
  pinMode(MTR_IN3, OUTPUT); pinMode(MTR_IN4, OUTPUT);
  stepperOff();

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    Serial.println("OLED OK");

    // 开机动画
    bootAnimation();
  }

  // SPIFFS
  if (!SPIFFS.begin(true)) { Serial.println("SPIFFS FAILED!"); while(1) delay(1000); }

  // WAV
  openWAV(wavGuitar, "/guitar.wav");
  openWAV(wavPiano,  "/piano.wav");
  openWAV(wavFlute,  "/flute.wav");

  // I2S
  initI2S();
  Serial.println("I2S OK");

  // BLE
  NimBLEDevice::init("MusicReceiver");

  oled.clearDisplay();
  oled.setCursor(10, 20);
  oled.print("Scanning BLE...");
  oled.setCursor(10, 36);
  oled.print("Turn on Sender!");
  oled.display();

  while (!connectToSender()) {
    Serial.println("Retrying in 3s...");
    delay(3000);
  }

  Serial.println("=== All systems ready! ===");
}

unsigned long lastOLED = 0;
unsigned long lastReconnect = 0;

void loop() {
  // ── 1. 平滑 FSR 值 ──
  smoothG += (volGuitar - smoothG) * SMOOTH_FACTOR;
  smoothP += (volPiano  - smoothP) * SMOOTH_FACTOR;
  smoothF += (volFlute  - smoothF) * SMOOTH_FACTOR;

  uint8_t vG = (uint8_t)smoothG;
  uint8_t vP = (uint8_t)smoothP;
  uint8_t vF = (uint8_t)smoothF;

  // ── 2. 音频混音 ──
  readWAV_loop(wavGuitar, bufG, MIX_BUF_SIZE);
  readWAV_loop(wavPiano,  bufP, MIX_BUF_SIZE);
  readWAV_loop(wavFlute,  bufF, MIX_BUF_SIZE);

  for (int i = 0; i < MIX_BUF_SIZE; i++) {
    int32_t g = ((int32_t)bufG[i] - 128) * vG;
    int32_t p = ((int32_t)bufP[i] - 128) * vP;
    int32_t f = ((int32_t)bufF[i] - 128) * vF;
    int32_t mixed = (g + p + f) * 85 / 100;
    if (mixed > 32767)  mixed = 32767;
    if (mixed < -32768) mixed = -32768;
    int16_t sample = (int16_t)mixed;
    mixBuf[i * 2]     = sample;
    mixBuf[i * 2 + 1] = sample;
  }

  size_t bw = 0;
  i2s_write(I2S_NUM_0, mixBuf, MIX_BUF_SIZE * 4, &bw, portMAX_DELAY);

  // ── 3. 电机 ──
  int totalVol = (int)vG + vP + vF;
  stepperTarget = map(totalVol, 0, 300, -150, 150);
  stepperUpdate();

  // ── 4. OLED ──
  if (millis() - lastOLED >= 150) {
    lastOLED = millis();
    updateOLED();
  }

  // ── 5. BLE 重连 ──
  if (!bleConnected && millis() - lastReconnect > 5000) {
    lastReconnect = millis();
    connectToSender();
  }
}