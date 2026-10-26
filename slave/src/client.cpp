#include <Arduino.h>

#ifdef ARDUINO_ARCH_ESP8266

extern "C" {
#include <espnow.h>
#include <user_interface.h>
}

#include <Adafruit_NeoPixel.h>
#include <ESP8266WiFi.h>

#include "buzzer_config.h"
#include "buzzer_logging.h"
#include "buzzer_protocol.h"

namespace vibe_wuzz {

namespace {

constexpr uint32_t kHelloIntervalMs = 2000;
constexpr uint32_t kHeartbeatIntervalMs = 3000;
constexpr uint32_t kDebounceMs = 35;
constexpr uint32_t kConnectionAnimationStepMs = 70;
constexpr uint32_t kCorrectAnimationStepMs = 120;
constexpr uint32_t kSearchingAnimationStepMs = 100;
constexpr uint16_t kStartupBreatheDurationMs = 2000;
constexpr uint8_t kStartupBreathePeak = 96;
Adafruit_NeoPixel g_pixels(kLedCount, kLedPin, NEO_GRB + NEO_KHZ800);
GameState g_state = GameState::Boot;
uint8_t g_winnerId = kNoWinner;
uint32_t g_nextSequence = 1;
uint32_t g_lastAppliedStateSequence = 0;
uint32_t g_lastButtonChangeMs = 0;
uint32_t g_lastHelloMs = 0;
uint32_t g_lastHeartbeatMs = 0;
bool g_lastButtonLevel = HIGH;
bool g_buzzSentForCurrentRound = false;
bool g_espNowReady = false;
bool g_peerReady = false;
bool g_masterSeen = false;
uint32_t g_lastAnimationMs = 0;
uint16_t g_animationHead = 0;

uint32_t nextSequence() { return g_nextSequence++; }

void renderArmedIndicator() {
  const bool armed = (g_state == GameState::Armed);
  digitalWrite(kArmedLedPin, armed ? HIGH : LOW);
  digitalWrite(LED_BUILTIN, armed ? LOW : HIGH);
}

uint32_t stripColor(uint8_t red, uint8_t green, uint8_t blue) {
  return g_pixels.Color(red, green, blue);
}

bool isLocalWinner() { return g_winnerId == kClientId; }

void logConfiguredMac(const char *label, const uint8_t mac[6]) {
  LOG_INFOF("%s %02X:%02X:%02X:%02X:%02X:%02X\n",
            label,
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5]);
}

uint32_t colorForState() {
  switch (g_state) {
  case GameState::Boot:
    return stripColor(20, 20, 20);
  case GameState::Idle:
    return stripColor(0, 0, 0);
  case GameState::Armed:
    return stripColor(0, 0, 50);
  case GameState::Locked:
    return stripColor(96, 0, 96);
  case GameState::Correct:
    return isLocalWinner() ? stripColor(0, 120, 0) : stripColor(0, 0, 0);
  case GameState::Incorrect:
    return isLocalWinner() ? stripColor(120, 0, 0) : stripColor(0, 0, 0);
  }

  return stripColor(0, 0, 0);
}

void renderState() {
  const uint32_t color = colorForState();
  for (uint16_t index = 0; index < kLedCount; ++index) {
    g_pixels.setPixelColor(index, color);
  }
  g_pixels.show();
  renderArmedIndicator();
}

void renderCorrectAnimation() {
  for (uint16_t index = 0; index < kLedCount; ++index) {
    g_pixels.setPixelColor(index, stripColor(0, 8, 0));
  }

  g_pixels.setPixelColor(g_animationHead % kLedCount, stripColor(0, 120, 0));
  g_pixels.setPixelColor((g_animationHead + kLedCount - 1) % kLedCount, stripColor(0, 48, 0));
  g_pixels.show();
  renderArmedIndicator();
}

void renderIncorrectAnimation() {
  for (uint16_t index = 0; index < kLedCount; ++index) {
    g_pixels.setPixelColor(index, stripColor(12, 0, 0));
  }

  g_pixels.setPixelColor(g_animationHead % kLedCount, stripColor(140, 0, 0));
  g_pixels.setPixelColor((g_animationHead + 1) % kLedCount, stripColor(80, 0, 0));
  g_pixels.show();
  renderArmedIndicator();
}

void renderSearchingAnimation() {
  constexpr uint8_t kSearchPhaseSteps = kLedCount * 8;
  constexpr uint8_t kWaveHalfWidth = 10;
  const uint8_t cycle = g_animationHead % kSearchPhaseSteps;
  const uint8_t globalPulse =
      (cycle < (kSearchPhaseSteps / 2))
          ? 8 + static_cast<uint8_t>((cycle * 24) / ((kSearchPhaseSteps / 2) - 1))
          : 8 + static_cast<uint8_t>(((kSearchPhaseSteps - 1 - cycle) * 24) /
                                      ((kSearchPhaseSteps / 2) - 1));

  for (uint16_t index = 0; index < kLedCount; ++index) {
    const uint8_t ledPhase = index * 8;
    int16_t distance = static_cast<int16_t>(cycle) - static_cast<int16_t>(ledPhase);
    if (distance < 0) {
      distance = -distance;
    }
    distance = min<int16_t>(distance, kSearchPhaseSteps - distance);

    uint8_t localBoost = 0;
    if (distance < kWaveHalfWidth) {
      localBoost = static_cast<uint8_t>(((kWaveHalfWidth - distance) * 110) / kWaveHalfWidth);
    }

    g_pixels.setPixelColor(index, stripColor(0, localBoost / 4, globalPulse + localBoost));
  }

  g_pixels.show();
  renderArmedIndicator();
}

void updateLedAnimation() {
  if (!g_masterSeen) {
    const uint32_t now = millis();
    if (now - g_lastAnimationMs < kSearchingAnimationStepMs) {
      return;
    }

    g_lastAnimationMs = now;
    g_animationHead = (g_animationHead + 1) % (kLedCount * 8);
    renderSearchingAnimation();
    return;
  }

  if (!isLocalWinner()) {
    return;
  }

  if (g_state != GameState::Correct && g_state != GameState::Incorrect) {
    return;
  }

  const uint32_t now = millis();
  if (now - g_lastAnimationMs < kCorrectAnimationStepMs) {
    return;
  }

  g_lastAnimationMs = now;
  g_animationHead = (g_animationHead + 1) % kLedCount;
  if (g_state == GameState::Correct) {
    renderCorrectAnimation();
  } else {
    renderIncorrectAnimation();
  }
}

void playStartupBreathe() {
  const uint32_t startMs = millis();

  while (true) {
    const uint32_t elapsed = millis() - startMs;
    if (elapsed >= kStartupBreatheDurationMs) {
      break;
    }

    const uint32_t cycleMs = elapsed % 1000;
    uint8_t brightness = 0;
    if (cycleMs < 500) {
      brightness = (cycleMs * kStartupBreathePeak) / 500;
    } else {
      brightness = ((1000 - cycleMs) * kStartupBreathePeak) / 500;
    }

    for (uint16_t index = 0; index < kLedCount; ++index) {
      g_pixels.setPixelColor(index, stripColor(0, 0, brightness));
    }
    g_pixels.show();
    delay(15);
  }

  g_pixels.clear();
  g_pixels.show();
}

void playConnectionAnimation() {
  for (uint16_t head = 0; head < kLedCount; ++head) {
    g_pixels.clear();
    g_pixels.setPixelColor(head, stripColor(0, 120, 0));
    if (head > 0) {
      g_pixels.setPixelColor(head - 1, stripColor(0, 40, 0));
    }
    g_pixels.show();
    delay(kConnectionAnimationStepMs);
  }

  g_pixels.clear();
  g_pixels.show();
}

void sendPacket(BuzzerPacket &packet) {
  if (!g_espNowReady || !g_peerReady) {
    LOG_DEBUGF("Skip send packet=%u espNowReady=%u peerReady=%u\n",
               static_cast<unsigned>(packet.type),
               g_espNowReady,
               g_peerReady);
    return;
  }

  packet.uptimeMs = millis();
  const int result = esp_now_send(const_cast<uint8_t *>(kMasterMac),
                                  reinterpret_cast<uint8_t *>(&packet),
                                  sizeof(packet));
  if (result != 0) {
    LOG_ERRORF("esp_now_send failed packet=%u err=%d\n",
               static_cast<unsigned>(packet.type),
               result);
  }
}

void sendAck(uint32_t ackSequence, AckReason reason) {
  BuzzerPacket packet = makePacket(PacketType::Ack, DeviceRole::Client,
                                   kClientId, 0, nextSequence());
  packet.ackSequence = ackSequence;
  packet.ackReason = reason;
  packet.state = g_state;
  packet.winnerId = g_winnerId;
  sendPacket(packet);
}

void sendPresence(PacketType type) {
  BuzzerPacket packet =
      makePacket(type, DeviceRole::Client, kClientId, 0, nextSequence());
  packet.state = g_state;
  packet.winnerId = g_winnerId;
  sendPacket(packet);
}

void sendBuzz() {
  BuzzerPacket packet = makePacket(PacketType::Buzz, DeviceRole::Client,
                                   kClientId, 0, nextSequence());
  packet.state = g_state;
  packet.winnerId = g_winnerId;
  sendPacket(packet);
}

void applyState(const BuzzerPacket &packet) {
  if (packet.sequence < g_lastAppliedStateSequence) {
    return;
  }

  g_lastAppliedStateSequence = packet.sequence;
  g_state = packet.state;
  g_winnerId = packet.winnerId;
  g_buzzSentForCurrentRound = (g_state != GameState::Armed);
  g_lastAnimationMs = 0;
  g_animationHead = 0;
  renderState();
}

void onDataSent(uint8_t *, uint8_t status) {
  if (status != 0) {
    LOG_DEBUGF("ESP-NOW async send status: fail code=%u\n", status);
  }
}

void onDataReceived(uint8_t *, uint8_t *incomingData, uint8_t length) {
  if (length != sizeof(BuzzerPacket)) {
    return;
  }

  BuzzerPacket packet{};
  memcpy(&packet, incomingData, sizeof(packet));

  if (packet.version != kProtocolVersion || packet.role != DeviceRole::Master) {
    return;
  }

  if (!g_masterSeen) {
    g_masterSeen = true;
    LOG_INFOLN("Master connection detected");
    playConnectionAnimation();
    renderState();
  }

  switch (packet.type) {
  case PacketType::StateUpdate:
    applyState(packet);
    sendAck(packet.sequence, AckReason::StateReceived);
    break;
  case PacketType::Hello:
  case PacketType::Heartbeat:
    sendPresence(packet.type == PacketType::Hello ? PacketType::Hello
                                                  : PacketType::Heartbeat);
    break;
  case PacketType::Ack:
  case PacketType::Buzz:
    break;
  }
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_set_channel(kEspNowChannel);
  LOG_INFOLN("=== Vibe Wuzz Client Boot ===");
  LOG_INFOF("Client STA MAC: %s\n", WiFi.macAddress().c_str());
  logConfiguredMac("Configured master MAC:", kMasterMac);
  LOG_INFOF("ESP-NOW channel: %u\n", kEspNowChannel);

  const int initResult = esp_now_init();
  if (initResult != 0) {
    LOG_ERRORF("ESP-NOW init failed err=%d\n", initResult);
    return;
  }
  g_espNowReady = true;
  LOG_INFOLN("ESP-NOW init ok");

  const int roleResult = esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  LOG_INFOF("ESP-NOW role set result=%d\n", roleResult);
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataReceived);
  const int peerResult =
      esp_now_add_peer(const_cast<uint8_t *>(kMasterMac), ESP_NOW_ROLE_COMBO, kEspNowChannel, nullptr, 0);
  g_peerReady = (peerResult == 0);
  LOG_INFOF("ESP-NOW add peer result=%d\n", peerResult);
  if (!g_peerReady) {
    LOG_ERRORLN("Client cannot send until configured master MAC matches the ESP32 STA MAC");
  }
}

