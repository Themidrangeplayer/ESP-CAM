#include <Arduino.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <vector>

const char* WIFI_SSID = "REPLACE_WITH_SSID";
const char* WIFI_PASSWORD = "REPLACE_WITH_PASSWORD";
constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 30000;
constexpr size_t MAX_EVENTS = 150;
constexpr size_t MAX_NOTIFICATIONS = 120;

WebServer server(80);

struct CameraRecord {
  String id;
  String name;
  String groupName;
  String location;
  String streamUrl;
  bool online;
  int battery;
  int rssi;
  uint32_t lastSeenMs;
  bool motionDetected;
  bool humanDetected;
  bool faceDetected;
  String lastFaceName;
  uint32_t lastDetectionMs;
};

struct EventRecord {
  uint32_t id;
  String cameraId;
  String type;
  String details;
  uint32_t timestampMs;
};

struct NotificationRecord {
  uint32_t id;
  String cameraId;
  String title;
  String message;
  String priority;
  bool acknowledged;
  uint32_t timestampMs;
};

std::vector<CameraRecord> cameras;
std::vector<EventRecord> events;
std::vector<NotificationRecord> notifications;
String alertWebhookUrl = "";
uint32_t nextEventId = 1;
uint32_t nextNotificationId = 1;

String jsonEscape(const String& value) {
  String out = "";
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    const char c = value[i];
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else {
      out += c;
    }
  }
  return out;
}

bool parseBoolArg(const String& value) {
  return value == "1" || value == "true" || value == "TRUE" ||
         value == "yes" || value == "on";
}

CameraRecord* findCameraById(const String& id) {
  for (auto& camera : cameras) {
    if (camera.id == id) {
      return &camera;
    }
  }
  return nullptr;
}

CameraRecord& upsertCamera(const String& id) {
  CameraRecord* camera = findCameraById(id);
  if (camera != nullptr) {
    return *camera;
  }

  CameraRecord created = {id, id, "default", "unknown", "", false, -1, -999, 0,
                          false, false, false, "", 0};
  cameras.push_back(created);
  return cameras.back();
}

void refreshHealth() {
  const uint32_t nowMs = millis();
  for (auto& camera : cameras) {
    const uint32_t elapsed =
        (camera.lastSeenMs == 0) ? HEARTBEAT_TIMEOUT_MS + 1 : nowMs - camera.lastSeenMs;
    camera.online = elapsed <= HEARTBEAT_TIMEOUT_MS;
  }
}

void trimQueues() {
  while (events.size() > MAX_EVENTS) {
    events.erase(events.begin());
  }
  while (notifications.size() > MAX_NOTIFICATIONS) {
    notifications.erase(notifications.begin());
  }
}

String cameraAsJson(const CameraRecord& camera) {
  String out = "{";
  out += "\"id\":\"" + jsonEscape(camera.id) + "\",";
  out += "\"name\":\"" + jsonEscape(camera.name) + "\",";
  out += "\"group\":\"" + jsonEscape(camera.groupName) + "\",";
  out += "\"location\":\"" + jsonEscape(camera.location) + "\",";
  out += "\"streamUrl\":\"" + jsonEscape(camera.streamUrl) + "\",";
  out += "\"online\":" + String(camera.online ? "true" : "false") + ",";
  out += "\"battery\":" + String(camera.battery) + ",";
  out += "\"rssi\":" + String(camera.rssi) + ",";
  out += "\"motionDetected\":" + String(camera.motionDetected ? "true" : "false") + ",";
  out += "\"humanDetected\":" + String(camera.humanDetected ? "true" : "false") + ",";
  out += "\"faceDetected\":" + String(camera.faceDetected ? "true" : "false") + ",";
  out += "\"lastFaceName\":\"" + jsonEscape(camera.lastFaceName) + "\",";
  out += "\"lastDetectionMs\":" + String(camera.lastDetectionMs) + ",";
  out += "\"lastSeenMs\":" + String(camera.lastSeenMs);
  out += "}";
  return out;
}

String eventAsJson(const EventRecord& event) {
  String out = "{";
  out += "\"id\":" + String(event.id) + ",";
  out += "\"cameraId\":\"" + jsonEscape(event.cameraId) + "\",";
  out += "\"type\":\"" + jsonEscape(event.type) + "\",";
  out += "\"details\":\"" + jsonEscape(event.details) + "\",";
  out += "\"timestampMs\":" + String(event.timestampMs);
  out += "}";
  return out;
}

