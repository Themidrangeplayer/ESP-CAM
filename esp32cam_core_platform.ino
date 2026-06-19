#include <Arduino.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <vector>

const char* WIFI_SSID = "REPLACE_WITH_SSID";
const char* WIFI_PASSWORD = "REPLACE_WITH_PASSWORD";
constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 30000;
constexpr size_t MAX_EVENTS = 180;
constexpr size_t MAX_NOTIFICATIONS = 160;
constexpr size_t MAX_SNAPSHOTS = 200;
constexpr size_t MAX_RECORDINGS = 160;
constexpr size_t MAX_TIMELINE = 400;

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

struct SnapshotRecord {
  uint32_t id;
  String cameraId;
  String reason;
  String imageUrl;
  uint32_t timestampMs;
  uint32_t sizeKb;
  bool storedOnSd;
  bool backedUpToCloud;
};

struct RecordingRecord {
  uint32_t id;
  String cameraId;
  String mode;
  String status;
  String trigger;
  String sourceUrl;
  uint32_t startMs;
  uint32_t endMs;
  uint32_t sizeKb;
  bool storedOnSd;
  bool backedUpToCloud;
};

struct TimelineRecord {
  uint32_t id;
  String cameraId;
  String itemType;
  String summary;
  uint32_t timestampMs;
  uint32_t referenceId;
};

struct StorageState {
  bool sdEnabled;
  uint32_t sdCapacityMb;
  uint32_t sdUsedMb;
  bool cloudEnabled;
  String cloudEndpoint;
  uint32_t lastSyncMs;
};

std::vector<CameraRecord> cameras;
std::vector<EventRecord> events;
std::vector<NotificationRecord> notifications;
std::vector<SnapshotRecord> snapshots;
std::vector<RecordingRecord> recordings;
std::vector<TimelineRecord> timeline;

StorageState storageState = {true, 2048, 0, false, "", 0};
String alertWebhookUrl = "";
bool autoEventRecordingEnabled = true;

uint32_t nextEventId = 1;
uint32_t nextNotificationId = 1;
uint32_t nextSnapshotId = 1;
uint32_t nextRecordingId = 1;
uint32_t nextTimelineId = 1;

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
  return value == "1" || value == "true" || value == "TRUE" || value == "yes" ||
         value == "on";
}

String toLowerCopy(const String& input) {
  String out = input;
  out.toLowerCase();
  return out;
}

