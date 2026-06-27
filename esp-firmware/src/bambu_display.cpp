/*
  HUEMIXLINK V3 - BAMBU LAB PRINTER DISPLAY
  ESP32 + GC9A01 round LCD (240x240)
  Plugin device: polls printer status via ESP-NOW, button wake/sleep, OTA
*/

#include "HueMixLink.h"
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <Bounce2.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <sdkconfig.h>
#ifdef CONFIG_IDF_TARGET_ESP32
  #undef CONFIG_IDF_TARGET_ESP32
#endif
#include <mbedtls/sha256.h>
#include <fonts/Dosis_Medium12pt7b.h>
#include <fonts/Dosis_Regular5pt7b.h>
#include <fonts/Dosis_Regular9pt7b.h>
#include <icons/bambuicon.h>
#include <icons/nozzle.h>
#include <icons/heatbed.h>

// --- Plugin Constants ---
#define PKT_BAMBU_DISPLAY     0x52
#define BAMBU_DISPLAY_TYPE    1
#define BAMBU_PLUGIN_UUID_RAW {0x0a, 0x6f, 0xd1, 0x32, 0x10, 0x43, 0x47, 0x37, 0xa7, 0xee, 0x6d, 0xcf, 0x81, 0xc8, 0x19, 0x97}

#define DISPLAY_SUBTYPE_DATA    0x01
#define DISPLAY_SUBTYPE_REQUEST 0x02

// --- Timing ---
#define POLL_INTERVAL_MS      15000
#define HOLD_TO_SLEEP_MS     2000
#define HOLD_TO_RESET_MS     5000
#define DOUBLE_TAP_MS          500
#define ACK_TIMEOUT_MS          75
#define OTA_WAKE_TIMEOUT_MS  30000

// --- Pins ---
#ifndef PIN_BTN
#define PIN_BTN 6
#endif
#ifndef PIN_LED
#define PIN_LED 0
#endif
#define LED_ACTIVE_HIGH HIGH

#define NA_VALUE 0xFFFF
#define NA_PCT   0xFF

// --- Display data ---
struct DisplayData {
  uint16_t nozzle_temp;
  uint16_t bed_temp;
  uint16_t nozzle_target;
  uint16_t bed_target;
  uint8_t  progress_pct;
  uint16_t current_layer;
  uint16_t total_layer;
  uint16_t remaining_min;
  uint16_t mode_color;
  char     status_text[21];
  char     filename[41];
  char     tray_type[16];
  uint16_t tray_color;
  uint8_t  hour;
  uint8_t  minute;
  uint8_t  second;
  uint8_t  clock_mode;
  bool     valid;
  unsigned long last_update;
};

// --- OTA state machine ---
enum OtaState { OTA_IDLE, OTA_WAITING_NOTIFY, OTA_RECEIVING, OTA_VALIDATING, OTA_COMPLETE };
OtaState otaState = OTA_IDLE;
const esp_partition_t *update_partition = nullptr;
esp_ota_handle_t update_handle = 0;
mbedtls_sha256_context sha256_ctx;
uint32_t expected_firmware_size = 0;
uint32_t received_bytes = 0;
uint16_t expected_chunk_index = 0;
uint8_t expected_sha256[32];
unsigned long last_ota_activity = 0;
unsigned long ota_wake_time = 0;
bool ota_mode = false;
volatile bool ackReceived = false;

// --- Globals ---
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);
bool spr_ok = false;
Preferences prefs;
uint32_t HOME_ID = 0;
Payload_GatewayList gateways;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;

Bounce2::Button button = Bounce2::Button();
DisplayData displayData;

// --- Battery ---
#if defined(PIN_BATTERY) && PIN_BATTERY >= 0
uint16_t battery_mv = 0;
void getBatteryVoltage() {
  analogSetPinAttenuation(PIN_BATTERY, ADC_2_5db);
  uint32_t raw = 0;
  for (int i = 0; i < 10; i++) {
    raw += analogRead(PIN_BATTERY);
    delay(5);
  }
  raw /= 10;
  uint32_t adc_mv = raw * 1250U / 4095U;
  battery_mv = (adc_mv * BATTERY_DIVIDER_TOP + BATTERY_DIVIDER_BOTTOM / 2) / BATTERY_DIVIDER_BOTTOM;
}
#else
uint16_t battery_mv = 0;
inline void getBatteryVoltage() {}
#endif
unsigned long lastPollTime = 0;
unsigned long lastActivityTime = 0;
unsigned long lastDataActivity = 0;
unsigned long lastClockUpdate = 0;
unsigned long lastClockTick = 0;
bool noSignalRendered = false;

// LED breathing (for OTA)
#define LED_PWM_FREQ 5000
#define LED_PWM_RESOLUTION 8
unsigned long breathingTimer = 0;
int breathingDirection = 1;
int breathingBrightness = 0;
bool breathingActive = false;

// Button state
unsigned long btnHoldStart = 0;
unsigned long lastTapTime = 0;
int tapCount = 0;

unsigned long ledTimer = 0;
bool ledActive = false;
volatile bool needsRedraw = false;

