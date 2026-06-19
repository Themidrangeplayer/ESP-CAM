# ESP-CAM Core Platform + Detection + Recording/Storage

This repository now includes an ESP32-hosted core camera platform scaffold focused on:

- Multi-camera support
- Camera naming/grouping/location tags
- Camera health monitoring (online/offline, RSSI, battery)
- Live view web dashboard (grid + full-screen view)
- Camera switching data endpoint
- Detection ingestion (motion/human/face)
- Notification queue + dashboard alerts
- Optional webhook notifications
- Snapshot capture
- Event + continuous recording metadata flow
- SD storage usage tracking + cloud backup sync metadata
- Searchable timeline feed

## File

- `esp32cam_core_platform.ino`

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
7. Adds recording + storage foundation:
   - snapshot capture endpoint
   - event recording auto-capture hooks
   - manual continuous recording start/stop
   - SD usage accounting and cloud backup sync calls
   - timeline API with filters/search

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

### `POST /api/snapshots/capture`
Captures a snapshot record:

- `id` (required camera id)
- `reason` (optional, default `manual`)
- `imageUrl` (optional, defaults to camera stream URL)
- `sizeKb` (optional)

### `GET /api/snapshots?cameraId=<id>&limit=30`
Returns recent snapshots.

### `POST /api/recordings/start`
Starts recording metadata session:

- `id` (required camera id)
- `mode` (`event` or `continuous`; default `continuous`)
- `trigger` (optional, default `manual`)
- `source` (optional stream/source URL)

### `POST /api/recordings/stop`
Stops active recording for a camera:

- `id` (required camera id)
- `sizeKb` (optional final size estimate)

### `GET /api/recordings`
Lists recordings with optional filters:

- `cameraId`
- `mode`
- `status` (`recording` or `completed`)

### `GET /api/storage`
Returns storage status (SD/cloud settings, usage, asset counts).

### `POST /api/storage/config`
Updates storage config:

- `sdEnabled` (`true/false`)
- `sdCapacityMb` (integer)
- `cloudEnabled` (`true/false`)
- `cloudEndpoint` (`http://` or `https://`)
- `autoEventRecording` (`true/false`)

### `POST /api/storage/sync`
Triggers cloud backup for an existing asset:

- `assetType` (`snapshot` or `recording`)
- `id` (asset id)

### `GET /api/timeline?cameraId=<id>&type=<itemType>&q=<text>&limit=50`
Returns recent timeline items with optional camera/type/text filters.

## Notes

- Replace Wi-Fi credentials inside the sketch before flashing.
- This milestone is intentionally scoped to the core platform foundation and API/dashboard layer.
- Stream URLs are expected to point to camera live streams (for example ESP32-CAM stream endpoints).
- Browser dashboard requests Notification permission to surface near real-time alerts.
- Dashboard now also surfaces storage usage and recent timeline entries.
- This implementation tracks recording/snapshot metadata and storage state; actual media encoding/writing is represented as platform scaffolding.
