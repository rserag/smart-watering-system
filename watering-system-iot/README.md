# Watering System ESP32 Firmware

The ESP32 monitors four capacitive soil-moisture sensors and controls four relay
channels. Watering decisions run locally and continue when Wi-Fi or the backend
is unavailable.

## Hardware mapping

| Zone | Relay GPIO | Moisture ADC GPIO |
| --- | ---: | ---: |
| 1 | 27 | 34 |
| 2 | 26 | 35 |
| 3 | 25 | 36 / VP |
| 4 | 33 | 39 / VN |

### OLED status display

Connect a 0.96-inch 128 x 64 I2C OLED with an SSD1306 controller as follows:

| OLED pin | ESP32 connection |
| --- | --- |
| VCC | 3.3 V |
| GND | GND |
| SDA | D21 / GPIO 21 |
| SCL | D22 / GPIO 22 |

The firmware uses I2C address `0x3C` at 400 kHz. Use the four-pin I2C version
of the display, not the visually similar SPI version. Many OLED boards already
include I2C pull-up resistors; any pull-ups must go to 3.3 V, never 5 V.

The display shows the main-tank interlock, automatic/manual mode, Wi-Fi (`W`),
backend WebSocket (`B`), moisture percentage, and controller state for all four
zones. A plus sign means the corresponding connection is active. A missing or
failed OLED is logged over serial and does not prevent local watering.

The main-tank float switch uses D23 / GPIO 23 with an external 3.3 V pull-up:

```text
3.3 V -- 5 kOhm --+-- tank signal -- switch -- GND
                  |
                  +-- 1 kOhm --+-- D23 / GPIO 23
                               |
                             100 nF (104)
                               |
                              GND
```

Mount and orient the switch so it is open while the water level is safe and
connects the tank signal to GND when the water level is low. GPIO 23 is
configured as a normal `INPUT`; the external resistor, not the ESP32's internal
pull-up, defines the open state. A high input means water is available, while a
low input engages the watering interlock. With this polarity, a disconnected
tank cable reads as ready rather than low water. Use 3.3 V, not 5 V, and
preferably use a twisted signal/GND pair for the tank cable.

The transistor relay drivers are active when the ESP32 output is `HIGH`. All
relays are driven `LOW` before the controller loads saved configuration.

## Default control behavior

- Sensors are sampled every 5 seconds.
- Each measurement averages 16 ADC conversions and is then filtered.
- The first three valid measurement cycles are used for stabilization.
- Provisional calibration is `2600` dry and `1250` wet.
- Watering demand begins at or below 30% after three consecutive measurements.
- A zone waters for 8 seconds and then soaks for 90 seconds.
- Pulses continue until two readings reach at least 63%.
- Total relay-on time is limited to 60 seconds per cycle.
- An independent ESP32 timer cuts off each relay pulse even if network handling
  temporarily delays the main control loop.
- A completed zone enters a 30-minute cooldown.
- Only one zone can energize a relay at a time.
- A low-water signal is confirmed within 250 milliseconds, then every relay is
  turned off and both automatic and manual watering are blocked. Recovery is
  debounced for two seconds before watering can resume.
- Sensor and maximum-watering faults turn the relay off and latch the zone in a
  fault state.

Automatic watering is enabled for Zones 1–3 in the compiled defaults; Zone 4
is disabled. Saved configuration takes precedence. At boot, the serial output
prints both the global automation state and each zone's enabled state.

## Configuration persistence

The firmware starts with compiled safe defaults, then loads a valid saved
configuration from ESP32 nonvolatile storage when available. WebSocket
configuration snapshots are validated as a whole, saved, and only then applied.
An invalid or older snapshot leaves the current configuration in place.

GPIO assignments and absolute safety limits cannot be changed remotely.

The Telegram debug preference is stored under a separate NVS key, so changing
it does not replace the watering configuration or reset calibration. Its safe
default is disabled.

The independent hourly-notification sound preference uses `tg_hr_silent` in
the same NVS namespace. It defaults to false (with sound), survives reboot,
and can be changed without changing watering settings or enabling debug.

## WebSocket setup

Copy `include/secrets.h.example` to `include/secrets.h` and configure:

- Wi-Fi SSID and password.
- `TELEGRAM_ENABLED`, the Telegram bot token, and destination chat ID for
  backend-independent notifications.
- A unique device ID and token.
- WebSocket host, port, and path.
- `WEBSOCKET_ENABLED = true` after those values are present.