// ── LED helpers ─────────────────────────────────────────────────

void triggerLed(int duration) {
  if (breathingActive) return;
  digitalWrite(PIN_LED, LED_ACTIVE_HIGH);
  ledActive = true;
  ledTimer = millis() + duration;
}

void ledBlink(int times, int delayMs) {
  if (breathingActive) return;
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_LED, LED_ACTIVE_HIGH); delay(delayMs);
    digitalWrite(PIN_LED, !LED_ACTIVE_HIGH); delay(delayMs);
  }
}

void startLedBreathing() {
  ledcAttach(PIN_LED, LED_PWM_FREQ, LED_PWM_RESOLUTION);
  breathingBrightness = 0;
  breathingDirection = 1;
  breathingActive = true;
  breathingTimer = millis();
}

void stopLedBreathing() {
  if (!breathingActive) return;
  ledcDetach(PIN_LED);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, !LED_ACTIVE_HIGH);
  breathingActive = false;
}

void updateBreathing() {
  if (!breathingActive) return;
  breathingBrightness += breathingDirection * 10;
  if (breathingBrightness >= 255) { breathingBrightness = 255; breathingDirection = -1; }
  else if (breathingBrightness <= 0) { breathingBrightness = 0; breathingDirection = 1; }
  if (LED_ACTIVE_HIGH) ledcWrite(PIN_LED, breathingBrightness);
  else ledcWrite(PIN_LED, 255 - breathingBrightness);
}

// ── Gateway persistence ──────────────────────────────────────────

void saveGateways() { prefs.putBytes("gw", &gateways, sizeof(gateways)); }

// ── Display Rendering ────────────────────────────────────────────

template<typename T>
void renderContent(T &dsp, int centerX, int centerY, int pct, uint16_t modeColor) {
  int outerRad = 115;
  int innerRad = 103;
  int startAngle = 180;
  int maxSweep = 360;
  int currentAngle = startAngle + (maxSweep * pct / 100);

  if (pct <= 100) {
    dsp.drawSmoothArc(centerX, centerY, outerRad, innerRad, startAngle, (startAngle + maxSweep) % 360, 0x39c7, TFT_BLACK, true);
    dsp.drawSmoothArc(centerX, centerY, outerRad, innerRad, startAngle, (currentAngle % 360), modeColor, modeColor, true);
  } else {
    dsp.drawSmoothArc(centerX, centerY, outerRad, innerRad, startAngle, (startAngle + maxSweep) % 360, modeColor, TFT_BLACK, true);
  }

  dsp.pushImage(99, 99, 43, 43, image_bambuicon_pixels);

  dsp.setTextSize(1);
  dsp.setFreeFont(&Dosis_Medium12pt7b);
  dsp.setTextDatum(MC_DATUM);
  dsp.setTextColor(modeColor);
  char percentString[8];
  if (pct == NA_PCT) {
    strcpy(percentString, "--%");
  } else {
    int displayValue = (pct > 100) ? 0 : pct;
    sprintf(percentString, "%d%%", displayValue);
  }
  dsp.drawString(percentString, 120, 35);

  dsp.setFreeFont(&Dosis_Regular9pt7b);
  dsp.setTextDatum(MC_DATUM);
  dsp.setTextColor(modeColor);
  dsp.drawString(displayData.status_text, 120, 80);

  dsp.setFreeFont(&Dosis_Regular9pt7b);
  dsp.setTextColor(TFT_WHITE);
  dsp.drawBitmap(20, centerY - 10, image_nozzle_pixels, 25, 25, TFT_WHITE);
  char nozzleStr[10];
  if (displayData.nozzle_temp == NA_VALUE) {
    strcpy(nozzleStr, "--~C");
  } else {
    sprintf(nozzleStr, "%d~C", displayData.nozzle_temp);
  }
  dsp.setTextDatum(ML_DATUM);
  dsp.drawString(nozzleStr, 46, centerY);

  char bedStr[10];
  if (displayData.bed_temp == NA_VALUE) {
    strcpy(bedStr, "--~C");
  } else {
    sprintf(bedStr, "%d~C", displayData.bed_temp);
  }
  dsp.setTextDatum(MR_DATUM);
  dsp.drawString(bedStr, 190, centerY);
  dsp.drawBitmap(195, centerY - 11, image_heatbed_pixels, 24, 24, TFT_WHITE);

  dsp.setFreeFont(&Dosis_Regular9pt7b);
  dsp.setTextColor(TFT_WHITE);
  dsp.setTextDatum(MC_DATUM);
  char layerStr[20];
  if (displayData.current_layer == NA_VALUE || displayData.total_layer == NA_VALUE) {
    strcpy(layerStr, "Layer: -- / --");
  } else {
    sprintf(layerStr, "Layer: %d / %d", displayData.current_layer, displayData.total_layer);
  }
  dsp.drawString(layerStr, 120, 160);

  dsp.setFreeFont(&Dosis_Regular5pt7b);
  dsp.setTextColor(0x7BEF);
  String displayFileName = String(displayData.filename);
  if (displayFileName.length() > 30) displayFileName = displayFileName.substring(0, 30) + "...";
  dsp.drawString(displayFileName, 120, 180);

  dsp.setFreeFont(&Dosis_Regular9pt7b);
  dsp.setTextColor(0x5DFF);
  char timeStr[16];
  if (displayData.remaining_min == NA_VALUE || displayData.remaining_min == 0) {
    strcpy(timeStr, "--m");
  } else {
    uint16_t h = displayData.remaining_min / 60;
    uint16_t m = displayData.remaining_min % 60;
    if (h > 0) sprintf(timeStr, "%dh%dm", h, m);
    else sprintf(timeStr, "%dm", m);
  }
  int clockRadius = 8, gap = 6;
  int textWidth = dsp.textWidth(timeStr);
  int totalWidth = (clockRadius * 2) + gap + textWidth;
  int startX = 120 - (totalWidth / 2);
  int clockX = startX + clockRadius;
  int clockY = 203, textX = clockX + clockRadius + gap;
  dsp.drawCircle(clockX, clockY, clockRadius, 0x5DFF);
  dsp.drawLine(clockX, clockY, clockX, clockY - 5, 0x5DFF);
  dsp.drawLine(clockX, clockY, clockX + 4, clockY + 3, 0x5DFF);
  dsp.setTextDatum(ML_DATUM);
  dsp.drawString(timeStr, textX, clockY);
}


