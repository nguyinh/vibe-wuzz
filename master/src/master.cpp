#include <Arduino.h>

#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_ESP8266)

#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>

#ifdef ARDUINO_ARCH_ESP32
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#endif

#ifdef ARDUINO_ARCH_ESP8266
extern "C" {
#include <espnow.h>
#include <user_interface.h>
}

#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#endif

#include "buzzer_config.h"
#include "buzzer_logging.h"
#include "buzzer_protocol.h"

namespace vibe_wuzz {

namespace {

constexpr uint32_t kClientTimeoutMs = 10000;
constexpr uint32_t kHelloIntervalMs = 2000;
constexpr uint32_t kHeartbeatIntervalMs = 3000;
constexpr uint32_t kRetryIntervalMs = 250;
constexpr uint32_t kStatusLogIntervalMs = 5000;
constexpr uint8_t kMaxRetries = 6;

#ifdef ARDUINO_ARCH_ESP32
using MasterWebServer = WebServer;
using MasterStreamClient = WiFiClient;
#endif

#ifdef ARDUINO_ARCH_ESP8266
using MasterWebServer = ESP8266WebServer;
using MasterStreamClient = WiFiClient;
#endif

constexpr uint8_t kMaxSseClients = 2;
constexpr uint32_t kSseKeepAliveIntervalMs = 15000;
constexpr uint32_t kLabelStoreMagic = 0x56575A5A;
constexpr size_t kClientLabelMaxLength = 24;

struct StoredClientLabels {
  uint32_t magic;
  char labels[kMaxClients][kClientLabelMaxLength + 1];
};

struct ClientRuntime {
  const ClientDefinition *definition;
  char label[kClientLabelMaxLength + 1];
  bool online;
  uint32_t lastSeenMs;
  uint32_t lastHelloMs;
  uint32_t lastHeartbeatMs;
  uint32_t lastStateSequenceSeen;
  uint32_t lastBuzzSequenceSeen;
  uint32_t awaitingAckSequence;
  uint32_t ackDeadlineMs;
  uint8_t retriesRemaining;
  BuzzerPacket pendingStatePacket;
};

struct SseClientSlot {
  MasterStreamClient client;
  bool active;
};

MasterWebServer g_server(80);
Adafruit_NeoPixel g_masterStrip(kMasterStripLedCount, kMasterStripPin, NEO_GRB + NEO_KHZ800);
GameState g_state = GameState::Boot;
uint8_t g_winnerId = kNoWinner;
uint32_t g_nextSequence = 1;
uint32_t g_lastStatusLogMs = 0;
uint32_t g_lastSseKeepAliveMs = 0;
ClientRuntime g_clients[kMaxClients];
SseClientSlot g_sseClients[kMaxSseClients];
String g_lastStatusPayload;

uint32_t nextSequence() { return g_nextSequence++; }

String stateName(GameState state) { return String(gameStateName(state)); }

bool canArm(GameState state) {
  return state == GameState::Idle || state == GameState::Correct ||
         state == GameState::Incorrect;
}

bool canMarkCorrect(GameState state) { return state == GameState::Locked; }

bool canMarkIncorrect(GameState state) { return state == GameState::Locked; }

bool canReset(GameState state) { return state != GameState::Idle && state != GameState::Boot; }

String trimLabel(const String &value) {
  String trimmed = value;
  trimmed.trim();
  return trimmed;
}

String escapeHtml(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 16);
  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&':
        escaped += F("&amp;");
        break;
      case '<':
        escaped += F("&lt;");
        break;
      case '>':
        escaped += F("&gt;");
        break;
      case '"':
        escaped += F("&quot;");
        break;
      case '\'':
        escaped += F("&#39;");
        break;
      default:
        escaped += value[i];
        break;
    }
  }
  return escaped;
}

