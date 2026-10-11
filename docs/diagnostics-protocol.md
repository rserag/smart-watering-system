# Diagnostics implementation contract

Diagnostics is a fourth dashboard view. All authenticated users may inspect it;
only `sahakyan.raphayel@gmail.com` may issue diagnostic or conflicting control
commands. Development previews must use that identity explicitly for operator
fixtures; development mode does not bypass authorization.

## Device protocol (version 1)

`device.hello` and normal `telemetry` gain `diagnosticsVersion: 1` and a `debug`
object (the session state below). Existing firmware may omit these fields.
New firmware uses version 0.8.0. Normal telemetry during simulation must not
store injected zone values as ordinary history or generate ordinary watering
events. Diagnostic hardware watering is labelled diagnostic rather than automatic.

Wire commands use `type`, `requestId`, `schemaVersion`, `deviceId`,
`expectedBootId`, `expiresAtEpoch` (fixed when the request is created, maximum
30 seconds), and `sessionId` where applicable. Replies use existing
`command.ack` with accepted/rejected and a message. Duplicate command IDs must
never replay actuation. Firmware must validate boot, deadline, mode, and lease.

Commands:

- `debug.snapshot.get`: read-only, sends `debug.snapshot` with matching requestId.
- `debug.session.start`: mode `hardware` or `simulation`, sessionId UUID,
  owner operator email. Stops all outputs, clears queued activity, holds
  automation, and grants one 30-second lease. Reject a competing active session.
- `debug.session.renew`: renew matching lease for 30 seconds.
- `debug.session.end`: stop all, clear injections, release lease but retain the
  durable automation hold. Ordinary operation requires explicit resume.
- `debug.automation.resume`: no active session, release durable hold, restore
  original configuration without overwriting calibration/pause/fault settings.
- `debug.output.pulse`: zoneId 1..4, durationMs 100..60000. Hardware mode bypasses
  soil validity/enabled/pause/fault eligibility but retains the real tank cutoff,
  independent timed cutoff, and one-output concurrency. Never clear faults or
  pauses as a side effect. Simulation uses a virtual pulse only.
- `debug.output.stop`: zoneId optional (all when omitted). Also allowed without
  a session in observation mode; stops activity and persists an automation hold.
- `debug.input.set`: input `soil` with zoneId and raw 0..4095, or input `tank`
  with low boolean. Simulation only. Injection goes before filter/validation
  and before tank debounce; preserve real observations separately.
- `debug.input.clear`: input `soil`/`tank`/`all`, zoneId optional.
- `debug.sample`: acquire a fresh normal-sized ADC batch (no unbounded loop).
- `debug.oled.test`: pattern `checkerboard`/`text`/`off`, durationMs 100..10000.
  Display returns to normal on completion/session termination.
- `debug.i2c.scan`: scan bounded I2C addresses, report found addresses via action
  event. Must not alter pin assignments; use short bus timeout.

All physical outputs remain LOW in simulation, including virtual automatic
decisions/pulses. Real tank LOW always cuts physical relays, even in testing.
Firmware lease uses monotonic time and local checks; disconnect and reboot stop
outputs. Durable automation hold persists across reboot until explicit resume.
All controller and Telegram recovery/config paths must respect the hold/session.
Failure to create/start the cutoff timer prohibits physical pulses.

## Snapshot

`debug.snapshot` contains type, schemaVersion, deviceId, bootId, sequence,
uptimeMs, configRevision, requestId (optional), session, hardware, tank, zones.
Optional timings reports loopDurationUs, maxLoopDurationUs, sampleDurationUs.
Snapshots are sent at normal telemetry cadence while observing and every 1s
while a session is active, also on meaningful state changes and requests.
Snapshot sequences increase independently of normal telemetry.

Session: mode `observe`/`hardware`/`simulation`, sessionId null|string, owner
null|string, leaseRemainingMs integer, automationHeld boolean.

Hardware: board `esp32dev`, relayPins [27,26,25,33], sensorPins [34,35,36,39],
tankPin 23, relayActiveHigh true, oled {sda:21,scl:22,address:60,available:bool}.
The physical 30-pin header layout follows the user's board photo, with USB at
the top: left 3V3/GND/15/2/4/RX2/TX2/5/18/19/21/RX0/TX0/22/23; right
VIN/GND/13/12/14/27/26/25/33/32/35/34/VN/VP/EN. VN is GPIO39 and VP GPIO36.
Wire colors and hidden wiring routes are not inferred from the photo.

Tank: physicalLow boolean, effectiveLow boolean (before debounce), low boolean
(debounced effective), pendingLow boolean, debounceRemainingMs integer,
overrideLow null|boolean.

Zones: id; physicalRaw; raw (effective batch mean); samples (16 physical ADC
conversions); minRaw; maxRaw; filteredRaw; relativeMoisturePercent;
unclampedPercent; sensorValid; validSamples; invalidSamples; dryCount; wetCount;
sampleAgeMs; phase; fault null|string; enabled; paused; relayOn (physical
commanded state); virtualRelayOn; decisionReason; phaseElapsedMs;
pulseRemainingMs; wateringOnMsThisCycle; overrideRaw null|integer; dryRaw;
wetRaw; startWateringPercent; stopWateringPercent; pulseMs; soakMs; maxOnMs.
Each snapshot is one correlated observation; show boot/sequence/config and
backend receivedAt. Stale filtered/percentage values after invalid ADC must be
visibly labelled rather than implied fresh.

