<p align="center">
  <img src="frontend/public/brand/garden-watering-icon-192.png" width="112" alt="Garden Watering icon">
</p>

# Watering system

This workspace contains three pieces:

- `watering-system-iot`: ESP32 firmware with local watering safety and WSS telemetry.
- `backend`: FastAPI, PostgreSQL persistence, Google OIDC, device/dashboard WebSockets, historical APIs, and CSV export.
- `frontend`: the private live and historical dashboard.

For the Padval VM production layout and rollback procedure, see `VM_DEPLOYMENT.md`.

Pull requests run backend tests, frontend lint/build checks, an ESP32 firmware compile with
the non-secret example configuration, Compose validation, and container builds. Merges to
`main` publish digest-pinned images to GHCR and deploy them through the protected GitHub
`production` environment.

## Local start

The certificate has SANs for `192.168.1.3`, `localhost`, `127.0.0.1`, and `Rafayels-MacBook-Pro.local`.

```sh
docker compose up --build -d
```

Open `https://localhost:8443` on this laptop or `https://192.168.1.3:8443` from the LAN. The entire dashboard, API, and WebSocket connection share that secure address. Port 3000 redirects to it automatically.

The browser needs to trust `backend/certs/ca.crt`, or you can visit the secure address once and accept its certificate warning for local review. This browser trust step is separate from the ESP32, which is configured to accept the certificate without validation.

The local `.env` starts with `AUTH_MODE=development`, which provides a one-click local review session. Secrets and certificates are excluded from source control. Use `docker compose logs -f backend` for device/telemetry logs and `docker compose down` to stop the application without deleting historical data.

## Enable Google sign-in

Create a Google OAuth 2.0 client of type **Web application** and configure this exact authorized redirect URI:

```text
https://localhost:8443/auth/google/callback
```

Keep the OAuth consent screen in testing mode and add your own Google account as a test user. Then update `.env`:

```dotenv
AUTH_MODE=google
GOOGLE_CLIENT_ID=your-client-id
GOOGLE_CLIENT_SECRET=your-client-secret
ALLOWED_GOOGLE_EMAILS=your-google-account@example.com
```

Apply it with `docker compose up -d --force-recreate backend`. The backend requests only `openid email profile`, verifies the Google ID token through the OIDC client, enforces the email allowlist, and stores only a local opaque session—not Google access or refresh tokens.

Google allows localhost OAuth redirects. It does not accept the laptop's raw LAN IP or `.local` hostname as a normal web OAuth redirect. Access from other devices therefore needs an owned HTTPS domain; the localhost setup is intended for this laptop.

## ESP32 connection

The current local `include/secrets.h` is configured for the production VM:

```text
wss://watering.sibex.zip/ws/device
```

It uses the same bearer token as production's `DEVICE_SHARED_TOKEN`. The ESP32 currently accepts the server certificate without validation while retaining WSS encryption; this permits connectivity but does not protect against server impersonation. Build the firmware with:

```sh
cd watering-system-iot
~/.platformio/penv/bin/pio run
```

The firmware has not been flashed automatically because uploading can affect connected watering hardware. Flash it when the controller and valves are in a safe state.

### OLED status display