bool isValidHttpUrl(const String& value) {
  return value.startsWith("http://") || value.startsWith("https://");
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

RecordingRecord* findActiveRecordingByCamera(const String& cameraId) {
  for (auto& rec : recordings) {
    if (rec.cameraId == cameraId && rec.status == "recording") {
      return &rec;
    }
  }
  return nullptr;
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
  while (events.size() > MAX_EVENTS) events.erase(events.begin());
  while (notifications.size() > MAX_NOTIFICATIONS) notifications.erase(notifications.begin());
  while (snapshots.size() > MAX_SNAPSHOTS) snapshots.erase(snapshots.begin());
  while (recordings.size() > MAX_RECORDINGS) recordings.erase(recordings.begin());
  while (timeline.size() > MAX_TIMELINE) timeline.erase(timeline.begin());
}

void consumeSdStorageKb(uint32_t sizeKb) {
  if (!storageState.sdEnabled) return;

  const uint32_t sizeMbRounded = (sizeKb + 1023) / 1024;
  if (storageState.sdUsedMb + sizeMbRounded > storageState.sdCapacityMb) {
    storageState.sdUsedMb = storageState.sdCapacityMb;
    return;
  }
  storageState.sdUsedMb += sizeMbRounded;
}

String storageUsageJson() {
  String out = "{";
  out += "\"sdEnabled\":" + String(storageState.sdEnabled ? "true" : "false") + ",";
  out += "\"sdCapacityMb\":" + String(storageState.sdCapacityMb) + ",";
  out += "\"sdUsedMb\":" + String(storageState.sdUsedMb) + ",";
  out += "\"sdFreeMb\":" +
         String(storageState.sdCapacityMb > storageState.sdUsedMb
                    ? (storageState.sdCapacityMb - storageState.sdUsedMb)
                    : 0) +
         ",";
  out += "\"cloudEnabled\":" + String(storageState.cloudEnabled ? "true" : "false") + ",";
  out += "\"cloudEndpoint\":\"" + jsonEscape(storageState.cloudEndpoint) + "\",";
  out += "\"lastSyncMs\":" + String(storageState.lastSyncMs);
  out += "}";
  return out;
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

String snapshotAsJson(const SnapshotRecord& item) {
  String out = "{";
  out += "\"id\":" + String(item.id) + ",";
  out += "\"cameraId\":\"" + jsonEscape(item.cameraId) + "\",";
  out += "\"reason\":\"" + jsonEscape(item.reason) + "\",";
  out += "\"imageUrl\":\"" + jsonEscape(item.imageUrl) + "\",";
  out += "\"timestampMs\":" + String(item.timestampMs) + ",";
  out += "\"sizeKb\":" + String(item.sizeKb) + ",";
  out += "\"storedOnSd\":" + String(item.storedOnSd ? "true" : "false") + ",";
  out += "\"backedUpToCloud\":" + String(item.backedUpToCloud ? "true" : "false");
  out += "}";
  return out;
}

String recordingAsJson(const RecordingRecord& item) {
  String out = "{";
  out += "\"id\":" + String(item.id) + ",";
  out += "\"cameraId\":\"" + jsonEscape(item.cameraId) + "\",";
  out += "\"mode\":\"" + jsonEscape(item.mode) + "\",";
  out += "\"status\":\"" + jsonEscape(item.status) + "\",";
  out += "\"trigger\":\"" + jsonEscape(item.trigger) + "\",";
  out += "\"sourceUrl\":\"" + jsonEscape(item.sourceUrl) + "\",";
  out += "\"startMs\":" + String(item.startMs) + ",";
  out += "\"endMs\":" + String(item.endMs) + ",";
  out += "\"sizeKb\":" + String(item.sizeKb) + ",";
  out += "\"storedOnSd\":" + String(item.storedOnSd ? "true" : "false") + ",";
  out += "\"backedUpToCloud\":" + String(item.backedUpToCloud ? "true" : "false");
  out += "}";
  return out;
}

String timelineAsJson(const TimelineRecord& item) {
  String out = "{";
  out += "\"id\":" + String(item.id) + ",";
  out += "\"cameraId\":\"" + jsonEscape(item.cameraId) + "\",";
  out += "\"itemType\":\"" + jsonEscape(item.itemType) + "\",";
  out += "\"summary\":\"" + jsonEscape(item.summary) + "\",";
  out += "\"timestampMs\":" + String(item.timestampMs) + ",";
  out += "\"referenceId\":" + String(item.referenceId);
  out += "}";
  return out;
}

void sendWebhookNotification(const NotificationRecord& item) {
  if (alertWebhookUrl.length() == 0 || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  if (!http.begin(alertWebhookUrl)) return;

  http.addHeader("Content-Type", "application/json");
  http.POST(notificationAsJson(item));
  http.end();
}

bool sendCloudBackup(const String& payload) {
  if (!storageState.cloudEnabled || storageState.cloudEndpoint.length() == 0 ||
      WiFi.status() != WL_CONNECTED) {
    return false;
  }

  HTTPClient http;
  if (!http.begin(storageState.cloudEndpoint)) {
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(payload);
  http.end();
  if (code > 0 && code < 400) {
    storageState.lastSyncMs = millis();
    return true;
  }
  return false;
}

void addTimeline(const String& cameraId, const String& itemType, const String& summary,
                 uint32_t referenceId) {
  timeline.push_back({nextTimelineId++, cameraId, itemType, summary, millis(), referenceId});
  trimQueues();
}

void addEvent(const String& cameraId, const String& type, const String& details) {
  EventRecord event = {nextEventId++, cameraId, type, details, millis()};
  events.push_back(event);
  addTimeline(cameraId, "event", type + ": " + details, event.id);
  trimQueues();
}

void addNotification(const String& cameraId, const String& title, const String& message,
                     const String& priority) {
  NotificationRecord item = {nextNotificationId++, cameraId, title, message, priority, false,
                             millis()};
  notifications.push_back(item);
  addTimeline(cameraId, "notification", title + " - " + message, item.id);
  trimQueues();
  sendWebhookNotification(item);
}

SnapshotRecord createSnapshot(const String& cameraId, const String& reason, const String& imageUrl,
                              uint32_t estimatedSizeKb) {
  SnapshotRecord snap = {nextSnapshotId++,
                         cameraId,
                         reason,
                         imageUrl,
                         millis(),
                         estimatedSizeKb,
                         storageState.sdEnabled,
                         false};

  if (snap.storedOnSd) {
    consumeSdStorageKb(estimatedSizeKb);
  }
  if (storageState.cloudEnabled) {
    snap.backedUpToCloud =
        sendCloudBackup("{\"type\":\"snapshot\",\"payload\":" + snapshotAsJson(snap) + "}");
  }

  snapshots.push_back(snap);
  addTimeline(cameraId, "snapshot", "Snapshot captured: " + reason, snap.id);
  trimQueues();
  return snap;
}

RecordingRecord& startRecording(const String& cameraId, const String& mode, const String& trigger,
                                const String& sourceUrl) {
  RecordingRecord created = {nextRecordingId++,
                             cameraId,
                             mode,
                             "recording",
                             trigger,
                             sourceUrl,
                             millis(),
                             0,
                             0,
                             storageState.sdEnabled,
                             false};
  recordings.push_back(created);
  RecordingRecord& rec = recordings.back();
  addTimeline(cameraId, "recording", "Recording started (" + mode + ")", rec.id);
  trimQueues();
  return rec;
}

bool stopRecording(RecordingRecord& rec, uint32_t sizeKb) {
  if (rec.status != "recording") {
    return false;
  }

  rec.status = "completed";
  rec.endMs = millis();
  rec.sizeKb = sizeKb;
  if (rec.storedOnSd) {
    consumeSdStorageKb(sizeKb);
  }
  if (storageState.cloudEnabled) {
    rec.backedUpToCloud =
        sendCloudBackup("{\"type\":\"recording\",\"payload\":" + recordingAsJson(rec) + "}");
  }

  addTimeline(rec.cameraId, "recording", "Recording completed (" + rec.mode + ")", rec.id);
  trimQueues();
  return true;
}

void maybeCreateEventRecording(const String& cameraId, const String& trigger, const String& sourceUrl) {
  if (!autoEventRecordingEnabled) return;
  if (findActiveRecordingByCamera(cameraId) != nullptr) return;

  RecordingRecord& rec = startRecording(cameraId, "event", trigger, sourceUrl);
  stopRecording(rec, 1024);
}

void handleGetCameras() {
  refreshHealth();
  String payload = "[";
  for (size_t i = 0; i < cameras.size(); i++) {
    payload += cameraAsJson(cameras[i]);
    if (i + 1 < cameras.size()) payload += ",";
  }
  payload += "]";
  server.send(200, "application/json", payload);
}

void handleUpsertCamera() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id is required\"}");
    return;
  }

  CameraRecord& camera = upsertCamera(server.arg("id"));
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
    addNotification(camera.id, "Motion Alert", camera.name + " detected motion", "low");
    createSnapshot(camera.id, "motion", camera.streamUrl, 256);
    maybeCreateEventRecording(camera.id, "motion", camera.streamUrl);
  }
  if (human) {
    addEvent(camera.id, "human", "Human detected");
    addNotification(camera.id, "Human Alert", camera.name + " detected a person", "medium");
    createSnapshot(camera.id, "human", camera.streamUrl, 280);
    maybeCreateEventRecording(camera.id, "human", camera.streamUrl);
  }
  if (face) {
    const bool knownFace = faceName.length() > 0 && !faceName.equalsIgnoreCase("unknown");
    addEvent(camera.id, "face", knownFace ? ("Face detected: " + faceName)
                                            : "Face detected: unknown");
    addNotification(camera.id, knownFace ? "Face Detected" : "Unknown Face Alert",
                    knownFace ? (camera.name + " recognized " + faceName)
                              : (camera.name + " detected an unknown face"),
                    knownFace ? "medium" : "high");
    createSnapshot(camera.id, knownFace ? ("face:" + faceName) : "face:unknown", camera.streamUrl,
                   300);
    maybeCreateEventRecording(camera.id, "face", camera.streamUrl);
  }

  server.send(200, "application/json", cameraAsJson(camera));
}

