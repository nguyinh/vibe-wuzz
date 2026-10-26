#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace vibe_wuzz {

constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kMaxClients = 4;
constexpr uint8_t kBroadcastClientId = 0xFF;
constexpr uint8_t kNoWinner = 0xFF;

enum class DeviceRole : uint8_t {
  Master = 1,
  Client = 2,
};

enum class GameState : uint8_t {
  Boot = 0,
  Idle = 1,
  Armed = 2,
  Locked = 3,
  Correct = 4,
  Incorrect = 5,
};

enum class PacketType : uint8_t {
  Hello = 1,
  StateUpdate = 2,
  Buzz = 3,
  Ack = 4,
  Heartbeat = 5,
};

enum class AckReason : uint8_t {
  None = 0,
  StateReceived = 1,
  BuzzReceived = 2,
};

struct __attribute__((packed)) BuzzerPacket {
  uint8_t version;
  PacketType type;
  DeviceRole role;
  uint8_t senderId;
  uint8_t targetId;
  uint32_t sequence;
  uint32_t ackSequence;
  GameState state;
  uint8_t winnerId;
  AckReason ackReason;
  uint32_t uptimeMs;
};

inline BuzzerPacket makePacket(PacketType type,
                               DeviceRole role,
                               uint8_t senderId,
                               uint8_t targetId,
                               uint32_t sequence) {
  BuzzerPacket packet{};
  packet.version = kProtocolVersion;
  packet.type = type;
  packet.role = role;
  packet.senderId = senderId;
  packet.targetId = targetId;
  packet.sequence = sequence;
  packet.ackSequence = 0;
  packet.state = GameState::Boot;
  packet.winnerId = kNoWinner;
  packet.ackReason = AckReason::None;
  packet.uptimeMs = millis();
  return packet;
}

}  // namespace vibe_wuzz
