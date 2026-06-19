# ESP-CAM Core Camera Platform + Detection Alerts

This repository now includes an ESP32-hosted core camera platform scaffold focused on:

- Multi-camera support
- Camera naming/grouping/location tags
- Camera health monitoring (online/offline, RSSI, battery)
- Live view web dashboard (grid + full-screen view)
- Camera switching data endpoint
- Detection ingestion (motion/human/face)
- Notification queue + dashboard alerts
- Optional webhook notifications

## File

- `/home/runner/work/ESP-CAM/ESP-CAM/esp32cam_core_platform.ino`

## What this milestone provides

The sketch acts as a lightweight coordinator:

1. Maintains a camera registry (`id`, `name`, `group`, `location`, `streamUrl`)
2. Accepts heartbeat updates to track:
   - `online` / `offline` status
   - `battery` level
   - signal strength (`rssi`)
3. Serves a web dashboard with:
   - live camera cards in a grid
   - click-to-fullscreen view
4. Exposes JSON APIs for camera management and live-view switching integration.
5. Ingests detection signals and raises prioritized alerts:
   - Motion alerts
   - Human alerts
   - Face alerts (known + unknown)
6. Provides alert/event feeds for dashboard and integrations.

## API

### `GET /api/cameras`
Returns all camera records and health status.

### `POST /api/cameras/upsert`
Upserts camera metadata with form/query arguments:

- `id` (required)
- `name`
- `group`
- `location`
- `stream`

### `POST /api/heartbeat`
Updates camera health with form/query arguments:

- `id` (required)
- `battery` (0-100)
- `rssi` (signal strength)

### `GET /api/camera/stream?id=<cameraId>`
Returns camera name and stream URL for switching flows.

### `POST /api/detections`
Ingests detection signals with form/query arguments:

- `id` (required)
- `motion` (`true/false`, `1/0`, `on/off`)
- `human` (`true/false`, `1/0`, `on/off`)
- `face` (`true/false`, `1/0`, `on/off`)
- `faceName` (optional; use `unknown` for unknown faces)

### `GET /api/events?limit=25`
Returns recent event history (motion/human/face and system events).

### `GET /api/notifications?unacked=true`
Returns queued alert notifications.

### `POST /api/notifications/ack?id=<notificationId>`
Marks a notification as acknowledged.

### `POST /api/alerts/config`
Sets outbound webhook notification target:

- `webhook` (optional URL)

When set, notifications are also POSTed as JSON to the webhook.

## Notes

- Replace Wi-Fi credentials inside the sketch before flashing.
- This milestone is intentionally scoped to the core platform foundation and API/dashboard layer.
- Stream URLs are expected to point to camera live streams (for example ESP32-CAM stream endpoints).
- Browser dashboard requests Notification permission to surface near real-time alerts.