String notificationAsJson(const NotificationRecord& item) {
  String out = "{";
  out += "\"id\":" + String(item.id) + ",";
  out += "\"cameraId\":\"" + jsonEscape(item.cameraId) + "\",";
  out += "\"title\":\"" + jsonEscape(item.title) + "\",";
  out += "\"message\":\"" + jsonEscape(item.message) + "\",";
  out += "\"priority\":\"" + jsonEscape(item.priority) + "\",";
  out += "\"acknowledged\":" + String(item.acknowledged ? "true" : "false") + ",";
  out += "\"timestampMs\":" + String(item.timestampMs);
  out += "}";
  return out;
}

void sendWebhookNotification(const NotificationRecord& item) {
  if (alertWebhookUrl.length() == 0 || WiFi.status() != WL_CONNECTED) {
    return;
  }

  HTTPClient http;
  if (!http.begin(alertWebhookUrl)) {
    return;
  }

  http.addHeader("Content-Type", "application/json");
  String body = notificationAsJson(item);
  http.POST(body);
  http.end();
}

void addEvent(const String& cameraId, const String& type, const String& details) {
  EventRecord event = {nextEventId++, cameraId, type, details, millis()};
  events.push_back(event);
  trimQueues();
}

void addNotification(const String& cameraId, const String& title,
                     const String& message, const String& priority) {
  NotificationRecord item = {nextNotificationId++, cameraId, title,
                             message, priority, false, millis()};
  notifications.push_back(item);
  trimQueues();
  sendWebhookNotification(item);
}

void handleGetCameras() {
  refreshHealth();
  String payload = "[";
  for (size_t i = 0; i < cameras.size(); i++) {
    payload += cameraAsJson(cameras[i]);
    if (i + 1 < cameras.size()) {
      payload += ",";
    }
  }
  payload += "]";
  server.send(200, "application/json", payload);
}

void handleUpsertCamera() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id is required\"}");
    return;
  }

  const String id = server.arg("id");
  CameraRecord& camera = upsertCamera(id);

  if (server.hasArg("name")) camera.name = server.arg("name");
  if (server.hasArg("group")) camera.groupName = server.arg("group");
  if (server.hasArg("location")) camera.location = server.arg("location");
  if (server.hasArg("stream")) camera.streamUrl = server.arg("stream");

  server.send(200, "application/json", cameraAsJson(camera));
}

void handleHeartbeat() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id is required\"}");
    return;
  }

  CameraRecord& camera = upsertCamera(server.arg("id"));
  camera.lastSeenMs = millis();
  camera.online = true;

  if (server.hasArg("battery")) camera.battery = server.arg("battery").toInt();
  if (server.hasArg("rssi")) camera.rssi = server.arg("rssi").toInt();

  server.send(200, "application/json", cameraAsJson(camera));
}

void handleDetection() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id is required\"}");
    return;
  }

  CameraRecord& camera = upsertCamera(server.arg("id"));
  const bool motion = server.hasArg("motion") && parseBoolArg(server.arg("motion"));
  const bool human = server.hasArg("human") && parseBoolArg(server.arg("human"));
  const bool face = server.hasArg("face") && parseBoolArg(server.arg("face"));
  const String faceName = server.hasArg("faceName") ? server.arg("faceName") : "";

  camera.motionDetected = motion;
  camera.humanDetected = human;
  camera.faceDetected = face;
  camera.lastFaceName = faceName;
  camera.lastDetectionMs = millis();

  if (motion) {
    addEvent(camera.id, "motion", "Motion detected");
    addNotification(camera.id, "Motion Alert",
                    camera.name + " detected motion", "low");
  }
  if (human) {
    addEvent(camera.id, "human", "Human detected");
    addNotification(camera.id, "Human Alert",
                    camera.name + " detected a person", "medium");
  }
  if (face) {
    const bool knownFace = faceName.length() > 0 && !faceName.equalsIgnoreCase("unknown");
    addEvent(camera.id, "face", knownFace ? ("Face detected: " + faceName)
                                            : "Face detected: unknown");
    addNotification(
        camera.id, knownFace ? "Face Detected" : "Unknown Face Alert",
        knownFace ? (camera.name + " recognized " + faceName)
                  : (camera.name + " detected an unknown face"),
        knownFace ? "medium" : "high");
  }

  server.send(200, "application/json", cameraAsJson(camera));
}

