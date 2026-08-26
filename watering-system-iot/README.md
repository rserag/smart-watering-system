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
- Sensor and maximum-watering faults turn the relay off and latch the zone in a
  fault state.

Automatic watering is enabled for Zone 1 in the current defaults. Zones 2–4
remain disabled until their sensors are calibrated. At boot, the serial output
prints both the global automation state and each zone's enabled state.

## Configuration persistence

The firmware starts with compiled safe defaults, then loads a valid saved
configuration from ESP32 nonvolatile storage when available. WebSocket
configuration snapshots are validated as a whole, saved, and only then applied.
An invalid or older snapshot leaves the current configuration in place.

GPIO assignments and absolute safety limits cannot be changed remotely.

## WebSocket setup

Copy `include/secrets.h.example` to `include/secrets.h` and configure:

- Wi-Fi SSID and password.
- A unique device ID and token.
- WebSocket host, port, and path.
- `WEBSOCKET_ENABLED = true` after those values are present.

The ESP32 makes an outbound `wss://` connection and sends the token in an
`Authorization: Bearer <token>` header. Certificate verification is disabled,
so a self-signed or otherwise untrusted server certificate is accepted. WSS
still encrypts traffic, but it does not authenticate the server and is therefore
vulnerable to a machine-in-the-middle impersonating the backend. This mode is
intended only for the current trusted local-network setup.

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
- `fault.clear` with `zoneId`.
- `telemetry.request`.
- `config.get`.

The device remembers a bounded set of recent request IDs to avoid executing an
immediate duplicate command. Expiration prevents an old watering command from
being replayed after a reboot.

## Telemetry

The device sends a `device.hello` after connecting, then telemetry on the
configured interval and immediately after sensor or controller state changes.
Each zone reports:

- Latest and filtered raw ADC values.
- Relative calibrated moisture percentage.
- Sensor validity.
- Controller phase and relay state.
- Relay-on time accumulated during the current cycle.
- Fault code, when present.

Relative moisture percentage is a device calibration scale and is not a
laboratory volumetric-water-content measurement.

## Build

The project uses PlatformIO with the Arduino ESP32 framework. ArduinoJson,
WebSockets, Preferences, and Wi-Fi dependencies are resolved by PlatformIO.