void handleCaptureSnapshot() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id is required\"}");
    return;
  }

  CameraRecord& camera = upsertCamera(server.arg("id"));
  const String reason = server.hasArg("reason") ? server.arg("reason") : "manual";
  const String imageUrl = server.hasArg("imageUrl") ? server.arg("imageUrl") : camera.streamUrl;
  const uint32_t sizeKb = server.hasArg("sizeKb") ? (uint32_t)server.arg("sizeKb").toInt() : 300;

  SnapshotRecord snap = createSnapshot(camera.id, reason, imageUrl, sizeKb);
  addEvent(camera.id, "snapshot", "Snapshot captured: " + reason);

  server.send(200, "application/json", snapshotAsJson(snap));
}

void handleGetSnapshots() {
  const String cameraId = server.hasArg("cameraId") ? server.arg("cameraId") : "";
  size_t limit = 30;
  if (server.hasArg("limit")) {
    const int parsed = server.arg("limit").toInt();
    if (parsed > 0) limit = (size_t)parsed;
  }

  String payload = "[";
  size_t returned = 0;
  for (size_t i = snapshots.size(); i > 0; i--) {
    const auto& item = snapshots[i - 1];
    if (cameraId.length() > 0 && item.cameraId != cameraId) continue;
    if (returned > 0) payload += ",";
    payload += snapshotAsJson(item);
    returned++;
    if (returned >= limit) break;
  }
  payload += "]";
  server.send(200, "application/json", payload);
}