## Action events

`debug.action` contains type, schemaVersion, deviceId, bootId, sessionId,
requestId, command, status (`started`, `completed`, `cancelled`), uptimeMs,
zoneId optional, message optional, result optional (e.g. I2C addresses).
Send started and terminal events for timed pulses/OLED tests. Bind the requestId
to actual execution; command ACK alone is not completion. Snapshot reports
reconcile state when action delivery is uncertain.

## HTTP API and browser data

- GET `/api/devices/{id}/diagnostics`: {canControl, supported, snapshot,
  receivedAt, actions, captures}. Offline/old firmware gets truthful fallback
  observation from existing telemetry/config; hardware map is documented fallback
  and marked as such. GET never sends a mutating command.
- POST `/api/devices/{id}/debug/session`: {mode,expectedBootId}, returns
  {sessionId, status, snapshot}. Server supplies UUID/owner/expiry.
- POST `/api/devices/{id}/debug/session/renew`: {sessionId,expectedBootId}.
- POST `/api/devices/{id}/debug/session/end`: {sessionId,expectedBootId}.
- POST `/api/devices/{id}/debug/actions`: {command,sessionId?,expectedBootId,
  parameters}. Command is one of the allowlisted wire types above (not session
  commands/snapshot.get). Strict per-command validation.
- POST `/api/devices/{id}/debug/captures`: {name}; captures the bounded backend
  live ring buffer. Inspection/export allowed to authenticated viewers; creation
  restricted to operator. GET `/api/devices/{id}/debug/captures/{captureId}`.
- GET `/api/devices/{id}/debug/actions`: last 200 command/event records.

The diagnostics GET may request a fresh read-only snapshot from online supported
firmware with a bounded timeout, falling back to cached/current state.
Frontend refreshes GET once per second while visible, avoiding overlapping
requests. Uses normal dashboard WS to refresh availability. This avoids exposing
sessionId through new broadcasts and retains existing dashboard transport.
Snapshot responses redact sessionId for viewers. Backend owns a bounded 600
snapshot ring/device. Store command/session audit for 30 days, captures 7 days,
purge periodically with bounded work. Additive tables preserve rollback.

Actions are online-only, never generic offline queues. Only authorized operator
can mutate. Require fresh same-boot diagnostics and active lease/session matching
for actions. Protect generic watering/config/recovery/Telegram actuation from
conflicts; old queued watering must not fire during debug or after reboot hold.
Stop should be available even when telemetry is stale, bound to current boot and
authorized operator. Readbacks and snapshots distinguish acceptance/execution.
Reject token/boot changes, invalid targets, duplicates and competing sessions.

## Frontend behavior

Single responsive Diagnostics view: sticky controller/mode/freshness/session/Stop
bar; linked wiring map and zone selector; ADC pipeline and live unbucketed trace;
physical/effective input override editor; timed relay and OLED/I2C controls;
scenario presets plus editable sequence runner; advanced validated command
picker/JSON parameters; command audit and saved/exported JSON/CSV captures.
Sequences use ordinary bounded actions, renew lease every 5s, abort on failure,
mode/boot/session change or stop. Read-only viewers never render enabled mutation
controls. Simulation must be visibly distinct and old firmware controls disabled.

## Verification

Host firmware tests: cutoff/lease expiry, boot hold, sensor bypass, real tank,
no physical simulation outputs, injection pipeline, duplicate IDs, concurrent
session/relay rejection, resume, and configuration/recovery conflicts.
Backend tests: operator authorization, validation, offline/stale/boot/lease,
ACK uncertainty, capture bounds/isolation/retention and old firmware fallback.
Frontend desktop/mobile fixtures: full observation and controls, timer/Stop,
read-only access, rejected/stale/reboot/offline, scenarios and trace/export.
Production verification is read-only until the user connects the ESP32 at the
end; do not command existing hardware during website deployment.

## Research and design choices

The existing authenticated website and outbound device WebSocket provide one
place to inspect and control the system. A separate ESP32-hosted web server
would add another authentication interface and require LAN access; a serial-only
console would require the USB cable for every session. The unified website uses
the existing transport, with firmware-owned execution and an optional USB serial
monitor during physical commissioning.

Local one-shot timers implement output deadlines; a separate timer enforces the
lease and a bounded periodic callback watches the real tank during hardware
tests. Espressif documents that timer task callbacks are serialized and should
be short. These callbacks only write outputs/flags; JSON, storage, networking,
and reporting run outside them. This protects against delays in the application
network loop, but does not constitute independent electrical relay feedback or
a guarantee against a failed MCU. See
[Espressif's ESP Timer documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/esp_timer.html).

Raw ADC counts, relative calibration, and software relay commands are shown
separately from physical feedback that this installation does not measure.
Simulation copies the runtime rather than changing configuration, so fault,
threshold, and tank scenarios can be exercised without energizing a relay or
replacing saved calibration. The command allowlist follows the actual configured
hardware rather than permitting arbitrary GPIO direction changes.

Backend TLS verification uses public trust roots obtained from the
[Google Trust Services repository](https://pki.goog/repository/) and
[Let's Encrypt certificate documentation](https://letsencrypt.org/certificates/),
with a private-CA override for local development.