The ESP32 makes an outbound `wss://` connection and sends the token in an
`Authorization: Bearer <token>` header. Firmware 0.8.0 verifies the server
certificate with the public Google Trust Services R4 and ISRG Root X1 roots in
`include/backend_trust.h`. It waits for a valid NTP clock before connecting.
For a private local CA, define `WEBSOCKET_CA_CERT` as the PEM certificate string
in the excluded `include/secrets.h`. Failed verification never falls back to
an unverified connection.

## Website diagnostics

Firmware 0.8.0 supports the website's unified **Diagnostics** page. Wiring is
fixed to the table above and tank GPIO 23. Unconfigured GPIOs and power pins are
inspect-only. Diagnostic snapshots preserve all sixteen physical ADC samples,
the physical/effective batch values, filtering and calibration stages, tank
debounce, controller counters/reasons, commanded relay state, and output/session
deadlines. The displayed moisture is a calibrated relative percentage, not a
measurement of volumetric water content.

Hardware and simulation sessions acquire an exclusive 30-second lease. Starting
either stops watering and persists an automation hold in NVS. The dashboard
renews the lease while open; local expiry, WebSocket disconnect, and reboot
stop outputs and clear test overrides. Automation remains held until an explicit
`debug.automation.resume` after the session ends. Existing faults, pauses,
calibration, and enabled settings are preserved.

Hardware relay tests accept 100–60,000 ms, one relay at a time. They bypass
ordinary soil/enabled/paused/fault eligibility so each configured output can be
tested, while retaining the real tank interlock and independent local cutoff.
Simulation substitutes soil/tank inputs before filtering/validation/debounce
and runs virtual outputs with every physical relay kept LOW. Its runtime is
discarded on exit; real sensor stabilization is reset before automation resumes.
Synthetic telemetry is marked and excluded from normal backend history.

Additional commands request fresh sensor batches, perform a bounded I2C scan,
or display OLED checkerboard/text/off patterns for at most ten seconds.
Timed actions report actual start and completion/cancellation events, separately
from acceptance. Duplicate, expired, previous-boot, or previous-session requests
cannot actuate outputs again. The backend exposes control only to
`sahakyan.raphayel@gmail.com` and never queues diagnostic commands.

See [`../docs/diagnostics-protocol.md`](../docs/diagnostics-protocol.md) for the
wire/API contract and the root README for the dashboard workflow.

## Direct Telegram notifications

The ESP32 contacts `api.telegram.org` directly over certificate-validated
HTTPS. Network work runs in a dedicated low-priority task, so DNS, TLS, and
Telegram response delays cannot block sensor sampling, tank detection, or relay
timing. Multiple NTP servers provide the valid clock required for TLS.

Low-tank and tank-restored messages are always enabled when Telegram credentials
are configured. The persisted Telegram debug toggle additionally sends every
pump start and one controller report per hour. Enabling debug sends the first
report immediately. A one-shot debug report can also be requested from the
website without changing that toggle. Failed messages remain in bounded
in-memory retry queues; critical tank messages use a separate queue from debug
traffic.

Firmware 0.6.0 supports silent hourly reports through the website’s **Silent
hourly updates** switch and Telegram’s `/settings` menu. The menu offers
**Silent** and **With sound** buttons; `/hourly_silent on` and
`/hourly_silent off` provide the same controls, while `/hourly_silent` or
`/start` displays the menu. Only hourly reports receive `disable_notification`;
tank alerts, pump-start messages, and on-demand reports retain sound. The
notification job captures its sound mode when queued, including for retries.

The Telegram worker polls `getUpdates` without long polling, processes only
messages and callbacks from the configured numeric chat ID, and hands setting
changes to the main loop through a queue. The main loop persists changes before
confirming them and immediately publishes the setting through telemetry when
connected. Forwarded messages and bot senders are ignored. The update cursor is
saved separately so commands are not replayed after restarting. Telegram
controls continue to work without the backend. Use a dedicated bot without a
webhook or another `getUpdates` consumer; a group chat allows its members to
control this notification preference. Commands are accepted without a bot
username suffix.

Every notification has a non-secret delivery record containing its kind,
attempt, HTTP/API result, and Telegram message ID. The latest 16 records are
persisted in NVS until the backend acknowledges them, so results produced while
the backend is unavailable are replayed after reconnection. Message text,
bot token, and chat ID are never included in this reporting channel.

The bot token is a device credential. Keep `include/secrets.h` out of source
control, use a dedicated watering bot, and rotate the token if a programmed
device is lost. Production deployments should also consider ESP32 flash
encryption and secure boot.

## Message requirements

Every backend-to-device message must include:

