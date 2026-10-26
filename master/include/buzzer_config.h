#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "buzzer_protocol.h"

namespace vibe_wuzz {

struct ClientDefinition {
  uint8_t id;
  const char *label;
  uint8_t mac[6];
};

// Replace these MAC addresses with the real ESP8266 station MACs for your
// clients.
constexpr ClientDefinition kClientDefinitions[kMaxClients] = {
    {1, "Player 1", {0xDC, 0x4F, 0x22, 0x57, 0xF1, 0xD5}},
    {2, "Player 2", {0xCC, 0x50, 0xE3, 0x08, 0x22, 0x3C}},
    {3, "Player 3", {0x24, 0x6F, 0x28, 0x00, 0x00, 0x03}},
    {4, "Player 4", {0x24, 0x6F, 0x28, 0x00, 0x00, 0x04}},
};

constexpr uint8_t kEspNowChannel = 1;

#ifdef ARDUINO_ARCH_ESP32
#ifdef LED_BUILTIN
constexpr uint8_t kMasterStatusLedPin = LED_BUILTIN;
#else
constexpr uint8_t kMasterStatusLedPin = 2;
#endif
constexpr uint8_t kMasterStripPin = 4;
#endif

#ifdef ARDUINO_ARCH_ESP8266
#ifdef LED_BUILTIN
constexpr uint8_t kMasterStatusLedPin = LED_BUILTIN;
#else
constexpr uint8_t kMasterStatusLedPin = 2;
#endif
constexpr uint8_t kMasterStripPin = D2;
#endif

// ESP32 master access point configuration for the control UI.
constexpr char kMasterApSsid[] = "VibeWuzzMaster";
constexpr char kMasterApPassword[] = "buzzmaster";
constexpr char kMasterHostname[] = "vibewuzz";
constexpr bool kMasterStatusLedActiveLow = true;
constexpr uint8_t kMasterReadyLedPin = kMasterStatusLedPin;
constexpr bool kMasterReadyLedActiveLow = kMasterStatusLedActiveLow;
constexpr uint16_t kMasterStripLedCount = 6;
constexpr uint8_t kMasterStripBrightness = 48;

// ESP8266 client hardware defaults.
#ifdef ARDUINO_ARCH_ESP8266
constexpr uint8_t kClientId = 1;
constexpr uint8_t kButtonPin = D3;
constexpr uint8_t kLedPin = D2;
constexpr uint16_t kLedCount = 6;
#endif

inline String macToString(const uint8_t mac[6]) {
  char buffer[18];
  snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buffer);
}

inline const ClientDefinition *findClientDefinitionById(uint8_t id) {
  for (const auto &client : kClientDefinitions) {
    if (client.id == id) {
      return &client;
    }
  }

  return nullptr;
}

inline const ClientDefinition *findClientDefinitionByMac(const uint8_t mac[6]) {
  for (const auto &client : kClientDefinitions) {
    if (memcmp(client.mac, mac, sizeof(client.mac)) == 0) {
      return &client;
    }
  }

  return nullptr;
}

} // namespace vibe_wuzz
