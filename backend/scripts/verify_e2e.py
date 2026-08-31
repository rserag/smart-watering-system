#!/usr/bin/env python3
import argparse
import asyncio
import json
import os
import ssl
import uuid
from datetime import datetime, timedelta, timezone

import httpx
import websockets


BOOT_ID = f"e2e-{uuid.uuid4().hex}"


def zone(zone_id: int, relay_on: bool, moisture: float) -> dict:
    return {
        "id": zone_id,
        "raw": 2400 + zone_id,
        "filteredRaw": 2390 + zone_id,
        "relativeMoisturePercent": moisture,
        "sensorValid": True,
        "phase": "pulse" if relay_on else "idle",
        "relayOn": relay_on,
        "wateringOnMsThisCycle": 1000 if relay_on else 2000,
        "fault": None,
    }


def telemetry(sequence: int, relay_on: bool, moisture: float) -> dict:
    return {
        "type": "telemetry",
        "schemaVersion": 1,
        "deviceId": "e2e-dashboard-device",
        "bootId": BOOT_ID,
        "sequence": sequence,
        "uptimeMs": sequence * 1000,
        "configRevision": 7,
        "wifiRssi": -58,
        "mainTankLow": False,
        "directTelegram": True,
        "telegramDebugEnabled": True,
        "telegramConfigured": True,
        "telegramPendingMessages": 0,
        "telegramLastSendSucceeded": True,
        "telegramWorkerRunning": True,
        "telegramTimeReady": True,
        "telegramLastFailureStage": None,
        "zones": [zone(index, relay_on if index == 1 else False, moisture + index) for index in range(1, 5)],
    }


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=8443)
    parser.add_argument("--ca", default="certs/ca.crt")
    args = parser.parse_args()
    base = f"https://{args.host}:{args.port}"
    wss = f"wss://{args.host}:{args.port}"
    ssl_context = ssl.create_default_context(cafile=args.ca)
    device_token = os.environ["DEVICE_SHARED_TOKEN"]

    async with httpx.AsyncClient(verify=args.ca, follow_redirects=False, trust_env=False) as client:
        login = await client.get(f"{base}/auth/google/login")
        assert login.status_code == 302, login.text
        identity = (await client.get(f"{base}/api/me")).json()
        assert identity["authenticated"] is True, identity
        cookie_header = "; ".join(f"{cookie.name}={cookie.value}" for cookie in client.cookies.jar)

        try:
            async with websockets.connect(
                f"{wss}/ws/device",
                ssl=ssl_context,
                additional_headers={"Authorization": "Bearer wrong-token"},
            ) as rejected:
                await rejected.recv()
            raise AssertionError("Invalid device token was accepted")
        except websockets.exceptions.ConnectionClosedError as exc:
            assert exc.code == 4001, exc

        async with websockets.connect(
            f"{wss}/ws/dashboard",
            ssl=ssl_context,
            additional_headers={"Cookie": cookie_header},
        ) as dashboard:
            initial = json.loads(await dashboard.recv())
            assert initial["type"] == "snapshot", initial

            async with websockets.connect(
                f"{wss}/ws/device",
                ssl=ssl_context,
                additional_headers={"Authorization": f"Bearer {device_token}"},
            ) as device:
                await device.send(
                    json.dumps(
                        {
                            "type": "device.hello",
                            "schemaVersion": 1,
                            "deviceId": "e2e-dashboard-device",
                            "firmwareVersion": "e2e/0.2.0",
                            "bootId": BOOT_ID,
                            "configRevision": 7,
                            "automaticWateringEnabled": True,
                            "directTelegram": True,
                            "telegramDebugEnabled": True,
                            "telegramConfigured": True,
                            "telegramWorkerRunning": True,
                            "telegramTimeReady": True,
                            "uptimeMs": 100,
                        }
                    )
                )
                ready = json.loads(await device.recv())
                assert ready["type"] == "device.ready", ready
                status = json.loads(await dashboard.recv())
                assert status["type"] == "device.status", status

                await device.send(json.dumps(telemetry(1, True, 37.0)))
                first_live = json.loads(await dashboard.recv())
                assert first_live["type"] == "telemetry", first_live
                await asyncio.sleep(0.03)
                await device.send(json.dumps(telemetry(2, False, 43.0)))
                second_live = json.loads(await dashboard.recv())
                assert second_live["device"]["zones"][0]["moisturePercent"] == 44.0, second_live

                command_response = await client.post(
                    f"{base}/api/devices/e2e-dashboard-device/commands",
                    json={"command": "telemetry.request", "parameters": {}},
                )
                assert command_response.status_code == 202, command_response.text
                command = json.loads(await device.recv())
                assert command["type"] == "telemetry.request", command
                await device.send(
                    json.dumps(
                        {
                            "type": "command.ack",
                            "schemaVersion": 1,
                            "deviceId": "e2e-dashboard-device",
                            "requestId": command["requestId"],
                            "command": command["type"],
                            "status": "accepted",
                        }
                    )
                )

                telegram_response = await client.post(
                    f"{base}/api/devices/e2e-dashboard-device/telegram/debug"
                )
                assert telegram_response.status_code == 202, telegram_response.text
                telegram_command = json.loads(await device.recv())
                assert telegram_command["type"] == "telegram.debug.send", telegram_command
                await device.send(
                    json.dumps(
                        {
                            "type": "command.ack",
                            "schemaVersion": 1,
                            "deviceId": "e2e-dashboard-device",
                            "requestId": telegram_command["requestId"],
                            "command": telegram_command["type"],
                            "status": "accepted",
                        }
                    )
                )
                delivery_event_id = f"{BOOT_ID}:telegram:1"
                for update_sequence, status in ((1, "queued"), (2, "sent")):
                    await device.send(
                        json.dumps(
                            {
                                "type": "telegram.delivery",
                                "schemaVersion": 1,
                                "deviceId": "e2e-dashboard-device",
                                "eventId": delivery_event_id,
                                "requestId": telegram_command["requestId"],
                                "updateSequence": update_sequence,
                                "kind": "manual_debug",
                                "status": status,
                                "attempt": 0 if status == "queued" else 1,
                                "uptimeMs": 3000 + update_sequence,
                                "pendingCount": 0,
                                "httpStatus": 200 if status == "sent" else None,
                                "telegramMessageId": 42 if status == "sent" else None,
                            }
                        )
                    )
                    delivery_ack = json.loads(await device.recv())
                    assert delivery_ack["type"] == "telegram.delivery.ack", delivery_ack
                    live_delivery = json.loads(await dashboard.recv())
                    assert live_delivery["type"] == "telegram.delivery", live_delivery
                    assert live_delivery["delivery"]["status"] == status, live_delivery

            disconnected = json.loads(await dashboard.recv())
            assert disconnected["type"] == "device.status", disconnected

        now = datetime.now(timezone.utc)
        params = {
            "from": (now - timedelta(hours=1)).isoformat(),
            "to": (now + timedelta(minutes=1)).isoformat(),
            "zone_id": 1,
            "metric": "moisturePercent",
            "bucket_seconds": 10,
        }
        history = await client.get(f"{base}/api/devices/e2e-dashboard-device/history", params=params)
        assert history.status_code == 200 and history.json()["points"], history.text
        events = await client.get(
            f"{base}/api/devices/e2e-dashboard-device/events",
            params={"from": params["from"], "to": params["to"], "zone_id": 1},
        )
        assert events.status_code == 200 and events.json()[0]["status"] == "completed", events.text
        export = await client.get(
            f"{base}/api/devices/e2e-dashboard-device/history.csv",
            params={"from": params["from"], "to": params["to"], "zone_id": 1},
        )
        assert export.status_code == 200 and "moisture_percent" in export.text, export.text
        deliveries = await client.get(
            f"{base}/api/devices/e2e-dashboard-device/telegram/deliveries"
        )
        assert deliveries.status_code == 200, deliveries.text
        assert deliveries.json()[0]["status"] == "sent", deliveries.text
        assert deliveries.json()[0]["telegramMessageId"] == 42, deliveries.text

    print(
        json.dumps(
            {
                "development_login": "ok",
                "invalid_device_auth": "rejected",
                "firmware_protocol": "ok",
                "dashboard_live_updates": "ok",
                "historical_bucketing": "ok",
                "watering_events": "ok",
                "csv_export": "ok",
                "command_round_trip": "ok",
                "telegram_debug_command": "ok",
                "telegram_delivery_history": "ok",
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    asyncio.run(main())