void drawRoundedHand(auto& d, int cx, int cy, float ang, int len, int r, uint16_t col) {
  float c = cos(ang), s = sin(ang);
  for (int i = 0; i <= len; i += 2) {
    int px = cx + (int)(c * i + 0.5f), py = cy + (int)(s * i + 0.5f);
    d.fillCircle(px, py, r, col);
  }
}

void renderClock() {
  int hh = displayData.hour, mm = displayData.minute, ss = displayData.second;

  int cx = 120, cy = 120, r = 102;
  int h12 = hh % 12;
  float ha = (h12 * 30.0 + mm * 0.5 - 90.0) * 0.0174533;
  float ma = (mm * 6.0 - 90.0) * 0.0174533;
  float sa = (ss * 6.0 - 90.0) * 0.0174533;

  auto drawFace = [&](auto& d) {
    d.fillSprite(TFT_BLACK);
    // Outer ring
    d.drawCircle(cx, cy, r, 0x5AEB);
    d.drawCircle(cx, cy, r + 1, 0x39E7);
    // Tick marks
    for (int i = 0; i < 60; i++) {
      float a = (i * 6.0 - 90.0) * 0.0174533;
      int inner = (i % 5 == 0) ? r - 14 : r - 7;
      int x1 = cx + cos(a) * inner + 0.5, y1 = cy + sin(a) * inner + 0.5;
      int x2 = cx + cos(a) * (r - 2) + 0.5, y2 = cy + sin(a) * (r - 2) + 0.5;
      d.drawLine(x1, y1, x2, y2, i % 5 == 0 ? 0xFFFF : 0x6B4D);
    }
    // Numbers
    d.setTextDatum(MC_DATUM);
    d.setTextFont(1);
    d.setTextSize(2);
    d.setTextColor(0xFFFF);
    for (int n = 1; n <= 12; n++) {
      float a = (n * 30.0 - 90.0) * 0.0174533;
      int nx = cx + cos(a) * (r - 22) + 0.5, ny = cy + sin(a) * (r - 22) + 0.5;
      char buf[4]; snprintf(buf, sizeof(buf), "%d", n);
      d.drawString(buf, nx, ny);
    }
    // Hour hand (short, thick)
    drawRoundedHand(d, cx, cy, ha, r * 0.38, 2, 0xFFFF);
    drawRoundedHand(d, cx, cy, ma, r * 0.62, 1, 0xFFFF);
    drawRoundedHand(d, cx, cy, sa, r * 0.78, 0, 0xF800);

    // Center cap
    d.fillCircle(cx, cy, 3, 0xFFFF);
    d.fillCircle(cx, cy, 1, TFT_BLACK);
  };

  if (spr_ok) {
    drawFace(spr);
    spr.pushSprite(0, 0);
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.drawCircle(cx, cy, r, 0x5AEB);
    tft.drawCircle(cx, cy, r + 1, 0x39E7);
    for (int i = 0; i < 60; i++) {
      float a = (i * 6.0 - 90.0) * 0.0174533;
      int inner = (i % 5 == 0) ? r - 14 : r - 7;
      int x1 = cx + cos(a) * inner + 0.5, y1 = cy + sin(a) * inner + 0.5;
      int x2 = cx + cos(a) * (r - 2) + 0.5, y2 = cy + sin(a) * (r - 2) + 0.5;
      tft.drawLine(x1, y1, x2, y2, i % 5 == 0 ? 0xFFFF : 0x6B4D);
    }
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(1);
    tft.setTextSize(2);
    tft.setTextColor(0xFFFF);
    for (int n = 1; n <= 12; n++) {
      float a = (n * 30.0 - 90.0) * 0.0174533;
      int nx = cx + cos(a) * (r - 22) + 0.5, ny = cy + sin(a) * (r - 22) + 0.5;
      char buf[4]; snprintf(buf, sizeof(buf), "%d", n);
      tft.drawString(buf, nx, ny);
    }
    drawRoundedHand(tft, cx, cy, ha, r * 0.38, 2, 0xFFFF);
    drawRoundedHand(tft, cx, cy, ma, r * 0.62, 1, 0xFFFF);
    drawRoundedHand(tft, cx, cy, sa, r * 0.78, 0, 0xF800);
    tft.fillCircle(cx, cy, 3, 0xFFFF);
    tft.fillCircle(cx, cy, 1, TFT_BLACK);
  }
}