```json
{
  "type": "message.type",
  "schemaVersion": 1,
  "deviceId": "watering-system-01",
  "requestId": "unique-request-id"
}
```

Messages larger than 4096 bytes, unsupported schema versions, mismatched device
IDs, malformed JSON, and unsupported message types are rejected.

### Configuration snapshot

`config.set` is a complete replacement, not a patch. It must contain all four
zones and a revision newer than the active revision.

```json
{
  "type": "config.set",
  "schemaVersion": 1,
  "deviceId": "watering-system-01",
  "requestId": "cfg-13",
  "revision": 13,
  "config": {
    "automaticWateringEnabled": true,
    "sampleIntervalMs": 5000,
    "telemetryIntervalMs": 5000,
    "maxConcurrentZones": 1,
    "zones": [
      {
        "id": 1,
        "enabled": true,
        "dryRaw": 2600,
        "wetRaw": 1250,
        "startWateringPercent": 30,
        "stopWateringPercent": 63,
        "dryConfirmationSamples": 3,
        "wetConfirmationSamples": 2,
        "pulseOnMs": 8000,
        "soakMs": 90000,
        "maxWateringOnMsPerCycle": 60000,
        "cooldownMs": 1800000
      },
      {
        "id": 2,
        "enabled": true,
        "dryRaw": 2600,
        "wetRaw": 1250,
        "startWateringPercent": 30,
        "stopWateringPercent": 63,
        "dryConfirmationSamples": 3,
        "wetConfirmationSamples": 2,
        "pulseOnMs": 8000,
        "soakMs": 90000,
        "maxWateringOnMsPerCycle": 60000,
        "cooldownMs": 1800000
      },
      {
        "id": 3,
        "enabled": true,
        "dryRaw": 2600,
        "wetRaw": 1250,
        "startWateringPercent": 30,
        "stopWateringPercent": 63,
        "dryConfirmationSamples": 3,
        "wetConfirmationSamples": 2,
        "pulseOnMs": 8000,
        "soakMs": 90000,
        "maxWateringOnMsPerCycle": 60000,
        "cooldownMs": 1800000
      },
      {
        "id": 4,
        "enabled": true,
        "dryRaw": 2600,
        "wetRaw": 1250,
        "startWateringPercent": 30,
        "stopWateringPercent": 63,
        "dryConfirmationSamples": 3,
        "wetConfirmationSamples": 2,
        "pulseOnMs": 8000,
        "soakMs": 90000,
        "maxWateringOnMsPerCycle": 60000,
        "cooldownMs": 1800000
      }
    ]
  }
}
```

The device responds with `config.ack`, with status `applied` or `rejected` and
an explanatory message when rejected.

Send `config.get` to receive the active `config.snapshot`:

```json
{
  "type": "config.get",
  "schemaVersion": 1,
  "deviceId": "watering-system-01",
  "requestId": "get-config-1"
}
```

### Telegram debug setting

The website uses a focused command rather than replacing the watering
configuration. The ESP32 acknowledges it only after saving the setting to NVS:

```json
{
  "type": "telegram.debug.set",
  "schemaVersion": 1,
  "deviceId": "watering-system-01",
  "requestId": "telegram-debug-1",
  "enabled": true
}
```

The one-shot website test uses an expiring `telegram.debug.send` command. It is
accepted only while Telegram is configured and the ESP32 clock is synchronized,
and it sends regardless of the recurring debug toggle. The job is deferred
until the inbound WebSocket callback has released its TLS/JSON working memory,
so opening Telegram's second TLS connection cannot collide with command parsing:

```json
{
  "type": "telegram.debug.send",
  "schemaVersion": 1,
  "deviceId": "watering-system-01",
  "requestId": "telegram-debug-now-1",
  "expiresAtEpoch": 1787402700
}
```

### Hourly notification sound

The website uses an acknowledged command with a strict boolean value. The
ESP32 saves it before acknowledging acceptance:

```json
{
  "type": "telegram.hourlySilent.set",
  "schemaVersion": 1,
  "deviceId": "watering-system-01",
  "requestId": "hourly-silent-1",
  "enabled": true
}
```

Both `device.hello` and `telemetry` include `telegramHourlySilent`. Old
firmware reports are interpreted as false, and the web UI requires firmware
0.6.0 before enabling this control.

### Manual watering

Manual watering bypasses the moisture threshold but still requires a valid
sensor and obeys zone, concurrency, pulse, and maximum-time safety limits.
`expiresAtEpoch` is UTC epoch seconds and must be within the next five minutes.

