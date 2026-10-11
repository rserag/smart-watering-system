"""Isolated HTTP/WebSocket Diagnostics smoke test and optional local preview.

Never connects to a remote backend or physical controller. Start an isolated
development backend with DEVELOPMENT_USER_EMAIL=sahakyan.raphayel@gmail.com
and supply its fixture DEVICE_SHARED_TOKEN in the environment.
"""

import argparse
import asyncio
import contextlib
import json
import os
import time
from urllib.parse import urlparse
from uuid import uuid4

import httpx
import websockets


DEVICE = "preview-diagnostics"


class FixtureController:
    def __init__(self):
        self.boot = f"diagnostics-fixture-{uuid4().hex}"
        self.started = time.monotonic()
        self.sequence = 0
        self.telemetry_sequence = 0
        self.mode = "observe"
        self.session_id = None
        self.owner = None
        self.held = False
        self.lease_until = 0
        self.overrides = {}
        self.tank_override = None
        self.pulses = {}
        self.tasks = set()

    def uptime(self):
        return int((time.monotonic() - self.started) * 1000)

    def session(self):
        return {
            "mode": self.mode, "sessionId": self.session_id, "owner": self.owner,
            "leaseRemainingMs": max(0, int((self.lease_until - time.monotonic()) * 1000)),
            "automationHeld": self.held,
        }

    def zones(self):
        zones = []
        for index in range(1, 5):
            physical = 2250 - index * 110
            raw = self.overrides.get(index, physical)
            percent = max(0, min(100, round(100 * (2600 - raw) / 1350)))
            pulse = self.pulses.get(index)
            zones.append({
                "id": index, "physicalRaw": physical, "raw": raw,
                "samples": [physical + (i % 5 - 2) * 2 for i in range(16)],
                "minRaw": physical - 4, "maxRaw": physical + 4,
                "filteredRaw": raw, "relativeMoisturePercent": percent,
                "unclampedPercent": 100 * (2600 - raw) / 1350,
                "sensorValid": 50 <= raw <= 4050, "validSamples": 3,
                "invalidSamples": 0, "dryCount": 1 if percent <= 30 else 0,
                "wetCount": 2 if percent >= 63 else 0, "sampleAgeMs": 0,
                "phase": "manual_watering" if pulse else "monitoring",
                "fault": None, "enabled": index < 4, "paused": False,
                "relayOn": bool(pulse) and self.mode == "hardware",
                "virtualRelayOn": bool(pulse) and self.mode == "simulation",
                "decisionReason": "Diagnostic pulse" if pulse else "Observing sensor",
                "phaseElapsedMs": 0,
                "pulseRemainingMs": max(0, int((pulse[0] - time.monotonic()) * 1000)) if pulse else 0,
                "wateringOnMsThisCycle": 0, "overrideRaw": self.overrides.get(index),
                "dryRaw": 2600, "wetRaw": 1250, "startWateringPercent": 30,
                "stopWateringPercent": 63, "pulseMs": 8000, "soakMs": 90000,
                "maxOnMs": 60000,
            })
        return zones

    async def snapshot(self, socket, request_id=None):
        self.sequence += 1
        message = {
            "type": "debug.snapshot", "schemaVersion": 1, "deviceId": DEVICE,
            "bootId": self.boot, "sequence": self.sequence, "uptimeMs": self.uptime(),
            "configRevision": 1, "session": self.session(),
            "hardware": {"board": "esp32dev", "relayPins": [27, 26, 25, 33],
                         "sensorPins": [34, 35, 36, 39], "tankPin": 23,
                         "relayActiveHigh": True,
                         "oled": {"sda": 21, "scl": 22, "address": 60, "available": True}},
            "tank": {"physicalLow": False, "effectiveLow": bool(self.tank_override),
                     "low": bool(self.tank_override), "pendingLow": bool(self.tank_override),
                     "debounceRemainingMs": 0, "overrideLow": self.tank_override},
            "zones": self.zones(),
        }
        if request_id:
            message["requestId"] = request_id
        await socket.send(json.dumps(message))

    async def telemetry(self, socket):
        self.telemetry_sequence += 1
        await socket.send(json.dumps({
            "type": "telemetry", "schemaVersion": 1, "deviceId": DEVICE,
            "bootId": self.boot, "sequence": self.telemetry_sequence,
            "uptimeMs": self.uptime(), "configRevision": 1, "wifiRssi": -52,
            "mainTankLow": bool(self.tank_override), "diagnosticsVersion": 1,
            "debug": self.session(), "simulation": self.mode == "simulation", "zones": self.zones(),
        }))

    async def event(self, socket, command, status, result=None):
        await socket.send(json.dumps({
            "type": "debug.action", "schemaVersion": 1, "deviceId": DEVICE,
            "bootId": self.boot, "sessionId": command.get("sessionId"),
            "requestId": command["requestId"], "command": command["type"],
            "status": status, "uptimeMs": self.uptime(),
            "zoneId": command.get("zoneId"), "message": f"Fixture {status}",
            "result": result,
        }))

    async def finish_pulse(self, socket, command):
        await asyncio.sleep(command["durationMs"] / 1000)
        zone_id = command["zoneId"]
        if zone_id in self.pulses and self.pulses[zone_id][1] == command["requestId"]:
            self.pulses.pop(zone_id, None)
            await self.event(socket, command, "completed")
            await self.snapshot(socket)

    async def commands(self, socket):
        async for raw in socket:
            command = json.loads(raw)
            kind = command["type"]
            if kind in {"device.ready", "telegram.delivery.ack"}:
                continue
            if kind == "config.get":
                await socket.send(json.dumps({
                    "type": "config.snapshot", "deviceId": DEVICE,
                    "requestId": command["requestId"], "revision": 1,
                    "config": {"zones": [{"id": i, "startWateringPercent": 30,
                                           "stopWateringPercent": 63} for i in range(1, 5)]},
                }))
                continue
            if kind == "debug.snapshot.get":
                await self.snapshot(socket, command["requestId"])
                continue
            if kind == "debug.session.start":
                self.mode = command["mode"]
                self.session_id = command["sessionId"]
                self.owner = command["owner"]
                self.held = True
                self.lease_until = time.monotonic() + 30
            elif kind == "debug.session.renew":
                self.lease_until = time.monotonic() + 30
            elif kind == "debug.session.end":
                self.mode, self.session_id, self.owner = "observe", None, None
                self.pulses.clear()
                self.overrides.clear()
                self.tank_override = None
                self.lease_until = 0
            elif kind == "debug.automation.resume":
                self.held = False
            elif kind == "debug.input.set":
                if command["input"] == "soil":
                    self.overrides[command["zoneId"]] = command["raw"]
                else:
                    self.tank_override = command["low"]
            elif kind == "debug.input.clear":
                self.overrides.clear()
                self.tank_override = None
            elif kind == "debug.output.pulse":
                self.pulses[command["zoneId"]] = (time.monotonic() + command["durationMs"] / 1000, command["requestId"])
                await self.event(socket, command, "started")
                task = asyncio.create_task(self.finish_pulse(socket, command))
                self.tasks.add(task)
                task.add_done_callback(self.tasks.discard)
            elif kind == "debug.output.stop":
                self.pulses.clear()
                await self.event(socket, command, "completed")
            elif kind in {"debug.sample", "debug.oled.test", "debug.i2c.scan"}:
                await self.event(socket, command, "completed", {"addresses": [60]} if kind == "debug.i2c.scan" else None)
            else:
                raise AssertionError(f"Unexpected fixture command: {kind}")
            await socket.send(json.dumps({
                "type": "command.ack", "schemaVersion": 1, "deviceId": DEVICE,
                "requestId": command["requestId"], "command": kind,
                "status": "accepted", "message": "Fixture accepted",
            }))
            await self.snapshot(socket)

    async def reports(self, socket):
        while True:
            if self.session_id and time.monotonic() > self.lease_until:
                self.mode, self.session_id, self.owner = "observe", None, None
                self.overrides.clear()
                self.tank_override = None
                self.pulses.clear()
            await self.snapshot(socket)
            await self.telemetry(socket)
            await asyncio.sleep(1)


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:18083")
    parser.add_argument("--hold", action="store_true")
    args = parser.parse_args()
    base = args.base_url.rstrip("/")
    if urlparse(base).hostname not in {"localhost", "127.0.0.1", "::1"}:
        raise SystemExit("Only an isolated loopback development backend is permitted")
    async with httpx.AsyncClient(base_url=base, trust_env=False) as client:
        health = (await client.get("/health")).json()
        assert health["authMode"] == "development", health
        login = await client.get("/auth/google/login")
        cookie = login.cookies.get("watering_session")
        assert cookie and login.status_code == 302
        client.headers["Cookie"] = "watering_session=" + cookie
        assert (await client.get("/api/me")).json()["email"] == "sahakyan.raphayel@gmail.com"
        fixture = FixtureController()
        ws_base = base.replace("http://", "ws://", 1)
        async with websockets.connect(ws_base + "/ws/device", additional_headers={"Authorization": "Bearer " + os.environ["DEVICE_SHARED_TOKEN"]}) as socket:
            await socket.send(json.dumps({
                "type": "device.hello", "schemaVersion": 1, "deviceId": DEVICE,
                "firmwareVersion": "0.8.0", "bootId": fixture.boot, "configRevision": 1,
                "automaticWateringEnabled": True, "uptimeMs": 0,
                "diagnosticsVersion": 1, "debug": fixture.session(),
            }))
            command_task = asyncio.create_task(fixture.commands(socket))
            report_task = asyncio.create_task(fixture.reports(socket))
            try:
                path = f"/api/devices/{DEVICE}"
                for _ in range(30):
                    response = await client.get(path + "/diagnostics")
                    if response.status_code == 200 and response.json().get("supported"):
                        break
                    await asyncio.sleep(0.1)
                assert response.status_code == 200, response.text
                data = response.json()
                assert data["canControl"] and data["supported"] and len(data["snapshot"]["zones"]) == 4, data
                before = (await client.get(path + "/latest")).json()["zones"][0]["raw"]
                start = await client.post(path + "/debug/session", json={"mode": "simulation", "expectedBootId": fixture.boot})
                assert start.status_code == 200, start.text
                session_id = start.json()["sessionId"]
                action = {"sessionId": session_id, "expectedBootId": fixture.boot}
                bad = await client.post(path + "/debug/actions", json={**action, "command": "debug.output.pulse", "parameters": {"zoneId": 1, "durationMs": 60001}})
                assert bad.status_code == 422, bad.text
                inject = await client.post(path + "/debug/actions", json={**action, "command": "debug.input.set", "parameters": {"input": "soil", "zoneId": 1, "raw": 3000}})
                assert inject.status_code == 200, inject.text
                pulse = await client.post(path + "/debug/actions", json={**action, "command": "debug.output.pulse", "parameters": {"zoneId": 1, "durationMs": 100}})
                assert pulse.status_code == 200, pulse.text
                await asyncio.sleep(1.2)
                state = (await client.get(path + "/diagnostics")).json()
                assert state["snapshot"]["zones"][0]["raw"] == 3000, state
                assert not any(z["relayOn"] for z in state["snapshot"]["zones"])
                ordinary = (await client.get(path + "/latest")).json()
                assert ordinary["zones"][0]["raw"] == before, ordinary
                capture = await client.post(path + "/debug/captures", json={"name": "Isolated integration trace"})
                assert capture.status_code in {200, 201}, capture.text
                audit = await client.get(path + "/debug/actions")
                assert audit.status_code == 200 and audit.json(), audit.text
                end = await client.post(path + "/debug/session/end", json=action)
                assert end.status_code == 200, end.text
                state = (await client.get(path + "/diagnostics")).json()
                assert state["snapshot"]["session"]["automationHeld"] is True
                resume = await client.post(path + "/debug/actions", json={"command": "debug.automation.resume", "expectedBootId": fixture.boot, "parameters": {}})
                assert resume.status_code == 200, resume.text
                print("Passed: real HTTP/WebSocket protocol, online session/actions, validation, virtual pulse, simulation history isolation, capture/audit, durable hold and resume.", flush=True)
                if args.hold:
                    print("Isolated preview controller online; stop this process to disconnect it.", flush=True)
                    await asyncio.gather(command_task, report_task)
            finally:
                pending = [command_task, report_task, *fixture.tasks]
                for task in pending:
                    task.cancel()
                for task in pending:
                    with contextlib.suppress(asyncio.CancelledError):
                        await task


if __name__ == "__main__":
    asyncio.run(main())