String escapeJson(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value[i];
    switch (ch) {
      case '\\':
      case '"':
        escaped += '\\';
        escaped += ch;
        break;
      case '\n':
        escaped += F("\\n");
        break;
      case '\r':
        escaped += F("\\r");
        break;
      case '\t':
        escaped += F("\\t");
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

void copyLabelToBuffer(char *destination, const String &label) {
  const String trimmed = trimLabel(label);
  const String fallback = trimmed.length() == 0 ? String("Player") : trimmed;
  fallback.substring(0, kClientLabelMaxLength).toCharArray(destination, kClientLabelMaxLength + 1);
}

String participantStatusLabel() {
  switch (g_state) {
    case GameState::Locked:
      return String("First buzz");
    case GameState::Correct:
      return String("Winner");
    case GameState::Incorrect:
      return String("Incorrect answer");
    default:
      return String("Player");
  }
}

uint32_t stripColor(uint8_t red, uint8_t green, uint8_t blue) {
  return g_masterStrip.Color(red, green, blue);
}

uint32_t masterStripColorForState(GameState state) {
  switch (state) {
    case GameState::Boot:
      return stripColor(96, 40, 0);
    case GameState::Idle:
      return stripColor(0, 90, 0);
    case GameState::Armed:
      return stripColor(0, 0, 96);
    case GameState::Locked:
      return stripColor(96, 0, 96);
    case GameState::Correct:
      return stripColor(0, 120, 0);
    case GameState::Incorrect:
      return stripColor(120, 0, 0);
  }

  return stripColor(0, 0, 0);
}

void renderMasterStrip(uint32_t color) {
  for (uint16_t index = 0; index < kMasterStripLedCount; ++index) {
    g_masterStrip.setPixelColor(index, color);
  }
  g_masterStrip.show();
}

void renderMasterStripForState() { renderMasterStrip(masterStripColorForState(g_state)); }

void setMasterStatusLed(bool on) {
  const uint8_t level = on
                            ? (kMasterStatusLedActiveLow ? LOW : HIGH)
                            : (kMasterStatusLedActiveLow ? HIGH : LOW);
  digitalWrite(kMasterStatusLedPin, level);
}

void setMasterReadyLed(bool on) {
  const uint8_t level = on
                            ? (kMasterReadyLedActiveLow ? LOW : HIGH)
                            : (kMasterReadyLedActiveLow ? HIGH : LOW);
  digitalWrite(kMasterReadyLedPin, level);
}

void initMasterStatusLed() {
  pinMode(kMasterStatusLedPin, OUTPUT);
  pinMode(kMasterReadyLedPin, OUTPUT);
  setMasterStatusLed(true);
  setMasterReadyLed(false);
  g_masterStrip.begin();
  g_masterStrip.setBrightness(kMasterStripBrightness);
  renderMasterStrip(masterStripColorForState(GameState::Boot));
}

void logMacLine(const char *label, const uint8_t mac[6]) {
  LOG_INFOF("%s %02X:%02X:%02X:%02X:%02X:%02X\n",
            label,
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5]);
}

String clientLabel(uint8_t clientId) {
  for (const auto &client : g_clients) {
    if (client.definition != nullptr && client.definition->id == clientId) {
      return String(client.label);
    }
  }

  return String("Unknown");
}

void loadStoredLabels() {
  EEPROM.begin(sizeof(StoredClientLabels));
  StoredClientLabels stored{};
  EEPROM.get(0, stored);

  const bool hasStoredLabels = stored.magic == kLabelStoreMagic;
  for (size_t index = 0; index < kMaxClients; ++index) {
    const String labelSource =
        hasStoredLabels && stored.labels[index][0] != '\0'
            ? String(stored.labels[index])
            : String(kClientDefinitions[index].label);
    copyLabelToBuffer(g_clients[index].label, labelSource);
  }
}

void saveStoredLabels() {
  StoredClientLabels stored{};
  stored.magic = kLabelStoreMagic;
  for (size_t index = 0; index < kMaxClients; ++index) {
    strncpy(stored.labels[index], g_clients[index].label, kClientLabelMaxLength);
    stored.labels[index][kClientLabelMaxLength] = '\0';
  }

  EEPROM.put(0, stored);
  EEPROM.commit();
}

void logStatusSummary() {
  uint8_t onlineCount = 0;
  for (const auto &client : g_clients) {
    if (client.online) {
      onlineCount++;
    }
  }

  LOG_DEBUGF("Status: state=%s winner=%u online=%u/%u\n",
             gameStateName(g_state),
             g_winnerId,
             onlineCount,
             kMaxClients);
}

String buildStatusJson() {
  String json;
  json.reserve(2048);
  json += "{\"state\":\"" + stateName(g_state) + "\",\"winnerId\":";
  json += (g_winnerId == kNoWinner) ? String("null") : String(g_winnerId);
  json += ",\"winnerLabel\":\"";
  json += escapeJson(participantStatusLabel());
  json += "\"";
  json += ",\"clients\":[";

  for (size_t index = 0; index < kMaxClients; ++index) {
    const auto &client = g_clients[index];
    if (index > 0) {
      json += ",";
    }

    json += "{\"id\":";
    json += String(client.definition->id);
    json += ",\"label\":\"";
    json += escapeJson(String(client.label));
    json += "\",\"mac\":\"";
    json += macToString(client.definition->mac);
    json += "\",\"online\":";
    json += client.online ? "true" : "false";
    json += ",\"lastSeenMs\":";
    json += String(client.lastSeenMs);
    json += ",\"awaitingAckSequence\":";
    json += String(client.awaitingAckSequence);
    json += "}";
  }

  json += "]}";
  return json;
}

void pruneSseClients() {
  for (auto &slot : g_sseClients) {
    if (slot.active && !slot.client.connected()) {
      slot.client.stop();
      slot.active = false;
    }
  }
}

void sendSsePayload(const String &payload) {
  pruneSseClients();
  for (auto &slot : g_sseClients) {
    if (!slot.active) {
      continue;
    }

    slot.client.print("data: ");
    slot.client.print(payload);
    slot.client.print("\n\n");
    if (!slot.client.connected()) {
      slot.client.stop();
      slot.active = false;
    }
  }
}

void broadcastStatusIfChanged(bool force = false) {
  const String payload = buildStatusJson();
  if (!force && payload == g_lastStatusPayload) {
    return;
  }

  g_lastStatusPayload = payload;
  sendSsePayload(payload);
}

ClientRuntime *findClientRuntimeByMac(const uint8_t mac[6]) {
  for (auto &client : g_clients) {
    if (client.definition != nullptr &&
        memcmp(client.definition->mac, mac, sizeof(client.definition->mac)) == 0) {
      return &client;
    }
  }

  return nullptr;
}

#ifdef ARDUINO_ARCH_ESP32
bool sendPacketToClient(const ClientRuntime &client, const BuzzerPacket &packet) {
  const esp_err_t result =
      esp_now_send(client.definition->mac, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));
  if (result != ESP_OK) {
    LOG_ERRORF("ESP-NOW send enqueue failed for client %u packet=%u err=%d\n",
               client.definition->id,
               static_cast<unsigned>(packet.type),
               static_cast<int>(result));
  }
  return result == ESP_OK;
}
#endif

