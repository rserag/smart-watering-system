import json
import logging
from threading import Event
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest
from fastapi import WebSocketDisconnect
from fastapi.testclient import TestClient

from app import main
from app.hubs import DeviceHub


DEVICE = "closure-test"
TOKEN = "private-test-device-token"
PRIVATE_REASON = "private-close-reason"
SESSION_ID = "09cdb4a0-8194-4c64-8a28-de988c832493"
COOKIE = "private-test-cookie"
MALFORMED = json.dumps({"sessionId": SESSION_ID, "token": TOKEN, "cookie": COOKIE})[:-1]
HELLO = {"type": "device.hello", "schemaVersion": 1, "deviceId": DEVICE,
         "firmwareVersion": "0.8.1", "bootId": "closure-test-boot",
         "configRevision": 1, "automaticWateringEnabled": False, "uptimeMs": 1}


@pytest.fixture
def websocket_client(monkeypatch):
    # Exercise the real ASGI WebSocket receive/JSON decoding and cleanup paths;
    # only persistence and dashboard broadcasts are replaced.
    session = AsyncMock()
    session.__aenter__.return_value = session
    session.get.return_value = None
    monkeypatch.setattr(main, "SessionLocal", lambda: session)
    monkeypatch.setattr(main, "record_hello", AsyncMock())
    monkeypatch.setattr(main, "deliver_queued_commands", AsyncMock())
    monkeypatch.setattr(main, "latest_snapshot", AsyncMock(return_value={}))
    monkeypatch.setattr(main.dashboard_hub, "broadcast", AsyncMock())
    monkeypatch.setattr(main.settings, "device_shared_token", TOKEN)
    hub = DeviceHub()
    disconnected = Event()
    unregister = hub.unregister

    async def tracked_unregister(device_id, websocket):
        await unregister(device_id, websocket)
        disconnected.set()

    hub.unregister = AsyncMock(side_effect=tracked_unregister)
    monkeypatch.setattr(main, "device_hub", hub)
    http = TestClient(main.app)
    try:
        yield SimpleNamespace(http=http, hub=hub, disconnected=disconnected)
    finally:
        http.close()


def connect(client):
    return client.http.websocket_connect("/ws/device", headers={
        "authorization": f"Bearer {TOKEN}", "cookie": f"session={COOKIE}"})


def hello(websocket):
    websocket.send_json(HELLO)
    assert websocket.receive_json()["type"] == "device.ready"
    assert websocket.receive_json()["type"] == "config.get"


def assert_private_values_absent(logs):
    for value in (TOKEN, SESSION_ID, COOKIE, PRIVATE_REASON, MALFORMED):
        assert value not in logs


def test_malformed_json_logs_metadata_and_unregisters(websocket_client, caplog):
    with caplog.at_level(logging.INFO, logger="watering-server"):
        with connect(websocket_client) as websocket:
            hello(websocket)
            websocket.send_text(MALFORMED)
            assert websocket_client.disconnected.wait(2)
    logs = caplog.text
    assert f"device JSON parsing error device_id={DEVICE} error_type=JSONDecodeError position={len(MALFORMED)}" in logs
    assert "device WebSocket closed" not in logs
    assert_private_values_absent(logs)
    websocket_client.hub.unregister.assert_awaited_once()
    assert not websocket_client.hub._connections


def test_disconnect_logs_close_code_without_reason(websocket_client, caplog):
    with caplog.at_level(logging.INFO, logger="watering-server"):
        with connect(websocket_client) as websocket:
            hello(websocket)
            websocket.close(code=1011, reason=PRIVATE_REASON)
            assert websocket_client.disconnected.wait(2)
    logs = caplog.text
    assert f"device WebSocket closed device_id={DEVICE} close_code=1011" in logs
    assert "device JSON parsing error" not in logs
    assert_private_values_absent(logs)
    websocket_client.hub.unregister.assert_awaited_once()
    assert not websocket_client.hub._connections


def test_malformed_hello_logs_metadata_and_preserves_rejection(websocket_client, caplog):
    with caplog.at_level(logging.INFO, logger="watering-server"):
        with connect(websocket_client) as websocket:
            websocket.send_text(MALFORMED)
            with pytest.raises(WebSocketDisconnect) as rejected:
                websocket.receive_json()
    assert rejected.value.code == 4000
    logs = caplog.text
    assert f"device JSON parsing error device_id=None error_type=JSONDecodeError position={len(MALFORMED)}" in logs
    assert "device WebSocket closed" not in logs
    assert_private_values_absent(logs)
    websocket_client.hub.unregister.assert_not_awaited()