```json
{
  "type": "zone.water",
  "schemaVersion": 1,
  "deviceId": "watering-system-01",
  "requestId": "water-zone-2-1042",
  "zoneId": 2,
  "durationMs": 10000,
  "expiresAtEpoch": 1787402700
}
```

Other supported commands are:

- `zone.stop` with `zoneId`.
- `system.stopAll`, which latches all zones in emergency-stop fault state.
- `fault.clearPaused` and `zone.resume` with `zoneId`, `expectedBootId`,
  `expectedRecoveryGeneration`, and `expiresAtEpoch` (within the next 60 seconds).
  `fault.clear` is rejected so older clients cannot bypass the confirmed pause.
- `telemetry.request`.
- `config.get`.
- `telegram.debug.set` with boolean `enabled`.
- `telegram.debug.send` with a valid `expiresAtEpoch`.
- `telegram.hourlySilent.set` with boolean `enabled`.

The device remembers a bounded set of recent request IDs to avoid executing an
immediate duplicate command. Expiration prevents an old watering command from
being replayed after a reboot.

### Durable channel recovery (firmware 0.7.0)

`fault.clearPaused` clears only the current `maximum_watering_reached` occurrence
and saves a channel pause before releasing the fault. Paused channels cannot
water automatically or manually. The pause is stored independently of watering
configuration and survives reboot, configuration updates, Stop, and tank-level
transitions. Missing pause records mean no channels paused; corrupt or unreadable
records keep all channels paused until an explicit successful recovery.

`zone.resume` requires the expected occurrence generation, an enabled paused
channel with no active fault, and a ready tank. It saves the released pause and
then enters `stabilizing`: all validation and wet/dry counters reset, and three
fresh valid samples are required before automatic or manual watering is eligible.
Commands for an old boot, generation or expired confirmation are rejected.
Duplicate command IDs replay their original outcome without repeating recovery.

Telemetry adds `paused` and `recoveryGeneration` to each zone. The OLED reports
`PAUSED`. The dashboard consumes these fields and requires firmware 0.7.0.

The direct Telegram bot adds audible watering-limit alerts independently of
debug, `/faults`, `/clearfault N` and `/resume N`. Commands open confirmations;
they never actuate immediately. Actor-bound confirmation nonces expire after
60 seconds. Completed buttons are removed; stale/replayed callbacks cannot
clear a later fault or resume another pause. Private-chat recovery requires the
configured numeric owner. Group recovery requires a numeric
`TELEGRAM_OPERATOR_USER_ID` macro in `secrets.h` (empty by default), in addition
to the chat allowlist. Only the main controller task performs clear/resume.

Recovery notifications use `maximum_watering_fault`, `fault_cleared_paused` and
`channel_resumed` delivery kinds. Recovery result request IDs include the
Telegram actor, update ID and channel for the persisted interaction history.
Telegram changes reach the dashboard through state telemetry when connected.
Dashboard command expiry uses the NTP clock even when Telegram is disabled.

## Telemetry

The device sends a `device.hello` after connecting, then telemetry on the
configured interval and immediately after sensor or controller state changes.
The top-level `mainTankLow` field reports the debounced float-switch state.
It also reports the persisted Telegram debug value, whether direct Telegram is
configured, the pending message count, and the result of the latest delivery.
Each zone reports:

- Latest and filtered raw ADC values.
- Relative calibrated moisture percentage.
- Sensor validity.
- Controller phase and relay state.
- Relay-on time accumulated during the current cycle.
- Fault code, when present.

Relative moisture percentage is a device calibration scale and is not a
laboratory volumetric-water-content measurement.

Delivery lifecycle updates use `telegram.delivery`. The backend replies with
`telegram.delivery.ack`; only then may the ESP32 remove that persisted audit
record. This acknowledgement affects observability only and never participates
in Telegram delivery or watering control.

## Build

The project uses PlatformIO with the Arduino ESP32 framework. ArduinoJson,
WebSockets, Preferences, and Wi-Fi dependencies are resolved by PlatformIO.

The notification sound and Telegram command policies can also be checked on a
host without hardware:

```sh
c++ -std=c++11 -Wall -Wextra -Werror -Iinclude tests/telegram_settings_test.cpp -o /tmp/telegram-settings-test
/tmp/telegram-settings-test
c++ -std=c++11 -Wall -Wextra -Werror -Iinclude tests/telegram_recovery_test.cpp -o /tmp/telegram-recovery-test
/tmp/telegram-recovery-test
sh tests/run_controller_recovery_tests.sh
```