#ifdef ARDUINO_ARCH_ESP8266
bool sendPacketToClient(const ClientRuntime &client, const BuzzerPacket &packet) {
  const int result = esp_now_send(const_cast<uint8_t *>(client.definition->mac),
                                  const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(&packet)),
                                  sizeof(packet));
  if (result != 0) {
    LOG_ERRORF("ESP-NOW send enqueue failed for client %u packet=%u err=%d\n",
               client.definition->id,
               static_cast<unsigned>(packet.type),
               result);
  }
  return result == 0;
}
#endif

bool sendStateToClient(ClientRuntime &client, bool critical, bool isRetry = false) {
  BuzzerPacket packet{};
  if (isRetry) {
    packet = client.pendingStatePacket;
  } else {
    packet = makePacket(PacketType::StateUpdate,
                        DeviceRole::Master,
                        0,
                        client.definition->id,
                        nextSequence());
    packet.state = g_state;
    packet.winnerId = g_winnerId;
  }

  if (!sendPacketToClient(client, packet)) {
    return false;
  }

  if (critical) {
    client.pendingStatePacket = packet;
    client.awaitingAckSequence = packet.sequence;
    client.ackDeadlineMs = millis() + kRetryIntervalMs;
    client.retriesRemaining = kMaxRetries;
  }

  return true;
}

void sendStateToAll(bool critical) {
  for (auto &client : g_clients) {
    sendStateToClient(client, critical);
  }
}