void renderDisplay() {
  int centerX = 120;
  int centerY = 120;

  if (displayData.clock_mode == 1) {
    renderClock();
    return;
  }

  if (!displayData.valid) {
    if (spr_ok) {
      spr.fillSprite(TFT_BLACK);
      spr.setTextDatum(MC_DATUM);
      spr.setTextColor(0x7BEF);
      spr.drawString("No Signal", centerX, centerY);
      spr.pushSprite(0, 0);
    } else {
      tft.fillScreen(TFT_BLACK);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(0x7BEF);
      tft.drawString("No Signal", centerX, centerY);
    }
    return;
  }

  int pct = displayData.progress_pct;
  uint16_t modeColor = displayData.mode_color;

  if (spr_ok) {
    spr.fillSprite(TFT_BLACK);
    renderContent(spr, centerX, centerY, pct, modeColor);
    spr.pushSprite(0, 0);
  } else {
    tft.fillScreen(TFT_BLACK);
    renderContent(tft, centerX, centerY, pct, modeColor);
  }
}

// ── Packet parsing ───────────────────────────────────────────────

void parseDisplayData(const uint8_t* payload) {
  if (payload[0] != DISPLAY_SUBTYPE_DATA) return;
  displayData.nozzle_temp    = payload[1] | (payload[2] << 8);
  displayData.bed_temp       = payload[3] | (payload[4] << 8);
  displayData.nozzle_target  = payload[5] | (payload[6] << 8);
  displayData.bed_target     = payload[7] | (payload[8] << 8);
  displayData.progress_pct   = payload[9];
  displayData.current_layer  = payload[10] | (payload[11] << 8);
  displayData.total_layer    = payload[12] | (payload[13] << 8);
  displayData.remaining_min  = payload[14] | (payload[15] << 8);
  displayData.mode_color     = payload[16] | (payload[17] << 8);
  uint8_t slen = payload[18];
  if (slen > 20) slen = 20;
  memcpy(displayData.status_text, &payload[19], slen);
  displayData.status_text[slen] = '\0';
  uint8_t flen = payload[39];
  if (flen > 40) flen = 40;
  memcpy(displayData.filename, &payload[40], flen);
  displayData.filename[flen] = '\0';
  uint8_t tlen = payload[80];
  if (tlen > 15) tlen = 15;
  memcpy(displayData.tray_type, &payload[81], tlen);
  displayData.tray_type[tlen] = '\0';
  displayData.tray_color = payload[96] | (payload[97] << 8);
  displayData.hour = payload[98];
  displayData.minute = payload[99];
  displayData.second = payload[100];
  displayData.clock_mode = payload[101];
  lastDataActivity = millis();
  displayData.last_update = millis();
  displayData.valid = true;
  noSignalRendered = false;
}

// ── Send functions ───────────────────────────────────────────────

void sendDisplayRequest() {
  if (HOME_ID == 0) return;

  getBatteryVoltage();

  HueMixLinkPacket pkt;
  memset(&pkt, 0, sizeof(HueMixLinkPacket));
  pkt.type = PKT_BAMBU_DISPLAY;
  WiFi.macAddress(pkt.sourceMAC);
  pkt.payload.raw[0] = DISPLAY_SUBTYPE_REQUEST;
  pkt.payload.raw[1] = battery_mv & 0xFF;
  pkt.payload.raw[2] = (battery_mv >> 8) & 0xFF;
  pkt.signature = calculateHash(pkt.payload.raw, 185, HOME_ID);

  ackReceived = false;
  bool sent = false;
  int successfulGatewayIndex = -1;

  for (int i = 0; i < gateways.count; i++) {
    if (!esp_now_is_peer_exist(gateways.macs[i])) {
      memcpy(peerInfo.peer_addr, gateways.macs[i], 6);
      peerInfo.channel = 0;
      peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
    }
    if (esp_now_send(gateways.macs[i], (uint8_t*)&pkt, sizeof(pkt)) == ESP_OK) {
      unsigned long w = millis();
      while (millis() - w < ACK_TIMEOUT_MS && !ackReceived) delay(1);
      if (ackReceived) {
        sent = true;
        successfulGatewayIndex = i;
        break;
      }
    }
  }

  if (successfulGatewayIndex > 0) {
    uint8_t tempMac[6];
    memcpy(tempMac, gateways.macs[successfulGatewayIndex], 6);
    for (int j = successfulGatewayIndex; j > 0; j--)
      memcpy(gateways.macs[j], gateways.macs[j - 1], 6);
    memcpy(gateways.macs[0], tempMac, 6);
    saveGateways();
  }
}