void setupHardware() {
  pinMode(kButtonPin, INPUT_PULLUP);
  pinMode(kArmedLedPin, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(kArmedLedPin, LOW);
  digitalWrite(LED_BUILTIN, HIGH);
  g_pixels.begin();
  g_pixels.clear();
  g_pixels.show();
}

void pollButton() {
  const bool level = digitalRead(kButtonPin);
  const uint32_t now = millis();

  if (level != g_lastButtonLevel) {
    g_lastButtonChangeMs = now;
    g_lastButtonLevel = level;
    return;
  }

  if ((now - g_lastButtonChangeMs) < kDebounceMs) {
    return;
  }

  if (level == LOW && g_state == GameState::Armed &&
      !g_buzzSentForCurrentRound) {
    g_buzzSentForCurrentRound = true;
    sendBuzz();
  }
}

void maintainPresence() {
  const uint32_t now = millis();
  if (now - g_lastHelloMs >= kHelloIntervalMs) {
    g_lastHelloMs = now;
    sendPresence(PacketType::Hello);
  }

  if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
    g_lastHeartbeatMs = now;
    sendPresence(PacketType::Heartbeat);
  }
}

void appSetup() {
  Serial.begin(115200);
  delay(250);
  setupHardware();
  playStartupBreathe();
  renderState();
  setupEspNow();
}

void appLoop() {
  pollButton();
  maintainPresence();
  updateLedAnimation();
}

} // namespace

} // namespace vibe_wuzz

void setup() { vibe_wuzz::appSetup(); }

void loop() { vibe_wuzz::appLoop(); }

#endif