void sendAck(const ClientRuntime &client, uint32_t ackSequence, AckReason reason) {
  BuzzerPacket packet = makePacket(PacketType::Ack,
                                   DeviceRole::Master,
                                   0,
                                   client.definition->id,
                                   nextSequence());
  packet.ackSequence = ackSequence;
  packet.ackReason = reason;
  packet.state = g_state;
  packet.winnerId = g_winnerId;
  LOG_DEBUGF("ACK -> client=%u reason=%u ackSeq=%lu\n",
             client.definition->id,
             static_cast<unsigned>(reason),
             static_cast<unsigned long>(ackSequence));
  sendPacketToClient(client, packet);
}

void transitionTo(GameState nextState, uint8_t winnerId, bool critical = true) {
  g_state = nextState;
  g_winnerId = winnerId;
  renderMasterStripForState();
  sendStateToAll(critical);
  LOG_INFOF("Transition -> %s winner=%u\n", gameStateName(g_state), g_winnerId);
}

void handleControlAction(GameState nextState, const char *label) {
  const GameState previousState = g_state;
  switch (nextState) {
    case GameState::Armed:
      transitionTo(GameState::Armed, kNoWinner);
      break;
    case GameState::Correct:
      if (g_state == GameState::Locked) {
        transitionTo(GameState::Correct, g_winnerId);
      }
      break;
    case GameState::Incorrect:
      if (g_state == GameState::Locked) {
        transitionTo(GameState::Incorrect, g_winnerId);
      }
      break;
    case GameState::Idle:
      transitionTo(GameState::Idle, kNoWinner);
      break;
    default:
      break;
  }

  LOG_INFOF("Web action: %s previous=%s current=%s\n",
            label,
            gameStateName(previousState),
            gameStateName(g_state));
}