void sendHello() {
  HueMixLinkPacket pkt;
  memset(&pkt, 0, sizeof(HueMixLinkPacket));
  pkt.type = PKT_HELLO;
  WiFi.macAddress(pkt.sourceMAC);
  pkt.payload.raw[0] = 0xFE;
  pkt.payload.raw[1] = 0;
  const uint8_t plugin_uuid[16] = BAMBU_PLUGIN_UUID_RAW;
  memcpy(&pkt.payload.raw[2], plugin_uuid, 16);
  pkt.payload.raw[18] = BAMBU_DISPLAY_TYPE;
#ifdef FIRMWARE_VERSION
  uint8_t major = 0, minor = 0, patch = 0;
  sscanf(FIRMWARE_VERSION, "%hhu.%hhu.%hhu", &major, &minor, &patch);
  pkt.payload.raw[19] = major;
  pkt.payload.raw[20] = minor;
  pkt.payload.raw[21] = patch;
  pkt.payload.raw[22] = 0;
#else
  pkt.payload.raw[19] = 1; pkt.payload.raw[20] = 0;
  pkt.payload.raw[21] = 0; pkt.payload.raw[22] = 0;
#endif
  pkt.signature = calculateHash(pkt.payload.raw, 185, HOME_ID != 0 ? HOME_ID : 0);

  if (HOME_ID != 0 && gateways.count > 0) {
    for (int i = 0; i < gateways.count; i++) {
      if (!esp_now_is_peer_exist(gateways.macs[i])) {
        memcpy(peerInfo.peer_addr, gateways.macs[i], 6);
        peerInfo.channel = 0;
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);
      }
      esp_now_send(gateways.macs[i], (uint8_t*)&pkt, sizeof(pkt));
    }
  } else {
    if (!esp_now_is_peer_exist(broadcastAddress)) {
      memcpy(peerInfo.peer_addr, broadcastAddress, 6);
      peerInfo.channel = 0;
      peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
    }
    esp_now_send(broadcastAddress, (uint8_t*)&pkt, sizeof(pkt));
  }
}

void sendOtaReady() {
  HueMixLinkPacket ready;
  memset(&ready, 0, sizeof(HueMixLinkPacket));
  ready.type = PKT_OTA_READY;
  WiFi.macAddress(ready.sourceMAC);
  memset(ready.targetMAC, 0xFF, 6);
  ready.payload.otaReady.firmware_size = 0;
  ready.payload.otaReady.battery_mv = 0;
  ready.signature = calculateHash(ready.payload.raw, 185, HOME_ID);
  for (int i = 0; i < gateways.count; i++) {
    if (!esp_now_is_peer_exist(gateways.macs[i])) {
      memcpy(peerInfo.peer_addr, gateways.macs[i], 6);
      peerInfo.channel = 0;
      peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
    }
    if (esp_now_send(gateways.macs[i], (uint8_t*)&ready, sizeof(ready)) == ESP_OK) break;
  }
}

// ── Gateway management ───────────────────────────────────────────

void addGateway(const uint8_t *mac) {
  for (int i = 0; i < gateways.count; i++)
    if (memcmp(gateways.macs[i], mac, 6) == 0) return;
  if (gateways.count < MAX_GATEWAYS) {
    memcpy(gateways.macs[gateways.count], mac, 6);
    gateways.count++;
    saveGateways();
  }
}

// ── OTA handlers ─────────────────────────────────────────────────

void abortOta(const char* reason) {
  Serial.printf("[OTA] ABORT: %s\n", reason);
  stopLedBreathing();
  if (update_handle) { esp_ota_abort(update_handle); update_handle = 0; }
  otaState = OTA_IDLE;
  expected_chunk_index = 0;
  received_bytes = 0;
  ota_mode = false;
  ledBlink(3, 100);
}

