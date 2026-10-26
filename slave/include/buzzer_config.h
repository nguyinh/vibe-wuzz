#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "buzzer_protocol.h"

namespace vibe_wuzz {

constexpr uint8_t kEspNowChannel = 1;
constexpr uint8_t kClientId = 1;
constexpr uint8_t kButtonPin = D1;
constexpr uint8_t kArmedLedPin = D3;
constexpr uint8_t kLedPin = D2;
constexpr uint16_t kLedCount = 6;

// Replace this with the ESP32 master station MAC.
constexpr uint8_t kMasterMac[6] = {0xCC, 0x50, 0xE3, 0x08, 0x73, 0x76};
} // namespace vibe_wuzz