void handleRoot() {
  String html;
  html.reserve(8192);
  html += F(
      "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,"
      "initial-scale=1'><title>Vibe Wuzz</title><style>"
      ":root{--bg:#f3efe7;--panel:#fffdf8;--ink:#1b1f23;--blue:#2563eb;--green:#16a34a;--red:#dc2626;--violet:#7c3aed;"
      "--muted:#6b7280;--line:#d6d3d1;}body{font-family:Verdana,sans-serif;background:linear-gradient(135deg,"
      "#f3efe7,#e7f6f2);color:var(--ink);margin:0;padding:24px;}main{max-width:900px;margin:0 auto;background:"
      "var(--panel);border:1px solid var(--line);border-radius:16px;padding:24px;box-shadow:0 10px 30px rgba"
      "(0,0,0,.08);}h1{margin-top:0;}button{border:none;border-radius:999px;padding:12px 18px;margin:6px;"
      "font-size:15px;font-weight:700;color:white;cursor:pointer;}button.arm{background:var(--blue);}button.correct{"
      "background:var(--green);}button.incorrect{background:var(--red);}button.reset{background:var(--violet);}"
      "button:disabled{opacity:.45;cursor:not-allowed;}"
      "table{width:100%;border-collapse:"
      "collapse;margin-top:20px;}th,td{padding:10px;border-bottom:1px solid var(--line);text-align:left;}small{"
      "color:var(--muted);}code{background:#ebe7df;padding:2px 6px;border-radius:6px;}</style></head><body><main>");
  html += F("<h1>Vibe Wuzz Master</h1>");
  html += "<p>State: <strong id='state-value'>" + stateName(g_state) + "</strong></p>";
  if (g_winnerId != kNoWinner) {
    html += "<p><span id='participant-label'>" + participantStatusLabel() +
            "</span>: <strong id='participant-value'>" + clientLabel(g_winnerId) + "</strong></p>";
  } else {
    html += F("<p><span id='participant-label'>Player</span>: <strong id='participant-value'>None</strong></p>");
  }

  html += "<p><button id='arm-button' class='arm' type='button' onclick=\"sendAction('/arm')\"";
  if (!canArm(g_state)) {
    html += F(" disabled");
  }
  html += F(">Arm Round</button>");

  html += "<button id='correct-button' class='correct' type='button' onclick=\"sendAction('/correct')\"";
  if (!canMarkCorrect(g_state)) {
    html += F(" disabled");
  }
  html += F(">Mark Correct</button>");

  html += "<button id='incorrect-button' class='incorrect' type='button' onclick=\"sendAction('/incorrect')\"";
  if (!canMarkIncorrect(g_state)) {
    html += F(" disabled");
  }
  html += F(">Mark Incorrect</button>");

  html += "<button id='reset-button' class='reset' type='button' onclick=\"sendAction('/reset')\"";
  if (!canReset(g_state)) {
    html += F(" disabled");
  }
  html += F(">Reset</button></p>");
  html += F("<p><small>Status API: <code>GET /status</code></small></p>");
  html += F("<section><h2>Player Names</h2><form method='post' action='/names'>");
  for (const auto &client : g_clients) {
    html += "<p><label>Client " + String(client.definition->id) + " <input name='label" +
            String(client.definition->id) + "' maxlength='" + String(kClientLabelMaxLength) +
            "' value='" + escapeHtml(String(client.label)) +
            "'></label></p>";
  }
  html += F("<button type='submit'>Save Names</button></form></section>");
  html += F("<table><thead><tr><th>Client</th><th>Online</th><th>Last Seen (ms)</th><th>Pending ACK</th></tr></thead><tbody id='clients-body'>");

  for (const auto &client : g_clients) {
    html += "<tr><td>" + escapeHtml(String(client.label)) + "</td><td>" +
            String(client.online ? "yes" : "no") + "</td><td>" + String(client.lastSeenMs) +
            "</td><td>" + String(client.awaitingAckSequence) + "</td></tr>";
  }

  html += F("</tbody></table><script>"
            "function escapeHtml(v){return String(v).replace(/[&<>\"']/g,function(ch){return({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'})[ch];});}"
            "function sendAction(path){"
            "fetch(path,{method:'POST'}).catch(()=>{});"
            "}"
            "function applyButtonStates(state){"
            "var canArm=(state==='IDLE'||state==='CORRECT'||state==='INCORRECT');"
            "var isLocked=state==='LOCKED';"
            "var isBoot=state==='BOOT';"
            "document.getElementById('arm-button').disabled=!canArm;"
            "document.getElementById('correct-button').disabled=!isLocked;"
            "document.getElementById('incorrect-button').disabled=!isLocked;"
            "document.getElementById('reset-button').disabled=((state==='IDLE')||isBoot);"
            "}"
            "function renderStatus(data){"
            "document.getElementById('state-value').textContent=data.state;"
            "applyButtonStates(data.state);"
            "var label=document.getElementById('participant-label');"
            "var value=document.getElementById('participant-value');"
            "label.textContent=data.winnerLabel||'Player';"
            "if(data.winnerId===null){value.textContent='None';}"
            "else{var winner=(data.clients||[]).find(c=>c.id===data.winnerId);value.textContent=winner?winner.label:('Client '+data.winnerId);}"
            "var rows=(data.clients||[]).map(c=>'<tr><td>'+escapeHtml(c.label)+'</td><td>'+(c.online?'yes':'no')+'</td><td>'+c.lastSeenMs+'</td><td>'+c.awaitingAckSequence+'</td></tr>').join('');"
            "document.getElementById('clients-body').innerHTML=rows;"
            "}"
            "if(window.EventSource){"
            "var es=new EventSource('/events');"
            "es.onmessage=function(ev){renderStatus(JSON.parse(ev.data));};"
            "es.onerror=function(){es.close();setInterval(function(){fetch('/status').then(r=>r.json()).then(renderStatus).catch(()=>{});},1000);};"
            "}else{setInterval(function(){fetch('/status').then(r=>r.json()).then(renderStatus).catch(()=>{});},1000);}"
            "</script></main></body></html>");
  g_server.send(200, "text/html", html);
}

void handleStatus() {
  g_server.send(200, "application/json", buildStatusJson());
}