void handleOtaNotify(HueMixLinkPacket* pkt) {
  if (otaState != OTA_WAITING_NOTIFY) return;
  expected_firmware_size = pkt->payload.otaNotify.firmware_size;
  memcpy(expected_sha256, pkt->payload.otaNotify.sha256_hash, 32);
  update_partition = esp_ota_get_next_update_partition(NULL);
  if (!update_partition) { abortOta("No partition"); return; }
  Serial.printf("[OTA] Starting update: %u bytes\n", expected_firmware_size);
  esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
  if (err != ESP_OK) { abortOta("Begin failed"); return; }
  mbedtls_sha256_init(&sha256_ctx);
  mbedtls_sha256_starts(&sha256_ctx, 0);
  otaState = OTA_RECEIVING;
  expected_chunk_index = 0;
  received_bytes = 0;
  last_ota_activity = millis();
  startLedBreathing();
  bool sent = false;
  int successfulGatewayIndex = -1;
  for (int i = 0; i < gateways.count; i++) {
    if (!esp_now_is_peer_exist(gateways.macs[i])) {
      memcpy(peerInfo.peer_addr, gateways.macs[i], 6);
      peerInfo.channel = 0;
      peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
    }
    HueMixLinkPacket ready;
    memset(&ready, 0, sizeof(HueMixLinkPacket));
    ready.type = PKT_OTA_READY;
    WiFi.macAddress(ready.sourceMAC);
    memset(ready.targetMAC, 0xFF, 6);
    ready.payload.otaReady.firmware_size = expected_firmware_size;
    ready.payload.otaReady.battery_mv = 0;
    ready.signature = calculateHash(ready.payload.raw, 185, HOME_ID);
    if (esp_now_send(gateways.macs[i], (uint8_t*)&ready, sizeof(ready)) == ESP_OK) {
      sent = true;
      successfulGatewayIndex = i;
      break;
    }
  }
  if (successfulGatewayIndex > 0) {
    uint8_t tempMac[6];
    memcpy(tempMac, gateways.macs[successfulGatewayIndex], 6);
    for (int j = successfulGatewayIndex; j > 0; j--) memcpy(gateways.macs[j], gateways.macs[j - 1], 6);
    memcpy(gateways.macs[0], tempMac, 6);
    saveGateways();
  }
  if (!sent) Serial.println("[OTA] Failed to send OTA_READY");
}

void handleOtaChunk(HueMixLinkPacket* pkt) {
  if (otaState != OTA_RECEIVING) return;
  last_ota_activity = millis();
  ota_wake_time = millis();
  uint16_t chunk_idx = pkt->payload.otaChunk.chunk_index;
  uint8_t data_len = pkt->payload.otaChunk.data_len;
  if (chunk_idx < expected_chunk_index) return;
  if (chunk_idx != expected_chunk_index) return;
  esp_err_t err = esp_ota_write(update_handle, pkt->payload.otaChunk.data, data_len);
  if (err != ESP_OK) { abortOta("Write failed"); return; }
  mbedtls_sha256_update(&sha256_ctx, pkt->payload.otaChunk.data, data_len);
  received_bytes += data_len;
  expected_chunk_index++;
  if (chunk_idx % 50 == 0) Serial.printf("[OTA] Progress: %u / %u bytes (%.1f%%)\n",
    received_bytes, expected_firmware_size, (received_bytes * 100.0) / expected_firmware_size);
}

void handleOtaCheckpointReq(HueMixLinkPacket* pkt) {
  if (otaState != OTA_RECEIVING) return;
  uint16_t last_chunk = (expected_chunk_index > 0) ? (expected_chunk_index - 1) : 0;
  HueMixLinkPacket ack;
  memset(&ack, 0, sizeof(HueMixLinkPacket));
  ack.type = PKT_OTA_CHUNK_ACK;
  WiFi.macAddress(ack.sourceMAC);
  memset(ack.targetMAC, 0, 6);
  ack.msgID = 0;
  ack.payload.otaChunkAck.last_chunk_index = last_chunk;
  ack.signature = calculateHash(ack.payload.raw, 185, HOME_ID);
  for (int i = 0; i < gateways.count; i++) {
    if (!esp_now_is_peer_exist(gateways.macs[i])) {
      memcpy(peerInfo.peer_addr, gateways.macs[i], 6);
      peerInfo.channel = 0;
      peerInfo.encrypt = false;
      esp_now_add_peer(&peerInfo);
    }
    if (esp_now_send(gateways.macs[i], (uint8_t*)&ack, sizeof(ack)) == ESP_OK) break;
  }
}

void handleOtaComplete(HueMixLinkPacket* pkt) {
  if (otaState != OTA_RECEIVING) return;
  otaState = OTA_VALIDATING;
  uint8_t calculated_sha256[32];
  mbedtls_sha256_finish(&sha256_ctx, calculated_sha256);
  mbedtls_sha256_free(&sha256_ctx);
  if (memcmp(calculated_sha256, expected_sha256, 32) != 0) { abortOta("SHA256 mismatch"); return; }
  Serial.println("[OTA] SHA256 verified!");
  esp_err_t err = esp_ota_end(update_handle);
  if (err != ESP_OK) { abortOta("End failed"); return; }
  update_handle = 0;
  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) { abortOta("Set boot failed"); return; }
  Serial.println("[OTA] UPDATE SUCCESSFUL! Rebooting...");
  otaState = OTA_COMPLETE;
  stopLedBreathing();
  ledBlink(10, 100);
  ESP.restart();
}

void handleOtaAbort(HueMixLinkPacket* pkt) {
  abortOta("Server abort");
}

