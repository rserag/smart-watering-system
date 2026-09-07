"""Exercise zone labels against an isolated, local development backend."""
import argparse
import asyncio
import json
import os
from contextlib import AsyncExitStack
from urllib.parse import urlparse
from uuid import uuid4

import httpx
import websockets


def reading(device_id, boot_id, sequence):
    return {
        "type": "telemetry", "schemaVersion": 1, "deviceId": device_id,
        "bootId": boot_id, "sequence": sequence, "uptimeMs": sequence * 5000,
        "configRevision": 2, "wifiRssi": -67, "mainTankLow": False,
        "zones": [{
            "id": index, "raw": 2400, "filteredRaw": 2380,
            "relativeMoisturePercent": value, "sensorValid": True,
            "phase": "disabled" if index == 4 else "monitoring",
            "relayOn": False, "wateringOnMsThisCycle": 0, "fault": None,
        } for index, value in enumerate([41, 98, 53, 36], 1)],
    }


async def receive_until(socket, kind, device_id=None):
    async def receive():
        while True:
            message = json.loads(await socket.recv())
            found_id = message.get("deviceId", message.get("device", {}).get("id"))
            if message["type"] == kind and (device_id is None or device_id == found_id):
                return message
    return await asyncio.wait_for(receive(), 10)


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:18080")
    parser.add_argument("--hold", action="store_true", help="Keep the isolated preview controller online after checks")
    args = parser.parse_args()
    base = args.base_url.rstrip("/")
    if urlparse(base).hostname not in {"localhost", "127.0.0.1", "::1"}:
        raise SystemExit("Use an isolated local development backend; remote hosts are not permitted.")
    token = os.environ["DEVICE_SHARED_TOKEN"]
    ws_base = base.replace("http://", "ws://", 1).replace("https://", "wss://", 1)
    boot = f"name-test-{uuid4().hex}"
    device_ids = ["preview-garden", "preview-second-controller"]
    async with httpx.AsyncClient(base_url=base, follow_redirects=False, trust_env=False) as client, AsyncExitStack() as stack:
        if (await client.get("/health")).json()["authMode"] != "development":
            raise SystemExit("The backend must be an isolated development instance.")
        login = await client.get("/auth/google/login")
        assert login.status_code == 302
        cookie = "watering_session=" + login.cookies["watering_session"]
        client.headers["Cookie"] = cookie
        dashboard = await stack.enter_async_context(websockets.connect(ws_base + "/ws/dashboard", additional_headers={"Cookie": cookie}))
        await receive_until(dashboard, "snapshot")
        sockets = []
        for device_id in device_ids:
            socket = await stack.enter_async_context(websockets.connect(ws_base + "/ws/device", additional_headers={"Authorization": "Bearer " + token}))
            sockets.append(socket)
            await socket.send(json.dumps({
                "type": "device.hello", "schemaVersion": 1, "deviceId": device_id,
                "firmwareVersion": "0.5.1", "bootId": boot, "configRevision": 2,
                "automaticWateringEnabled": True, "uptimeMs": 0,
            }))
            await receive_until(socket, "device.ready")
            await socket.send(json.dumps(reading(device_id, boot, 1)))
            await receive_until(dashboard, "telemetry", device_id)
        path = "/api/devices/preview-garden/zones/1"
        async with httpx.AsyncClient(base_url=base, trust_env=False) as anonymous:
            assert (await anonymous.patch(path, json={"name": "Forbidden"})).status_code == 401
        assert (await client.patch(path, json={"name": "  Լոլիկ 🍅  "})).json() == {"id": 1, "name": "Լոլիկ 🍅"}
        event = await receive_until(dashboard, "zone.updated", device_ids[0])
        assert event["zone"]["name"] == "Լոլիկ 🍅"
        for invalid in [{"name": "x" * 61}, {"name": "bad\x00name"}, {"name": "Plants", "relayOn": True}]:
            assert (await client.patch(path, json=invalid)).status_code == 422
        assert (await client.patch("/api/devices/preview-garden/zones/16", json={"name": "Unknown"})).status_code == 404
        assert (await client.patch("/api/devices/no-such-controller/zones/1", json={"name": "Unknown"})).status_code == 404
        await sockets[0].send(json.dumps(reading(device_ids[0], boot, 2)))
        updated = await receive_until(dashboard, "telemetry", device_ids[0])
        assert updated["device"]["zones"][0]["name"] == "Լոլիկ 🍅"
        snapshots = (await client.get("/api/devices")).json()
        other = next(device for device in snapshots if device["id"] == device_ids[1])
        assert other["zones"][0]["name"] is None
        async with websockets.connect(ws_base + "/ws/dashboard", additional_headers={"Cookie": cookie}) as second_browser:
            snapshot = await receive_until(second_browser, "snapshot")
            assert next(device for device in snapshot["devices"] if device["id"] == device_ids[0])["zones"][0]["name"] == "Լոլիկ 🍅"
        assert (await client.patch(path, json={"name": " "})).json()["name"] is None
        assert (await client.get("/api/devices/preview-garden/latest")).json()["zones"][0]["name"] is None
        await client.patch(path, json={"name": "Tomatoes"})
        print("Passed: authenticated saves, Unicode/validation, reset, device isolation, persisted reads, telemetry updates, and a second dashboard session.", flush=True)
        if args.hold:
            print("Isolated preview controller is online. Stop this process to disconnect it.", flush=True)
            sequence = 2
            while True:
                await asyncio.sleep(5)
                sequence += 1
                for device_id, socket in zip(device_ids, sockets):
                    await socket.send(json.dumps(reading(device_id, boot, sequence)))


if __name__ == "__main__":
    asyncio.run(main())