void handleGetEvents() {
  size_t limit = 25;
  if (server.hasArg("limit")) {
    const int parsed = server.arg("limit").toInt();
    if (parsed > 0) {
      limit = static_cast<size_t>(parsed);
    }
  }

  if (limit > events.size()) {
    limit = events.size();
  }

  String payload = "[";
  const size_t start = events.size() - limit;
  for (size_t i = start; i < events.size(); i++) {
    payload += eventAsJson(events[i]);
    if (i + 1 < events.size()) {
      payload += ",";
    }
  }
  payload += "]";
  server.send(200, "application/json", payload);
}

void handleGetNotifications() {
  const bool unackedOnly = server.hasArg("unacked") && parseBoolArg(server.arg("unacked"));
  String payload = "[";
  bool first = true;
  for (const auto& item : notifications) {
    if (unackedOnly && item.acknowledged) {
      continue;
    }
    if (!first) {
      payload += ",";
    }
    payload += notificationAsJson(item);
    first = false;
  }
  payload += "]";
  server.send(200, "application/json", payload);
}

void handleAckNotification() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id is required\"}");
    return;
  }

  const uint32_t id = static_cast<uint32_t>(server.arg("id").toInt());
  for (auto& item : notifications) {
    if (item.id == id) {
      item.acknowledged = true;
      server.send(200, "application/json", notificationAsJson(item));
      return;
    }
  }

  server.send(404, "application/json", "{\"error\":\"notification not found\"}");
}