void handleStartRecording() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id is required\"}");
    return;
  }

  CameraRecord& camera = upsertCamera(server.arg("id"));
  if (findActiveRecordingByCamera(camera.id) != nullptr) {
    server.send(409, "application/json", "{\"error\":\"recording already active for camera\"}");
    return;
  }

  String mode = server.hasArg("mode") ? toLowerCopy(server.arg("mode")) : "continuous";
  if (!(mode == "continuous" || mode == "event")) {
    server.send(400, "application/json", "{\"error\":\"mode must be event or continuous\"}");
    return;
  }

  const String trigger = server.hasArg("trigger") ? server.arg("trigger") : "manual";
  const String sourceUrl = server.hasArg("source") ? server.arg("source") : camera.streamUrl;

  RecordingRecord& rec = startRecording(camera.id, mode, trigger, sourceUrl);
  addEvent(camera.id, "recording", "Recording started (" + mode + ")");
  server.send(200, "application/json", recordingAsJson(rec));
}

void handleStopRecording() {
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"id is required\"}");
    return;
  }

  const String cameraId = server.arg("id");
  RecordingRecord* rec = findActiveRecordingByCamera(cameraId);
  if (rec == nullptr) {
    server.send(404, "application/json", "{\"error\":\"no active recording for camera\"}");
    return;
  }

  const uint32_t sizeKb = server.hasArg("sizeKb") ? (uint32_t)server.arg("sizeKb").toInt() : 4096;
  stopRecording(*rec, sizeKb);
  addEvent(cameraId, "recording", "Recording stopped");
  server.send(200, "application/json", recordingAsJson(*rec));
}

void handleGetRecordings() {
  const String cameraId = server.hasArg("cameraId") ? server.arg("cameraId") : "";
  const String modeFilter = server.hasArg("mode") ? toLowerCopy(server.arg("mode")) : "";
  const String statusFilter = server.hasArg("status") ? toLowerCopy(server.arg("status")) : "";

  String payload = "[";
  bool first = true;
  for (const auto& item : recordings) {
    if (cameraId.length() > 0 && item.cameraId != cameraId) continue;
    if (modeFilter.length() > 0 && toLowerCopy(item.mode) != modeFilter) continue;
    if (statusFilter.length() > 0 && toLowerCopy(item.status) != statusFilter) continue;
    if (!first) payload += ",";
    payload += recordingAsJson(item);
    first = false;
  }
  payload += "]";
  server.send(200, "application/json", payload);
}

