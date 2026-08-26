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

The current local `include/secrets.h` is configured for:

```text
wss://192.168.1.3:8443/ws/device
```

It uses the same bearer token as `DEVICE_SHARED_TOKEN` in `.env`. The ESP32 accepts the local untrusted certificate while retaining WSS encryption. Build the firmware with:

```sh
cd watering-system-iot
~/.platformio/penv/bin/pio run
```

The firmware has not been flashed automatically because uploading can affect connected watering hardware. Flash it when the controller and valves are in a safe state.

## Dashboard data

Each telemetry message creates one parent sample and one normalized row per zone. The backend derives watering events from relay transitions and publishes the committed snapshot to authenticated dashboard WebSockets.

Historical queries support 24-hour, 7-day, and 30-day server-side time buckets for moisture, raw/filtered sensor readings, and watering time. CSV export returns raw zone samples for the chosen range.

Useful endpoints:

- `GET /health`
- `WSS /ws/device` for the ESP32
- `WSS /ws/dashboard` for signed-in browsers
- `GET /api/devices`
- `GET /api/devices/{id}/history`
- `GET /api/devices/{id}/events`
- `GET /api/devices/{id}/history.csv`

## Verification

The integration verifier exercises local login, invalid device-token rejection, the exact ESP32 hello/telemetry protocol, dashboard live updates, command acknowledgement, historical aggregation, derived watering events, and CSV export:

```sh
cd backend
set -a; source ../.env; set +a
.venv/bin/python scripts/verify_e2e.py --host localhost --ca certs/ca.crt
```