void handleAlertConfig() {
  if (server.hasArg("webhook")) {
    alertWebhookUrl = server.arg("webhook");
  }

  String payload = "{";
  payload += "\"webhook\":\"" + jsonEscape(alertWebhookUrl) + "\"";
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleGetStream() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id is required\"}");
    return;
  }

  refreshHealth();
  CameraRecord* camera = findCameraById(server.arg("id"));
  if (camera == nullptr) {
    server.send(404, "application/json", "{\"error\":\"camera not found\"}");
    return;
  }

  String payload = "{";
  payload += "\"id\":\"" + jsonEscape(camera->id) + "\",";
  payload += "\"name\":\"" + jsonEscape(camera->name) + "\",";
  payload += "\"streamUrl\":\"" + jsonEscape(camera->streamUrl) + "\"";
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleDashboard() {
  const char* html = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP-CAM Core Platform</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; background: #111; color: #fff; }
    header { padding: 12px 16px; background: #1d1d1d; font-weight: bold; }
    #layout { display: grid; grid-template-columns: 2fr 1fr; gap: 8px; }
    #grid { display: grid; grid-template-columns: repeat(auto-fit,minmax(260px,1fr)); gap: 12px; padding: 12px; }
    .card { background: #1e1e1e; border-radius: 10px; overflow: hidden; border: 1px solid #333; cursor: pointer; }
    .meta { padding: 8px 10px; font-size: 13px; line-height: 1.4; }
    .chip { display: inline-block; padding: 2px 8px; margin-right: 6px; border-radius: 12px; font-size: 11px; border: 1px solid #444; }
    .status-online { color: #52d273; }
    .status-offline { color: #ff6b6b; }
    .detected { border-color: #3f8cff; color: #8ab6ff; }
    img { width: 100%; display: block; background: #000; min-height: 170px; object-fit: cover; }
    #alerts { padding: 12px 12px 12px 0; max-height: 92vh; overflow: auto; }
    .alert-item { background: #1d1d1d; border: 1px solid #323232; border-left-width: 4px; border-radius: 8px; margin-bottom: 8px; padding: 8px; font-size: 12px; }
    .p-low { border-left-color: #6f8cff; }
    .p-medium { border-left-color: #ffb347; }
    .p-high { border-left-color: #ff6b6b; }
    #fullscreen { position: fixed; inset: 0; display: none; background: rgba(0,0,0,0.96); align-items: center; justify-content: center; flex-direction: column; }
    #fullscreen img { max-width: 96vw; max-height: 86vh; }
    #closeBtn { margin-top: 12px; padding: 10px 14px; background: #2f2f2f; border: 1px solid #555; color: #fff; border-radius: 8px; }
    @media (max-width: 980px) {
      #layout { grid-template-columns: 1fr; }
      #alerts { padding: 0 12px 12px; }
    }
  </style>
</head>
<body>
  <header>ESP-CAM Live View + Detection Alerts</header>
  <div id="layout">
    <main id="grid"></main>
    <aside id="alerts">
      <h3>Recent Alerts</h3>
      <div id="alertList"></div>
    </aside>
  </div>
  <div id="fullscreen" onclick="closeFullscreen()">
    <img id="fullscreenImage" src="" alt="fullscreen stream">
    <button id="closeBtn" type="button">Close</button>
  </div>
  <script>
    let lastNotificationId = 0;

    function escapeHtml(value) {
      return String(value || '')
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#39;');
    }

    async function loadCameras() {
      const res = await fetch('/api/cameras');
      const cameras = await res.json();
      const grid = document.getElementById('grid');
      grid.innerHTML = '';
      cameras.forEach((cam) => {
        const card = document.createElement('article');
        card.className = 'card';
        const chips = [
          cam.motionDetected ? '<span class="chip detected">Motion</span>' : '',
          cam.humanDetected ? '<span class="chip detected">Human</span>' : '',
          cam.faceDetected ? `<span class="chip detected">Face${cam.lastFaceName ? ': ' + escapeHtml(cam.lastFaceName) : ''}</span>` : ''
        ].join('');
        card.innerHTML = `
          <img src="${escapeHtml(cam.streamUrl || '')}" alt="${escapeHtml(cam.name)}" />
          <div class="meta">
            <div><strong>${escapeHtml(cam.name)}</strong> (${escapeHtml(cam.id)})</div>
            <div>Group: ${escapeHtml(cam.group)}</div>
            <div>Location: ${escapeHtml(cam.location)}</div>
            <div>Status: <span class="${cam.online ? 'status-online':'status-offline'}">${cam.online ? 'Online':'Offline'}</span></div>
            <div>Battery: ${cam.battery}% | RSSI: ${cam.rssi}</div>
            <div>${chips || '<span class="chip">No active detections</span>'}</div>
          </div>`;
        card.addEventListener('click', () => openFullscreen(cam.streamUrl));
        grid.appendChild(card);
      });
    }

    async function loadAlerts() {
      const res = await fetch('/api/notifications?unacked=true');
      const alerts = await res.json();
      const list = document.getElementById('alertList');
      list.innerHTML = '';
      alerts.slice().reverse().forEach((item) => {
        const div = document.createElement('div');
        const priority = ['low', 'medium', 'high'].includes(item.priority) ? item.priority : 'low';
        div.className = `alert-item p-${priority}`;
        div.innerHTML = `
          <div><strong>${escapeHtml(item.title)}</strong></div>
          <div>${escapeHtml(item.message)}</div>
          <div style="margin-top:6px"><button onclick="ack(${item.id})">Acknowledge</button></div>`;
        list.appendChild(div);

        if (item.id > lastNotificationId) {
          if (window.Notification && Notification.permission === 'granted') {
            new Notification(item.title, { body: item.message });
          }
          lastNotificationId = item.id;
        }
      });
    }

    async function ack(id) {
      await fetch('/api/notifications/ack?id=' + id, { method: 'POST' });
      await loadAlerts();
    }

    function openFullscreen(streamUrl) {
      if (!streamUrl) return;
      document.getElementById('fullscreenImage').src = streamUrl;
      document.getElementById('fullscreen').style.display = 'flex';
    }

    function closeFullscreen() {
      document.getElementById('fullscreen').style.display = 'none';
    }

    if (window.Notification && Notification.permission === 'default') {
      Notification.requestPermission();
    }

    loadCameras();
    loadAlerts();
    setInterval(loadCameras, 5000);
    setInterval(loadAlerts, 4000);
  </script>
</body>
</html>)rawliteral";

  server.send(200, "text/html", html);
}

void seedSampleData() {
  CameraRecord& entry = upsertCamera("cam-front-door");
  entry.name = "Front Door";
  entry.groupName = "outdoor";
  entry.location = "Entrance";
  entry.streamUrl = "http://192.168.1.101:81/stream";
  entry.battery = 92;
  entry.rssi = -63;
  entry.lastSeenMs = millis();
  entry.online = true;

  addEvent(entry.id, "boot", "Camera profile seeded");
}

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  seedSampleData();

  server.on("/", HTTP_GET, handleDashboard);
  server.on("/api/cameras", HTTP_GET, handleGetCameras);
  server.on("/api/cameras/upsert", HTTP_POST, handleUpsertCamera);
  server.on("/api/heartbeat", HTTP_POST, handleHeartbeat);
  server.on("/api/detections", HTTP_POST, handleDetection);
  server.on("/api/events", HTTP_GET, handleGetEvents);
  server.on("/api/notifications", HTTP_GET, handleGetNotifications);
  server.on("/api/notifications/ack", HTTP_POST, handleAckNotification);
  server.on("/api/alerts/config", HTTP_POST, handleAlertConfig);
  server.on("/api/camera/stream", HTTP_GET, handleGetStream);

  server.begin();

  Serial.println("Core platform server started");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();
}