void handleSyncAssetToCloud() {
  if (!server.hasArg("assetType") || !server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"assetType and id are required\"}");
    return;
  }

  if (!storageState.cloudEnabled || storageState.cloudEndpoint.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"cloud backup is not configured\"}");
    return;
  }

  const String assetType = toLowerCopy(server.arg("assetType"));
  const uint32_t id = (uint32_t)server.arg("id").toInt();

  if (assetType == "snapshot") {
    for (auto& item : snapshots) {
      if (item.id != id) continue;
      item.backedUpToCloud =
          sendCloudBackup("{\"type\":\"snapshot\",\"payload\":" + snapshotAsJson(item) + "}");
      server.send(200, "application/json", snapshotAsJson(item));
      return;
    }
  }

  if (assetType == "recording") {
    for (auto& item : recordings) {
      if (item.id != id) continue;
      item.backedUpToCloud = sendCloudBackup("{\"type\":\"recording\",\"payload\":" +
                                              recordingAsJson(item) + "}");
      server.send(200, "application/json", recordingAsJson(item));
      return;
    }
  }

  server.send(404, "application/json", "{\"error\":\"asset not found\"}");
}

void handleGetTimeline() {
  const String cameraId = server.hasArg("cameraId") ? server.arg("cameraId") : "";
  const String type = server.hasArg("type") ? toLowerCopy(server.arg("type")) : "";
  const String query = server.hasArg("q") ? toLowerCopy(server.arg("q")) : "";
  size_t limit = 50;
  if (server.hasArg("limit")) {
    const int parsed = server.arg("limit").toInt();
    if (parsed > 0) limit = (size_t)parsed;
  }

  String payload = "[";
  size_t returned = 0;
  for (size_t i = timeline.size(); i > 0; i--) {
    const TimelineRecord& item = timeline[i - 1];
    if (cameraId.length() > 0 && item.cameraId != cameraId) continue;
    if (type.length() > 0 && toLowerCopy(item.itemType) != type) continue;
    if (query.length() > 0 && toLowerCopy(item.summary).indexOf(query) < 0) continue;
    if (returned > 0) payload += ",";
    payload += timelineAsJson(item);
    returned++;
    if (returned >= limit) break;
  }
  payload += "]";
  server.send(200, "application/json", payload);
}

void handleGetEvents() {
  size_t limit = 25;
  if (server.hasArg("limit")) {
    const int parsed = server.arg("limit").toInt();
    if (parsed > 0) limit = (size_t)parsed;
  }

  if (limit > events.size()) limit = events.size();
  String payload = "[";
  const size_t start = events.size() - limit;
  for (size_t i = start; i < events.size(); i++) {
    payload += eventAsJson(events[i]);
    if (i + 1 < events.size()) payload += ",";
  }
  payload += "]";
  server.send(200, "application/json", payload);
}

void handleGetNotifications() {
  const bool unackedOnly = server.hasArg("unacked") && parseBoolArg(server.arg("unacked"));
  String payload = "[";
  bool first = true;
  for (const auto& item : notifications) {
    if (unackedOnly && item.acknowledged) continue;
    if (!first) payload += ",";
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

  const uint32_t id = (uint32_t)server.arg("id").toInt();
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
    const String candidate = server.arg("webhook");
    if (candidate.length() > 0 && !isValidHttpUrl(candidate)) {
      server.send(400, "application/json", "{\"error\":\"webhook must start with http:// or https://\"}");
      return;
    }
    alertWebhookUrl = candidate;
  }

  String payload = "{";
  payload += "\"webhook\":\"" + jsonEscape(alertWebhookUrl) + "\"";
  payload += "}";
  server.send(200, "application/json", payload);
}