The firmware supports a 0.96-inch, 128 x 64 SSD1306 I2C OLED at address `0x3C`.
Connect VCC to 3.3 V, GND to GND, SDA to D21 / GPIO 21, and SCL to D22 /
GPIO 22. The screen shows tank, network, and four-zone moisture/controller
status; watering continues safely if the OLED is missing. See the
[`watering-system-iot` hardware guide](watering-system-iot/README.md#oled-status-display)
for electrical details.

### Main-tank low-water protection

Use D23 / GPIO 23 for the main-tank input. Pull the tank signal up to 3.3 V
through 5 kOhm, connect that signal to GPIO 23 through 1 kOhm, and connect a
100 nF ceramic capacitor (`104`) from the GPIO 23 side of the series resistor
to GND.
Run the tank signal and GND to the float switch, oriented so it is open while
the tank has enough water and closes to GND when the water level is low. GPIO 23
is configured as a normal `INPUT`: HIGH/open means safe, while LOW/closed means
low water. With this polarity, a disconnected tank wire reads as safe. After a
250-millisecond confirmation, the ESP32 stops every relay locally and blocks
automatic and manual watering. A safe level must remain stable for two seconds
before watering can resume.

Telegram alerts are sent directly by the ESP32, so tank notifications and
optional debug messages continue if the backend is unavailable. Create a bot
with Telegram's `@BotFather`, send the bot one message, and obtain the
destination chat ID from the bot API `getUpdates` response. Configure the
excluded `watering-system-iot/include/secrets.h` before building:

```cpp
constexpr bool TELEGRAM_ENABLED = true;
constexpr char TELEGRAM_BOT_TOKEN[] = "123456:replace-with-real-token";
constexpr char TELEGRAM_CHAT_ID[] = "replace-with-your-chat-id";
```

Low-tank and restored transitions are always sent. The website's Telegram debug
toggle additionally enables pump-start messages and an hourly controller
report. Telegram delivery runs in a separate firmware task and retries without
blocking local control. The hardware cutoff does not depend on Wi-Fi, Telegram,
or the backend.

The Telegram panel also has a **Send debug now** action. It asks the online
ESP32 to send one report directly to Telegram; it does not enable recurring
debug messages. The controller reports queued, sending, retry, success, and
failure states back to the backend. Up to 16 latest unacknowledged results are
kept in ESP32 flash and replayed after the backend reconnects, without exposing
the bot token or chat ID.

## Dashboard data

The overview puts zones first, with controller diagnostics and Telegram under
**System**. Select a zone and choose **Edit name** to save a name of up to 60
characters. Names are shared across signed-in dashboards for that controller;
leave the field blank to return to **Zone 1**, **Zone 2**, and so on. Names are
dashboard metadata and do not change firmware settings or watering behavior.

On the first backend upgrade, an additive `zone_settings` table preserves the
previous **Tomatoes** label for zone 1 of existing controllers. Later starts
preserve saved names and explicit resets. No telemetry or historical rows are
rewritten, and older application versions can ignore the new table on rollback.

Each telemetry message creates one parent sample and one normalized row per zone. The backend derives watering events from relay transitions and publishes the committed snapshot to authenticated dashboard WebSockets.

Historical queries support 24-hour, 7-day, and 30-day server-side time buckets for moisture, raw/filtered sensor readings, and watering time. CSV export returns raw zone samples for the chosen range.

Useful endpoints:

- `GET /health`
- `WSS /ws/device` for the ESP32
- `WSS /ws/dashboard` for signed-in browsers
- `GET /api/devices`
- `PATCH /api/devices/{id}/zones/{zone_id}` with `{"name":"Tomatoes"}` (signed-in users only; empty name resets the label)
- `GET /api/devices/{id}/history`
- `GET /api/devices/{id}/events`
- `GET /api/devices/{id}/telegram/deliveries`
- `POST /api/devices/{id}/telegram/debug`
- `GET /api/devices/{id}/history.csv`

## Verification

The integration verifier exercises local login, invalid device-token rejection,
the exact ESP32 protocol, dashboard live updates, command acknowledgement,
historical aggregation, derived watering events, CSV export, the one-shot
Telegram command, delivery acknowledgements, and persisted Telegram interaction
history:

```sh
cd backend
set -a; source ../.env; set +a
.venv/bin/python scripts/verify_e2e.py --host localhost --ca certs/ca.crt
```

### Frontend development against a local backend

To use the frontend development server with an independently running local
backend, set `GARDEN_API_URL`, for example:

```sh
cd frontend
GARDEN_API_URL=http://127.0.0.1:18080 pnpm dev
```

This proxies `/api`, `/auth`, and `/ws` through the frontend origin. Set the
backend's `FRONTEND_URL` to the URL printed by the frontend server. This mode
uses Vinext's Node development server so the Worker emulator does not claim the
backend's WebSocket upgrades; the production build remains unchanged.

`backend/scripts/verify_zone_names.py` verifies label persistence, validation,
controller isolation, live updates, and a second dashboard session against an
**isolated local development database**. It refuses remote hosts and requires
`AUTH_MODE=development` plus `DEVICE_SHARED_TOKEN` in the script environment.
It creates only the `preview-garden` and `preview-second-controller` fixtures;
`--hold` keeps their simulated telemetry running for a local UI preview.

### History and delivery regression checks

History CSV exports stream all matching rows in bounded batches (up to a 366-day
range) and close their database cursor when a client disconnects. History keeps
its selected timestamp and keyboard focus during background refreshes; period
and metric filters are saved in the URL. Telegram reports merge by update
sequence and refresh after every dashboard reconnection.

The backend requests the controller's read-only `config.get` report on connection
and when its reported configuration revision changes. It retains only validated
zone thresholds in `device_configurations`. Dashboard thresholds are shown only
when that saved revision matches the controller's current revision; unavailable
settings are never replaced with invented defaults. No firmware update is needed.

Run `pnpm build` and `pnpm test:e2e` in `frontend` after installing Chromium with
`pnpm exec playwright install chromium`. The browser suite runs the production
frontend against isolated HTTP/WebSocket fixtures on port 4173, on desktop and
mobile viewports. It covers slow/failed refreshes, focus and reading preservation,
filter navigation/reload, reconnect delivery ordering, large CSV downloads, and
threshold/timestamp visibility. These checks also run in the production pipeline.
Backend tests cover full exports beyond 100,000 rows, cancellation cleanup,
authentication, range validation, and configuration revision/threshold validation.