void handleEvents() {
  g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  g_server.sendHeader("Cache-Control", "no-cache");
  g_server.sendHeader("Connection", "keep-alive");
  g_server.send(200, "text/event-stream", "");

#ifdef ARDUINO_ARCH_ESP32
  MasterStreamClient client = g_server.client();
#endif
#ifdef ARDUINO_ARCH_ESP8266
  MasterStreamClient client = g_server.client();
#endif
  client.setNoDelay(true);

  for (auto &slot : g_sseClients) {
    if (!slot.active) {
      slot.client = client;
      slot.active = true;
      slot.client.print("retry: 1000\n");
      slot.client.print("data: ");
      slot.client.print(buildStatusJson());
      slot.client.print("\n\n");
      return;
    }
  }

  client.print("event: full\n");
  client.print("data: busy\n\n");
  client.stop();
}

void handleArm() {
  handleControlAction(GameState::Armed, "arm");
  g_server.sendHeader("Location", "/");
  g_server.send(303);
}

void handleCorrect() {
  handleControlAction(GameState::Correct, "correct");
  g_server.sendHeader("Location", "/");
  g_server.send(303);
}

void handleIncorrect() {
  handleControlAction(GameState::Incorrect, "incorrect");
  g_server.sendHeader("Location", "/");
  g_server.send(303);
}

void handleReset() {
  handleControlAction(GameState::Idle, "reset");
  g_server.sendHeader("Location", "/");
  g_server.send(303);
}

void handleNames() {
  bool changed = false;
  for (auto &client : g_clients) {
    const String field = String("label") + client.definition->id;
    if (!g_server.hasArg(field)) {
      continue;
    }

    const String candidate = trimLabel(g_server.arg(field));
    const String nextLabel =
        candidate.length() == 0 ? String(client.definition->label) : candidate;
    if (String(client.label) != nextLabel) {
      copyLabelToBuffer(client.label, nextLabel);
      LOG_INFOF("Player label updated: id=%u label=%s\n",
                client.definition->id,
                client.label);
      changed = true;
    }
  }

  if (changed) {
    saveStoredLabels();
    broadcastStatusIfChanged(true);
  }

  g_server.sendHeader("Location", "/");
  g_server.send(303);
}

void registerRoutes() {
  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/events", HTTP_GET, handleEvents);
  g_server.on("/status", HTTP_GET, handleStatus);
  g_server.on("/names", HTTP_POST, handleNames);
  g_server.on("/arm", HTTP_POST, handleArm);
  g_server.on("/correct", HTTP_POST, handleCorrect);
  g_server.on("/incorrect", HTTP_POST, handleIncorrect);
  g_server.on("/reset", HTTP_POST, handleReset);
}

void setupClients() {
  for (size_t index = 0; index < kMaxClients; ++index) {
    g_clients[index] = {};
    g_clients[index].definition = &kClientDefinitions[index];
  }
  loadStoredLabels();
}

void addEspNowPeers() {
  for (const auto &client : g_clients) {
    logMacLine("Config client MAC:", client.definition->mac);

#ifdef ARDUINO_ARCH_ESP32
    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, client.definition->mac, sizeof(client.definition->mac));
    peer.channel = kEspNowChannel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    const esp_err_t result = esp_now_add_peer(&peer);
    if (result != ESP_OK) {
      LOG_ERRORF("Failed to add peer for client %u err=%d\n",
                 client.definition->id,
                 static_cast<int>(result));
    } else {
      LOG_INFOF("Peer added for client %u on channel %u\n",
                client.definition->id,
                kEspNowChannel);
    }
#endif

#ifdef ARDUINO_ARCH_ESP8266
    const int result = esp_now_add_peer(const_cast<uint8_t *>(client.definition->mac),
                                        ESP_NOW_ROLE_COMBO,
                                        kEspNowChannel,
                                        nullptr,
                                        0);
    if (result != 0) {
      LOG_ERRORF("Failed to add peer for client %u err=%d\n",
                 client.definition->id,
                 result);
    } else {
      LOG_INFOF("Peer added for client %u on channel %u\n",
                client.definition->id,
                kEspNowChannel);
    }
#endif
  }
}

void handleStateAck(ClientRuntime &client, const BuzzerPacket &packet) {
  if (packet.ackSequence == client.awaitingAckSequence &&
      packet.ackReason == AckReason::StateReceived) {
    LOG_DEBUGF("ACK <- client=%u ackSeq=%lu\n",
               client.definition->id,
               static_cast<unsigned long>(packet.ackSequence));
    client.awaitingAckSequence = 0;
    client.retriesRemaining = 0;
  }
}