void handleStorageConfig() {
  if (server.hasArg("sdEnabled")) storageState.sdEnabled = parseBoolArg(server.arg("sdEnabled"));
  if (server.hasArg("sdCapacityMb")) {
    const int val = server.arg("sdCapacityMb").toInt();
    if (val > 0) storageState.sdCapacityMb = (uint32_t)val;
    if (storageState.sdUsedMb > storageState.sdCapacityMb) {
      storageState.sdUsedMb = storageState.sdCapacityMb;
    }
  }
  if (server.hasArg("cloudEnabled")) {
    storageState.cloudEnabled = parseBoolArg(server.arg("cloudEnabled"));
  }
  if (server.hasArg("cloudEndpoint")) {
    const String candidate = server.arg("cloudEndpoint");
    if (candidate.length() > 0 && !isValidHttpUrl(candidate)) {
      server.send(400, "application/json", "{\"error\":\"cloudEndpoint must start with http:// or https://\"}");
      return;
    }
    storageState.cloudEndpoint = candidate;
  }
  if (server.hasArg("autoEventRecording")) {
    autoEventRecordingEnabled = parseBoolArg(server.arg("autoEventRecording"));
  }

  server.send(200, "application/json",
              "{\"storage\":" + storageUsageJson() +
                  ",\"autoEventRecording\":" +
                  String(autoEventRecordingEnabled ? "true" : "false") + "}");
}

