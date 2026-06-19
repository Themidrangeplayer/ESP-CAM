#include "esp_camera.h"
#include "img_converters.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>

// =====================
// WiFi / OTA settings
// =====================
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* OTA_HOSTNAME = "esp32cam";
const char* OTA_PASSWORD = ""; // Optional; keep empty to disable OTA auth.

// =====================
// AI-Thinker ESP32-CAM pinout
// =====================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

WebServer server(80);
unsigned long lastReconnectAttemptMs = 0;
const unsigned long reconnectIntervalMs = 10000;

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s != nullptr) {
    s->set_framesize(s, FRAMESIZE_VGA);
  }

  return true;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.printf("Connecting to WiFi SSID: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connection timed out. Auto-reconnect will continue in loop().");
  }
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);

  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    Serial.println("OTA update started");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA update finished");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA progress: %u%%\r", (progress * 100U) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error[%u]\n", error);
  });

  ArduinoOTA.begin();
  Serial.println("OTA ready");
}

void handleRoot() {
  const char page[] PROGMEM =
      "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'/>"
      "<title>ESP32-CAM Stream</title></head><body><h1>ESP32-CAM Live Stream</h1>"
      "<img src='/stream' style='max-width:100%;height:auto;' /></body></html>";
  server.send(200, "text/html", page);
}

void handleStream() {
  WiFiClient client = server.client();

  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: multipart/x-mixed-replace; boundary=frame\r\n");
  client.print("Cache-Control: no-cache\r\n");
  client.print("Connection: close\r\n\r\n");

  while (client.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera frame capture failed");
      break;
    }

    uint8_t* jpg_buf = fb->buf;
    size_t jpg_len = fb->len;
    bool converted = false;

    if (fb->format != PIXFORMAT_JPEG) {
      converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
      esp_camera_fb_return(fb);
      if (!converted) {
        Serial.println("JPEG conversion failed");
        break;
      }
    }

    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", static_cast<unsigned int>(jpg_len));
    client.write(jpg_buf, jpg_len);
    client.print("\r\n");

    if (!converted) {
      esp_camera_fb_return(fb);
    } else {
      free(jpg_buf);
    }

    ArduinoOTA.handle();
    delay(30);
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/stream", HTTP_GET, handleStream);
  server.begin();
  Serial.println("HTTP server started");
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println();

  if (!initCamera()) {
    Serial.println("Camera init failed; restarting...");
    delay(2000);
    ESP.restart();
  }

  connectWiFi();
  setupOTA();
  setupWebServer();

  Serial.println("Milestone 1 firmware ready");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    const unsigned long now = millis();
    if (now - lastReconnectAttemptMs >= reconnectIntervalMs) {
      lastReconnectAttemptMs = now;
      Serial.println("WiFi disconnected, retrying...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  server.handleClient();
  ArduinoOTA.handle();
  delay(1);
}
