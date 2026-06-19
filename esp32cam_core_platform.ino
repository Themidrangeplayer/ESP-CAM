#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <vector>

const char* WIFI_SSID = "REPLACE_WITH_SSID";
const char* WIFI_PASSWORD = "REPLACE_WITH_PASSWORD";
constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 30000;

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
};

std::vector<CameraRecord> cameras;

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

  CameraRecord created = {
      id, id, "default", "unknown", "", false, -1, -999, 0};
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
  out += "\"lastSeenMs\":" + String(camera.lastSeenMs);
  out += "}";
  return out;
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
    #grid { display: grid; grid-template-columns: repeat(auto-fit,minmax(260px,1fr)); gap: 12px; padding: 12px; }
    .card { background: #1e1e1e; border-radius: 10px; overflow: hidden; border: 1px solid #333; cursor: pointer; }
    .meta { padding: 8px 10px; font-size: 13px; line-height: 1.4; }
    .status-online { color: #52d273; }
    .status-offline { color: #ff6b6b; }
    img { width: 100%; display: block; background: #000; min-height: 170px; object-fit: cover; }
    #fullscreen { position: fixed; inset: 0; display: none; background: rgba(0,0,0,0.96); align-items: center; justify-content: center; flex-direction: column; }
    #fullscreen img { max-width: 96vw; max-height: 86vh; }
    #closeBtn { margin-top: 12px; padding: 10px 14px; background: #2f2f2f; border: 1px solid #555; color: #fff; border-radius: 8px; }
  </style>
</head>
<body>
  <header>ESP-CAM Live View</header>
  <main id="grid"></main>
  <div id="fullscreen" onclick="closeFullscreen()">
    <img id="fullscreenImage" src="" alt="fullscreen stream">
    <button id="closeBtn" type="button">Close</button>
  </div>
  <script>
    async function loadCameras() {
      const res = await fetch('/api/cameras');
      const cameras = await res.json();
      const grid = document.getElementById('grid');
      grid.innerHTML = '';
      cameras.forEach((cam) => {
        const card = document.createElement('article');
        card.className = 'card';
        card.innerHTML = `
          <img src="${cam.streamUrl || ''}" alt="${cam.name}" />
          <div class="meta">
            <div><strong>${cam.name}</strong> (${cam.id})</div>
            <div>Group: ${cam.group}</div>
            <div>Location: ${cam.location}</div>
            <div>Status: <span class="${cam.online ? 'status-online':'status-offline'}">${cam.online ? 'Online':'Offline'}</span></div>
            <div>Battery: ${cam.battery}% | RSSI: ${cam.rssi}</div>
          </div>`;
        card.addEventListener('click', () => openFullscreen(cam.streamUrl));
        grid.appendChild(card);
      });
    }

    function openFullscreen(streamUrl) {
      if (!streamUrl) return;
      document.getElementById('fullscreenImage').src = streamUrl;
      document.getElementById('fullscreen').style.display = 'flex';
    }

    function closeFullscreen() {
      document.getElementById('fullscreen').style.display = 'none';
    }

    loadCameras();
    setInterval(loadCameras, 5000);
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
  server.on("/api/camera/stream", HTTP_GET, handleGetStream);

  server.begin();

  Serial.println("Core platform server started");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();
}