// ── Packet processing ────────────────────────────────────────────
void processReceivedPacket(HueMixLinkPacket *rx, const uint8_t *mac) {
  if (rx->type == PKT_OTA_NOTIFY || rx->type == PKT_OTA_CHUNK ||
      rx->type == PKT_OTA_CHECKPOINT_REQ || rx->type == PKT_OTA_COMPLETE ||
      rx->type == PKT_OTA_ABORT) {
    if (HOME_ID != 0) {
      uint32_t expected_sig = calculateHash(rx->payload.raw, 185, HOME_ID);
      if (rx->signature != expected_sig) return;
    }
  }
  if (rx->type == PKT_OTA_NOTIFY) { handleOtaNotify(rx); lastActivityTime = millis(); ota_wake_time = millis(); return; }
  if (rx->type == PKT_OTA_CHUNK) { handleOtaChunk(rx); lastActivityTime = millis(); return; }
  if (rx->type == PKT_OTA_CHECKPOINT_REQ) { handleOtaCheckpointReq(rx); return; }
  if (rx->type == PKT_OTA_COMPLETE) { handleOtaComplete(rx); return; }
  if (rx->type == PKT_OTA_ABORT) { handleOtaAbort(rx); return; }
  if (rx->type == PKT_PAIR_CONFIRM) {
    if (HOME_ID == 0) {
      uint32_t sig = calculateHash(rx->payload.raw, 185, 0);
      if (rx->signature == sig) {
        HOME_ID = rx->payload.pair.newHomeID;
        addGateway(mac);
        Serial.printf("[DISP] PAIRED! ID: 0x%X\n", HOME_ID);
        prefs.putUInt("hid", HOME_ID);
        sendHello();
        triggerLed(150);
        lastPollTime = 0;
      }
    }
    return;
  }
  if (rx->type == PKT_ACK_TO_BTN) {
    ackReceived = true;
    if (rx->payload.gwList.count > 0) {
      if (HOME_ID != 0) {
        uint32_t expected_sig = calculateHash(rx->payload.raw, 185, HOME_ID);
        if (rx->signature != expected_sig) return;
      }
      Payload_GatewayList newList;
      newList.count = 0;
      for (int i = 0; i < gateways.count && newList.count < MAX_GATEWAYS; i++) {
        bool stillExists = false;
        for (int j = 0; j < rx->payload.gwList.count; j++)
          if (memcmp(gateways.macs[i], rx->payload.gwList.macs[j], 6) == 0) { stillExists = true; break; }
        if (stillExists) { memcpy(newList.macs[newList.count], gateways.macs[i], 6); newList.count++; }
      }
      for (int i = 0; i < rx->payload.gwList.count && newList.count < MAX_GATEWAYS; i++) {
        bool isNew = true;
        for (int j = 0; j < newList.count; j++)
          if (memcmp(rx->payload.gwList.macs[i], newList.macs[j], 6) == 0) { isNew = false; break; }
        if (isNew) { memcpy(newList.macs[newList.count], rx->payload.gwList.macs[i], 6); newList.count++; }
      }
      gateways = newList;
      saveGateways();
    }
    return;
  }
  if (rx->type == PKT_BAMBU_DISPLAY) {
    if (HOME_ID != 0) {
      uint32_t expected_sig = calculateHash(rx->payload.raw, 185, HOME_ID);
      if (rx->signature != expected_sig) return;
    }
    if (rx->payload.raw[0] == DISPLAY_SUBTYPE_DATA) {
      parseDisplayData(rx->payload.raw);
      needsRedraw = true;
      triggerLed(50);
    }
    return;
  }
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(HueMixLinkPacket)) return;
  processReceivedPacket((HueMixLinkPacket*)data, info->src_addr);
}

// ── Sleep ────────────────────────────────────────────────────────

void goToSleep() {
  Serial.println("[DISP] Going to sleep");
  tft.fillScreen(TFT_BLACK);
  tft.writecommand(0x28);
  delay(5);
  tft.writecommand(0x10);
  delay(10);
  digitalWrite(PIN_BACKLIGHT, LOW);
  gpio_hold_en(GPIO_NUM_1);
  digitalWrite(PIN_LED, !LED_ACTIVE_HIGH);
  delay(50);
  WiFi.mode(WIFI_OFF);
  btStop();
  Serial.flush();
  delay(10);
  esp_sleep_enable_ext1_wakeup((1ULL << PIN_BTN), ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

// ── Factory reset ────────────────────────────────────────────────

void factoryReset() {
  Serial.println("[DISP] Factory reset triggered!");
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Factory Reset", 120, 120);
  ledBlink(10, 50);
  delay(500);
  prefs.clear();
  HOME_ID = 0;
  gateways.count = 0;
  ESP.restart();
}

// ── Setup ────────────────────────────────────────────────────────

void setup() {
  gpio_hold_dis(GPIO_NUM_1);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_LED, !LED_ACTIVE_HIGH);
  pinMode(PIN_BTN, INPUT_PULLUP);

  getBatteryVoltage();

  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- BAMBU DISPLAY START ---");

  // Init display
  tft.init();
  tft.setRotation(0);
  spr_ok = spr.createSprite(240, 240);
  digitalWrite(PIN_BACKLIGHT, HIGH);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Bambu Display", 120, 110);
  tft.drawString("Starting...", 120, 130);

  // Load saved state
  prefs.begin("huemixlink", false);
  HOME_ID = prefs.getUInt("hid", 0);
  prefs.getBytes("gw", &gateways, sizeof(gateways));

  // Init ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  if (esp_now_init() != ESP_OK) ESP.restart();
  esp_now_register_recv_cb(OnDataRecv);

  // Init button
  button.attach(PIN_BTN, INPUT_PULLDOWN);
  button.interval(25);

  // Check wake source
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool wokeFromButton = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1);

  if (HOME_ID != 0) {
    sendHello();
    delay(100);
    sendDisplayRequest();
    lastPollTime = millis();
    lastDataActivity = millis();
  } else {
    sendHello();
  }

  lastActivityTime = millis();
  lastTapTime = 0;
  tapCount = 0;

  if (wokeFromButton) {
    Serial.println("[DISP] Woke from button press");
  } else {
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason == ESP_RST_SW) {
      Serial.println("[OTA] Detected software reset (post-OTA)");
      if (HOME_ID != 0) {
        delay(500);
        sendHello();
      }
    }
    Serial.println("[DISP] Cold boot");
  }
}