void handleGetStorage() {
  String payload = "{";
  payload += "\"storage\":" + storageUsageJson() + ",";
  payload += "\"snapshotCount\":" + String(snapshots.size()) + ",";
  payload += "\"recordingCount\":" + String(recordings.size()) + ",";
  payload += "\"autoEventRecording\":" +
             String(autoEventRecordingEnabled ? "true" : "false");
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
  <title>ESP-CAM Platform</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; background: #111; color: #fff; }
    header { padding: 12px 16px; background: #1d1d1d; font-weight: bold; }
    #layout { display: grid; grid-template-columns: 2fr 1fr; gap: 8px; }
    #grid { display: grid; grid-template-columns: repeat(auto-fit,minmax(260px,1fr)); gap: 12px; padding: 12px; }
    .card { background: #1e1e1e; border-radius: 10px; overflow: hidden; border: 1px solid #333; cursor: pointer; }
    .meta { padding: 8px 10px; font-size: 13px; line-height: 1.4; }
    .chip { display: inline-block; padding: 2px 8px; margin-right: 6px; border-radius: 12px; font-size: 11px; border: 1px solid #444; }
    .detected { border-color: #3f8cff; color: #8ab6ff; }
    .status-online { color: #52d273; }
    .status-offline { color: #ff6b6b; }
    img { width: 100%; display: block; background: #000; min-height: 170px; object-fit: cover; }
    #panel { padding: 12px 12px 12px 0; max-height: 92vh; overflow: auto; }
    .box { background: #1d1d1d; border: 1px solid #323232; border-radius: 8px; margin-bottom: 10px; padding: 10px; }
    .alert-item { border-left-width: 4px; border-left-style: solid; margin-bottom: 8px; padding-left: 8px; font-size: 12px; }
    .p-low { border-left-color: #6f8cff; }
    .p-medium { border-left-color: #ffb347; }
    .p-high { border-left-color: #ff6b6b; }
    #fullscreen { position: fixed; inset: 0; display: none; background: rgba(0,0,0,0.96); align-items: center; justify-content: center; flex-direction: column; }
    #fullscreen img { max-width: 96vw; max-height: 86vh; }
    #closeBtn { margin-top: 12px; padding: 10px 14px; background: #2f2f2f; border: 1px solid #555; color: #fff; border-radius: 8px; }
    @media (max-width: 980px) { #layout { grid-template-columns: 1fr; } #panel { padding: 0 12px 12px; } }
  </style>
</head>
<body>
  <header>ESP-CAM Live View + Detection + Recording</header>
  <div id="layout">
    <main id="grid"></main>
    <aside id="panel">
      <div class="box">
        <h3>Storage</h3>
        <div id="storageInfo">Loading...</div>
      </div>
      <div class="box">
        <h3>Recent Alerts</h3>
        <div id="alertList"></div>
      </div>
      <div class="box">
        <h3>Timeline</h3>
        <div id="timelineList"></div>
      </div>
    </aside>
  </div>
  <div id="fullscreen" onclick="closeFullscreen()">
    <img id="fullscreenImage" src="" alt="fullscreen stream">
    <button id="closeBtn" type="button">Close</button>
  </div>
  <script>
    let lastNotificationId = 0;

    function escapeHtml(value) {
      return String(value || '').replaceAll('&', '&amp;').replaceAll('<', '&lt;').replaceAll('>', '&gt;').replaceAll('"', '&quot;').replaceAll("'", '&#39;');
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
            <div>Group: ${escapeHtml(cam.group)} | Location: ${escapeHtml(cam.location)}</div>
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

      alerts.slice().reverse().slice(0, 8).forEach((item) => {
        const div = document.createElement('div');
        const priority = ['low', 'medium', 'high'].includes(item.priority) ? item.priority : 'low';
        div.className = `alert-item p-${priority}`;
        div.innerHTML = `<div><strong>${escapeHtml(item.title)}</strong></div><div>${escapeHtml(item.message)}</div><div style="margin-top:6px"><button onclick="ack(${item.id})">Acknowledge</button></div>`;
        list.appendChild(div);

        if (item.id > lastNotificationId) {
          if (window.Notification && Notification.permission === 'granted') {
            new Notification(item.title, { body: item.message });
          }
          lastNotificationId = item.id;
        }
      });
    }

    async function loadStorage() {
      const res = await fetch('/api/storage');
      const data = await res.json();
      const s = data.storage || {};
      document.getElementById('storageInfo').innerHTML = `
        <div>SD: ${s.sdEnabled ? 'Enabled' : 'Disabled'}</div>
        <div>Used: ${s.sdUsedMb || 0} MB / ${s.sdCapacityMb || 0} MB</div>
        <div>Cloud: ${s.cloudEnabled ? 'Enabled' : 'Disabled'}</div>
        <div>Snapshots: ${data.snapshotCount || 0}</div>
        <div>Recordings: ${data.recordingCount || 0}</div>`;
    }

    async function loadTimeline() {
      const res = await fetch('/api/timeline?limit=8');
      const items = await res.json();
      const list = document.getElementById('timelineList');
      list.innerHTML = '';
      items.forEach((item) => {
        const div = document.createElement('div');
        div.style.fontSize = '12px';
        div.style.marginBottom = '6px';
        div.textContent = `[${item.itemType}] ${item.summary}`;
        list.appendChild(div);
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
    loadStorage();
    loadTimeline();
    setInterval(loadCameras, 5000);
    setInterval(loadAlerts, 4000);
    setInterval(loadStorage, 7000);
    setInterval(loadTimeline, 6000);
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
  SnapshotRecord snap = createSnapshot(entry.id, "startup", entry.streamUrl, 220);
  addEvent(entry.id, "snapshot", "Startup snapshot saved (id=" + String(snap.id) + ")");
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

  server.on("/api/detections", HTTP_POST, handleDetection);
  server.on("/api/events", HTTP_GET, handleGetEvents);
  server.on("/api/notifications", HTTP_GET, handleGetNotifications);
  server.on("/api/notifications/ack", HTTP_POST, handleAckNotification);
  server.on("/api/alerts/config", HTTP_POST, handleAlertConfig);

  server.on("/api/snapshots/capture", HTTP_POST, handleCaptureSnapshot);
  server.on("/api/snapshots", HTTP_GET, handleGetSnapshots);
  server.on("/api/recordings/start", HTTP_POST, handleStartRecording);
  server.on("/api/recordings/stop", HTTP_POST, handleStopRecording);
  server.on("/api/recordings", HTTP_GET, handleGetRecordings);
  server.on("/api/storage", HTTP_GET, handleGetStorage);
  server.on("/api/storage/config", HTTP_POST, handleStorageConfig);
  server.on("/api/storage/sync", HTTP_POST, handleSyncAssetToCloud);
  server.on("/api/timeline", HTTP_GET, handleGetTimeline);

  server.begin();

  Serial.println("Core platform server started");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();
}