void handleClientBuzz(ClientRuntime &client, const BuzzerPacket &packet) {
  if (packet.sequence <= client.lastBuzzSequenceSeen) {
    LOG_DEBUGF("Duplicate buzz ignored from client=%u seq=%lu\n",
               client.definition->id,
               static_cast<unsigned long>(packet.sequence));
    sendAck(client, packet.sequence, AckReason::BuzzReceived);
    return;
  }

  client.lastBuzzSequenceSeen = packet.sequence;
  LOG_INFOF("Buzz <- client=%u seq=%lu state=%s\n",
            client.definition->id,
            static_cast<unsigned long>(packet.sequence),
            gameStateName(g_state));
  sendAck(client, packet.sequence, AckReason::BuzzReceived);

  if (g_state != GameState::Armed) {
    LOG_DEBUGF("Buzz ignored from client=%u because state=%s\n",
               client.definition->id,
               gameStateName(g_state));
    broadcastStatusIfChanged();
    return;
  }

  transitionTo(GameState::Locked, client.definition->id);
}

void handlePresence(ClientRuntime &client) {
  if (!client.online) {
      LOG_INFOF("Client online: id=%u label=%s\n",
                client.definition->id,
                client.label);
  }
  client.online = true;
  client.lastSeenMs = millis();
}

#ifdef ARDUINO_ARCH_ESP32
void onEspNowReceive(const uint8_t *macAddr, const uint8_t *data, int length) {
  if (length != sizeof(BuzzerPacket) || macAddr == nullptr) {
    return;
  }

  BuzzerPacket packet{};
  memcpy(&packet, data, sizeof(packet));

  if (packet.version != kProtocolVersion || packet.role != DeviceRole::Client) {
    return;
  }

  ClientRuntime *client = findClientRuntimeByMac(macAddr);
  if (client == nullptr) {
    logMacLine("Received packet from unknown client:", macAddr);
    return;
  }

  handlePresence(*client);

  switch (packet.type) {
    case PacketType::Hello:
    case PacketType::Heartbeat:
      break;
    case PacketType::Buzz:
      handleClientBuzz(*client, packet);
      break;
    case PacketType::Ack:
      handleStateAck(*client, packet);
      break;
    case PacketType::StateUpdate:
      break;
  }
}
#endif

#ifdef ARDUINO_ARCH_ESP8266
void onEspNowReceive(uint8_t *macAddr, uint8_t *data, uint8_t length) {
  if (length != sizeof(BuzzerPacket) || macAddr == nullptr) {
    return;
  }

  BuzzerPacket packet{};
  memcpy(&packet, data, sizeof(packet));

  if (packet.version != kProtocolVersion || packet.role != DeviceRole::Client) {
    return;
  }

  ClientRuntime *client = findClientRuntimeByMac(macAddr);
  if (client == nullptr) {
    logMacLine("Received packet from unknown client:", macAddr);
    return;
  }

  handlePresence(*client);

  switch (packet.type) {
    case PacketType::Hello:
    case PacketType::Heartbeat:
      break;
    case PacketType::Buzz:
      handleClientBuzz(*client, packet);
      break;
    case PacketType::Ack:
      handleStateAck(*client, packet);
      break;
    case PacketType::StateUpdate:
      break;
  }
}
#endif

void initEspNow() {
#ifdef ARDUINO_ARCH_ESP32
  const esp_err_t initResult = esp_now_init();
  if (initResult != ESP_OK) {
    LOG_ERRORF("ESP-NOW init failed err=%d\n", static_cast<int>(initResult));
    return;
  }

  const esp_err_t channelResult = esp_wifi_set_channel(kEspNowChannel, WIFI_SECOND_CHAN_NONE);
  LOG_INFOF("ESP-NOW init ok, channel set result=%d\n", static_cast<int>(channelResult));
  esp_now_register_recv_cb(onEspNowReceive);
#endif

#ifdef ARDUINO_ARCH_ESP8266
  wifi_set_channel(kEspNowChannel);
  const int initResult = esp_now_init();
  if (initResult != 0) {
    LOG_ERRORF("ESP-NOW init failed err=%d\n", initResult);
    return;
  }

  const int roleResult = esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  LOG_INFOF("ESP-NOW init ok, role set result=%d\n", roleResult);
  esp_now_register_recv_cb(onEspNowReceive);
#endif

  addEspNowPeers();
}