// ── Loop ─────────────────────────────────────────────────────────

void loop() {
  button.update();

  // LED breathing update
  if (breathingActive && millis() - breathingTimer >= 30) {
    breathingTimer = millis();
    updateBreathing();
  }

  // LED timer
  if (ledActive && millis() > ledTimer) {
    digitalWrite(PIN_LED, !LED_ACTIVE_HIGH);
    ledActive = false;
  }

  // ── Deferred rendering (from ESP-NOW callback) ────────────
  if (needsRedraw) {
    needsRedraw = false;
    renderDisplay();
    Serial.println("[DISP] Display updated");
  }

  // ── OTA handling ────────────────────────────────────────────
  if (otaState == OTA_RECEIVING && millis() - last_ota_activity > 30000) {
    Serial.println("[OTA] Timeout - no activity for 30s");
    abortOta("Timeout");
  }
  if (otaState == OTA_WAITING_NOTIFY && millis() - ota_wake_time > OTA_WAKE_TIMEOUT_MS) {
    Serial.println("[OTA] No NOTIFY received within 30s, returning to normal");
    otaState = OTA_IDLE;
    ota_mode = false;
    stopLedBreathing();
  }

  // ── Data keepalive / staleness ────────────────────────────────
  if (HOME_ID != 0) {
    unsigned long now = millis();
    unsigned long sinceActivity = now - lastDataActivity;

    if (displayData.valid) {
      // Have valid data — timeout after 10m inactivity, keepalive at 9m
      if (sinceActivity > 600000) {
        displayData.valid = false;
        renderDisplay();
      } else if (sinceActivity > 540000 && now - lastPollTime >= POLL_INTERVAL_MS) {
        lastPollTime = now;
        sendDisplayRequest();
      }
    } else {
      // No valid data — keep polling periodically
      if (now - lastPollTime >= POLL_INTERVAL_MS) {
        lastPollTime = now;
        sendDisplayRequest();
      }
      // After 10s boot with no data, transition "Starting..." → "No Signal"
      if (!noSignalRendered && sinceActivity >= 10000) {
        noSignalRendered = true;
        renderDisplay();
      }
    }

    // Real-time second hand tick
    if (displayData.clock_mode == 1 && now - lastClockTick >= 1000) {
      lastClockTick = now;
      displayData.second = (displayData.second + 1) % 60;
      if (displayData.second == 0) {
        displayData.minute = (displayData.minute + 1) % 60;
        if (displayData.minute == 0) {
          displayData.hour = (displayData.hour + 1) % 24;
        }
      }
      renderClock();
    }
  }

  // ── Button handling (only when awake, not in OTA mode) ──────
  if (!ota_mode) {
    if (button.pressed()) {
      unsigned long now = millis();
      if (now - lastTapTime < DOUBLE_TAP_MS) {
        tapCount++;
      } else {
        tapCount = 1;
      }
      lastTapTime = now;
      btnHoldStart = now;
      lastActivityTime = now;
    }

    // Double-tap → OTA mode
    if (button.released() && tapCount >= 2) {
      Serial.println("[OTA] Double-tap detected! Entering OTA mode...");
      otaState = OTA_WAITING_NOTIFY;
      ota_mode = true;
      ota_wake_time = millis();
      lastActivityTime = millis();
      tapCount = 0;
      ledBlink(3, 200);
      sendOtaReady();
    }

    // Single tap (quick release) → send hello to re-register
    if (button.released() && tapCount < 2 && millis() - btnHoldStart < HOLD_TO_SLEEP_MS) {
      sendHello();
      triggerLed(50);
      tapCount = 0;
    }

    // Hold 2s+ then release → deep sleep
    if (button.released() && tapCount < 2 && millis() - btnHoldStart >= HOLD_TO_SLEEP_MS) {
      goToSleep();
    }

    // Hold 5s+ (continuous, no release) → factory reset
    if (button.isPressed() && millis() - btnHoldStart >= HOLD_TO_RESET_MS) {
      factoryReset();
    }
  }

  delay(5);
}
