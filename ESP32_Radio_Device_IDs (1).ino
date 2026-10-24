#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// =====================================================
// ESP32 Radio: local chat with stable device IDs
// Device IDs survive browser refreshes and ESP32 reboots.
// IDs are assigned to browser tokens stored in localStorage.
// This is identification, NOT authentication: users on the
// same Wi-Fi can still spoof requests if they modify them.
// =====================================================

const char* ssid = "ESP32_Radio";
const char* password = "radio123";

WebServer server(80);
Preferences prefs;

const int MAX_DEVICES = 10;
const size_t MAX_CHAT_LENGTH = 1400;
const size_t MAX_MESSAGE_LENGTH = 200;

String chatLog = "ESP32 Radio is online!\n";
unsigned long messageCount = 0;
unsigned long requestCount = 0;

String slotKey(int slot) {
  return "tok" + String(slot); // NVS keys are short enough
}

String deviceIdForSlot(int slot) {
  char id[20];
  snprintf(id, sizeof(id), "DEVICE-%04d", slot + 1);
  return String(id);
}

bool isValidToken(const String& token) {
  if (token.length() != 32) return false;
  for (size_t i = 0; i < token.length(); i++) {
    char c = token[i];
    bool hex = (c >= '0' && c <= '9') ||
               (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return true;
}

int findDeviceSlot(const String& token) {
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (prefs.getString(slotKey(i).c_str(), "") == token) {
      return i;
    }
  }
  return -1;
}

int registerDeviceToken(const String& token) {
  int existing = findDeviceSlot(token);
  if (existing >= 0) return existing;

  for (int i = 0; i < MAX_DEVICES; i++) {
    String saved = prefs.getString(slotKey(i).c_str(), "");
    if (saved.length() == 0) {
      prefs.putString(slotKey(i).c_str(), token);
      return i;
    }
  }
  return -1; // Registry is full
}

String cleanMessage(String msg) {
  msg.replace("\r", " ");
  msg.replace("\n", " ");
  msg.trim();
  return msg;
}

// =====================================================
// CHAT PAGE
// =====================================================

const char chatPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Radio</title>
<style>
* { box-sizing: border-box; }
body {
  font-family: Arial, sans-serif; max-width: 600px; margin: 25px auto;
  padding: 15px; background: #101820; color: white;
}
h2 { color: #65e6b5; }
.card { background: #202c38; padding: 12px; border-radius: 10px; margin: 12px 0; }
#messages {
  background: #202c38; padding: 15px; height: 340px; overflow-y: auto;
  white-space: pre-wrap; overflow-wrap: anywhere; border-radius: 10px;
}
input, button { padding: 12px; margin-top: 10px; border-radius: 6px; font-size: 16px; }
input { width: 72%; background: #f7fafc; border: 0; }
button { background: #65e6b5; border: 0; cursor: pointer; }
a { color: #65e6b5; }
.small { color: #a8bacb; font-size: 13px; }
#deviceId { color: #65e6b5; font-weight: bold; overflow-wrap: anywhere; }
</style>
</head>
<body>
<h2>ESP32 Radio</h2>
<p class="small">Offline messaging network</p>
<div class="card">
  <div class="small">YOUR DEVICE ID</div>
  <div id="deviceId">Registering...</div>
  <div class="small">This ID is saved in this browser.</div>
</div>
<div id="messages">Connecting...</div>
<form id="form">
  <input id="msg" maxlength="200" placeholder="Type a message..." required>
  <button type="submit">Send</button>
</form>
<p><a href="/admin">System Admin Panel →</a></p>
<p class="small" id="status">Connecting to ESP32...</p>
<script>
const messages = document.getElementById('messages');
const input = document.getElementById('msg');
const idLabel = document.getElementById('deviceId');
const statusLabel = document.getElementById('status');
let deviceToken = '';
let deviceId = '';

function makeToken() {
  const bytes = new Uint8Array(16);
  if (window.crypto && window.crypto.getRandomValues) {
    window.crypto.getRandomValues(bytes);
  } else {
    for (let i = 0; i < bytes.length; i++) bytes[i] = Math.floor(Math.random() * 256);
  }
  return Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
}

async function registerDevice() {
  deviceToken = localStorage.getItem('esp32RadioToken') || '';
  if (!/^[0-9a-f]{32}$/i.test(deviceToken)) {
    deviceToken = makeToken();
    localStorage.setItem('esp32RadioToken', deviceToken);
  }

  const r = await fetch('/register', {
    method: 'POST',
    headers: {'Content-Type': 'text/plain'},
    body: deviceToken
  });
  if (!r.ok) throw new Error(await r.text());
  deviceId = (await r.text()).trim();
  idLabel.textContent = deviceId;
  statusLabel.textContent = 'Connected to ESP32 Radio';
}

async function refresh() {
  try {
    const r = await fetch('/messages');
    if (!r.ok) throw new Error('Request failed');
    const text = await r.text();
    if (messages.textContent !== text) {
      messages.textContent = text;
      messages.scrollTop = messages.scrollHeight;
    }
  } catch (e) {
    statusLabel.textContent = 'Chat connection interrupted; retrying...';
  }
}

document.getElementById('form').onsubmit = async (e) => {
  e.preventDefault();
  const message = input.value.trim();
  if (!message || !deviceId) return;
  input.value = '';

  try {
    const body = new URLSearchParams({token: deviceToken, message: message});
    const r = await fetch('/send', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: body
    });
    if (!r.ok) throw new Error(await r.text());
    await refresh();
  } catch (e) {
    statusLabel.textContent = 'Could not send message. Please try again.';
    input.value = message;
  }
};

(async () => {
  try {
    await registerDevice();
    await refresh();
    setInterval(refresh, 1000);
  } catch (e) {
    idLabel.textContent = 'Registration failed';
    statusLabel.textContent = 'Could not register: ' + e.message;
  }
})();
</script>
</body>
</html>
)rawliteral";

// =====================================================
// ADMIN PAGE
// =====================================================

const char adminPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Admin</title>
<style>
body { background:#101820; color:#e8f0f8; font-family:Arial,sans-serif; max-width:850px; margin:25px auto; padding:15px; }
h2 { color:#65e6b5; }
.grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(145px,1fr)); gap:12px; }
.card { background:#202c38; padding:16px; border-radius:12px; overflow-wrap:anywhere; }
.value { font-size:23px; font-weight:bold; margin-top:8px; }
.label, small { color:#a8bacb; font-size:13px; }
a { color:#65e6b5; }
</style>
</head>
<body>
<h2>ESP32 System Monitor</h2>
<p>Radio hub infrastructure dashboard</p>
<div class="grid">
  <div class="card"><div class="label">Free Heap</div><div class="value" id="heap">--</div><small id="heapDetail"></small></div>
  <div class="card"><div class="label">CPU Frequency</div><div class="value" id="cpu">--</div></div>
  <div class="card"><div class="label">Flash Capacity</div><div class="value" id="flash">--</div></div>
  <div class="card"><div class="label">Firmware Size</div><div class="value" id="firmware">--</div><small id="ota"></small></div>
  <div class="card"><div class="label">Uptime</div><div class="value" id="uptime">--</div></div>
  <div class="card"><div class="label">Connected Wi-Fi Devices</div><div class="value" id="clients">--</div></div>
  <div class="card"><div class="label">Registered Browser IDs</div><div class="value" id="registered">--</div></div>
  <div class="card"><div class="label">Messages Received</div><div class="value" id="msgCount">--</div></div>
  <div class="card"><div class="label">HTTP Requests</div><div class="value" id="reqCount">--</div></div>
  <div class="card"><div class="label">Minimum Free Heap</div><div class="value" id="minHeap">--</div></div>
</div>
<p id="status">Reading system metrics...</p>
<small>Refresh interval: 2 seconds</small>
<p><a href="/">← Back to Radio Chat</a></p>
<script>
async function update() {
  try {
    const response = await fetch('/api/status');
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const d = await response.json();
    document.getElementById('heap').textContent = d.freeHeapKB + ' KB';
    document.getElementById('heapDetail').textContent = 'Used: ' + d.usedHeapKB + ' / ' + d.totalHeapKB + ' KB';
    document.getElementById('cpu').textContent = d.cpuMHz + ' MHz';
    document.getElementById('flash').textContent = d.flashMB + ' MB';
    document.getElementById('firmware').textContent = d.firmwareKB + ' KB';
    document.getElementById('ota').textContent = 'Free app slot: ' + d.freeOtaKB + ' KB';
    document.getElementById('uptime').textContent = Math.floor(d.uptimeSeconds / 60) + ' min';
    document.getElementById('clients').textContent = d.clients;
    document.getElementById('registered').textContent = d.registered;
    document.getElementById('msgCount').textContent = d.messages;
    document.getElementById('reqCount').textContent = d.requests;
    document.getElementById('minHeap').textContent = d.minHeapKB + ' KB';
    document.getElementById('status').textContent = 'ONLINE | ' + d.chip + ' | Updated ' + new Date().toLocaleTimeString();
  } catch (e) {
    document.getElementById('status').textContent = 'Unable to reach ESP32: ' + e.message;
  }
}
update();
setInterval(update, 2000);
</script>
</body>
</html>
)rawliteral";

// =====================================================
// API HANDLERS
// =====================================================

void handleChat() {
  requestCount++;
  server.send_P(200, "text/html", chatPage);
}

void handleMessages() {
  requestCount++;
  server.send(200, "text/plain; charset=utf-8", chatLog);
}

void handleRegister() {
  requestCount++;
  String token = server.arg("plain");
  token.trim();

  if (!isValidToken(token)) {
    server.send(400, "text/plain", "Invalid client token");
    return;
  }

  int slot = registerDeviceToken(token);
  if (slot < 0) {
    server.send(503, "text/plain", "Device registry full");
    return;
  }

  server.send(200, "text/plain", deviceIdForSlot(slot));
}

void handleSend() {
  requestCount++;

  String token = server.arg("token");
  String msg = cleanMessage(server.arg("message"));

  if (!isValidToken(token) || findDeviceSlot(token) < 0) {
    server.send(403, "text/plain", "Device not registered. Refresh the chat page.");
    return;
  }
  if (msg.length() == 0 || msg.length() > MAX_MESSAGE_LENGTH) {
    server.send(400, "text/plain", "Message must be between 1 and 200 characters");
    return;
  }

  int slot = findDeviceSlot(token);
  String line = "[" + deviceIdForSlot(slot) + "] " + msg + "\n";

  while (chatLog.length() + line.length() > MAX_CHAT_LENGTH) {
    int newline = chatLog.indexOf('\n');
    if (newline < 0) {
      chatLog = "";
      break;
    }
    chatLog.remove(0, newline + 1);
  }

  chatLog += line;
  messageCount++;

  Serial.print("Message from ");
  Serial.print(deviceIdForSlot(slot));
  Serial.print(": ");
  Serial.println(msg);

  server.send(200, "text/plain", "Message received");
}

void handleAdmin() {
  requestCount++;
  server.send_P(200, "text/html", adminPage);
}

int registeredDeviceCount() {
  int count = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (prefs.getString(slotKey(i).c_str(), "").length() > 0) count++;
  }
  return count;
}

void handleStatus() {
  requestCount++;

  uint32_t totalHeap = ESP.getHeapSize();
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t flashSize = ESP.getFlashChipSize();
  uint32_t firmwareSize = ESP.getSketchSize();
  uint32_t freeOta = ESP.getFreeSketchSpace();

  char json[512];
  snprintf(
    json, sizeof(json),
    "{"
      "\"chip\":\"%s\","
      "\"freeHeapKB\":%u,"
      "\"usedHeapKB\":%u,"
      "\"totalHeapKB\":%u,"
      "\"minHeapKB\":%u,"
      "\"cpuMHz\":%u,"
      "\"flashMB\":%.2f,"
      "\"firmwareKB\":%.1f,"
      "\"freeOtaKB\":%.1f,"
      "\"uptimeSeconds\":%lu,"
      "\"clients\":%u,"
      "\"registered\":%u,"
      "\"messages\":%lu,"
      "\"requests\":%lu"
    "}",
    ESP.getChipModel(),
    (unsigned)(freeHeap / 1024),
    (unsigned)((totalHeap - freeHeap) / 1024),
    (unsigned)(totalHeap / 1024),
    (unsigned)(ESP.getMinFreeHeap() / 1024),
    (unsigned)ESP.getCpuFreqMHz(),
    flashSize / 1048576.0,
    firmwareSize / 1024.0,
    freeOta / 1024.0,
    (unsigned long)(millis() / 1000),
    (unsigned)WiFi.softAPgetStationNum(),
    (unsigned)registeredDeviceCount(),
    messageCount,
    requestCount
  );

  server.send(200, "application/json", json);
}

// =====================================================
// SETUP & LOOP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("Starting ESP32 Radio...");

  if (!prefs.begin("radio", false)) {
    Serial.println("Warning: Preferences/NVS could not be opened.");
  }

  WiFi.mode(WIFI_AP);
  // Request up to 10 connected stations. Actual limits depend on
  // the ESP32 variant and Arduino-ESP32 core version.
  if (!WiFi.softAP(ssid, password, 1, 0, 10)) {
    Serial.println("Failed to start hotspot!");
    return;
  }

  Serial.println("Hotspot started!");
  Serial.print("Wi-Fi name: ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleChat);
  server.on("/messages", HTTP_GET, handleMessages);
  server.on("/register", HTTP_POST, handleRegister);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/api/status", HTTP_GET, handleStatus);

  server.onNotFound([]() {
    server.send(404, "text/plain", "404 Not Found");
  });

  server.begin();
  Serial.println("HTTP server started!");
  Serial.println("Chat:  http://192.168.4.1/");
  Serial.println("Admin: http://192.168.4.1/admin");
}

void loop() {
  server.handleClient();
}