void initWifiAndWeb() {
  WiFi.mode(WIFI_AP_STA);
#ifdef ARDUINO_ARCH_ESP32
  WiFi.setSleep(false);
#endif
  WiFi.softAP(kMasterApSsid, kMasterApPassword, kEspNowChannel);
  LOG_INFOF("Master STA MAC: %s\n", WiFi.macAddress().c_str());
  LOG_INFOF("Master AP  MAC: %s\n", WiFi.softAPmacAddress().c_str());
  LOG_INFOF("ESP-NOW channel: %u\n", kEspNowChannel);
  LOG_INFOF("Master AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  if (MDNS.begin(kMasterHostname)) {
    MDNS.addService("http", "tcp", 80);
    LOG_INFOF("mDNS ready: http://%s.local\n", kMasterHostname);
  } else {
    LOG_ERRORLN("mDNS init failed");
  }

  registerRoutes();
  g_server.begin();
}

void sendPresenceProbe(ClientRuntime &client, PacketType packetType) {
  BuzzerPacket packet =
      makePacket(packetType, DeviceRole::Master, 0, client.definition->id, nextSequence());
  packet.state = g_state;
  packet.winnerId = g_winnerId;
  sendPacketToClient(client, packet);
}

void maintainClients() {
  const uint32_t now = millis();

  for (auto &client : g_clients) {
    if (client.online && (now - client.lastSeenMs > kClientTimeoutMs)) {
      client.online = false;
      LOG_INFOF("Client offline: id=%u label=%s timeoutMs=%lu\n",
                client.definition->id,
                client.label,
                static_cast<unsigned long>(kClientTimeoutMs));
      broadcastStatusIfChanged(true);
    }

    if (now - client.lastHelloMs >= kHelloIntervalMs) {
      client.lastHelloMs = now;
      sendPresenceProbe(client, PacketType::Hello);
    }

    if (now - client.lastHeartbeatMs >= kHeartbeatIntervalMs) {
      client.lastHeartbeatMs = now;
      sendPresenceProbe(client, PacketType::Heartbeat);
    }

    if (client.awaitingAckSequence != 0 && now >= client.ackDeadlineMs) {
      if (client.retriesRemaining == 0) {
        LOG_ERRORF("ACK timeout for client %u seq=%lu\n",
                   client.definition->id,
                   static_cast<unsigned long>(client.awaitingAckSequence));
        client.awaitingAckSequence = 0;
      } else {
        client.retriesRemaining--;
        client.ackDeadlineMs = now + kRetryIntervalMs;
        sendStateToClient(client, true, true);
      }
    }
  }

  if (now - g_lastStatusLogMs >= kStatusLogIntervalMs) {
    g_lastStatusLogMs = now;
    logStatusSummary();
  }

  if (now - g_lastSseKeepAliveMs >= kSseKeepAliveIntervalMs) {
    g_lastSseKeepAliveMs = now;
    pruneSseClients();
    for (auto &slot : g_sseClients) {
      if (slot.active) {
        slot.client.print(": keep-alive\n\n");
      }
    }
  }

  broadcastStatusIfChanged();
}

}  // namespace

void appSetup() {
  Serial.begin(115200);
  delay(250);
  initMasterStatusLed();
  LOG_INFOLN("=== Vibe Wuzz Master Boot ===");
  setupClients();
  initWifiAndWeb();
  initEspNow();
  transitionTo(GameState::Idle, kNoWinner, true);
  logStatusSummary();
  setMasterStatusLed(false);
  setMasterReadyLed(true);
  LOG_INFOF("Master ready indicator ON pin=%u\n", kMasterReadyLedPin);
}

void appLoop() {
  g_server.handleClient();
  maintainClients();
}

}  // namespace vibe_wuzz

void setup() { vibe_wuzz::appSetup(); }

void loop() { vibe_wuzz::appLoop(); }

#endif
