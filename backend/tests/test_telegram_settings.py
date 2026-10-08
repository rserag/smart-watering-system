import asyncio
from datetime import datetime, timezone
from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock

import pytest
from fastapi.testclient import TestClient

from app.auth import require_user
from app.database import get_session
from app.hubs import device_hub
from app.main import app, latest_snapshot
from app.models import Device
from app.telegram_settings import supports_hourly_silent


URL = "/api/devices/garden/telegram/hourly-silent"


@pytest.fixture
def client(monkeypatch):
    session = AsyncMock()
    session.get.return_value = Device(id="garden", firmware_version="0.6.0")
    ack = {"type": "command.ack", "schemaVersion": 1, "deviceId": "garden",
           "requestId": "request-1", "command": "telegram.hourlySilent.set", "status": "accepted"}
    request = AsyncMock(return_value=ack)
    monkeypatch.setattr(device_hub, "request", request)

    async def dependency():
        yield session

    app.dependency_overrides[get_session] = dependency
    app.dependency_overrides[require_user] = lambda: object()
    http = TestClient(app)
    try:
        yield http, session, request, ack
    finally:
        http.close()
        app.dependency_overrides.clear()


@pytest.mark.parametrize("enabled", [True, False])
def test_save_waits_for_device_acceptance_and_changes_only_sound(client, enabled):
    http, _, request, _ = client
    response = http.patch(URL, json={"enabled": enabled})
    assert response.status_code == 200
    assert response.json() == {"enabled": enabled}
    request.assert_awaited_once_with(
        "garden", {"type": "telegram.hourlySilent.set", "enabled": enabled}, "command.ack",
    )


@pytest.mark.parametrize("payload", [{}, {"enabled": "true"}, {"enabled": 1},
                                      {"enabled": None}, {"enabled": True, "debugEnabled": True}])
def test_invalid_values_never_reach_device(client, payload):
    http, _, request, _ = client
    assert http.patch(URL, json=payload).status_code == 422
    request.assert_not_awaited()


@pytest.mark.parametrize("firmware", [None, "development", "0.5.1"])
def test_older_or_unknown_firmware_is_rejected(client, firmware):
    http, session, request, _ = client
    session.get.return_value.firmware_version = firmware
    assert http.patch(URL, json={"enabled": True}).status_code == 409
    request.assert_not_awaited()


@pytest.mark.parametrize("status", ["rejected", "duplicate"])
def test_unsaved_changes_are_not_reported_as_success(client, status):
    http, _, _, ack = client
    ack.update(status=status, message="Could not persist setting")
    response = http.patch(URL, json={"enabled": True})
    assert response.status_code == 409
    assert response.json()["detail"] == "Could not persist setting"


@pytest.mark.parametrize("failure,status", [(ConnectionError(), 503), (TimeoutError(), 504)])
def test_disconnects_and_timeout_require_checking_current_state(client, failure, status):
    http, _, request, _ = client
    request.side_effect = failure
    response = http.patch(URL, json={"enabled": True})
    assert response.status_code == status
    assert "could not be confirmed" in response.json()["detail"]


@pytest.mark.parametrize("field,value", [("command", "telegram.debug.set"), ("deviceId", "other"), ("status", "invalid")])
def test_wrong_or_malformed_confirmation_is_rejected(client, field, value):
    http, _, _, ack = client
    ack[field] = value
    assert http.patch(URL, json={"enabled": True}).status_code == 409


def test_authentication_and_unknown_device(client):
    http, session, request, _ = client
    session.get.return_value = None
    assert http.patch(URL, json={"enabled": True}).status_code == 404
    del app.dependency_overrides[require_user]
    assert http.patch(URL, json={"enabled": True}).status_code == 401
    request.assert_not_awaited()


@pytest.mark.parametrize("firmware,supported", [("0.6.0", True), ("0.6.1", True), ("1.0.0", True),
                                               ("0.5.99", False), ("0.6", False), ("0.6.-1", False)])
def test_firmware_support(firmware, supported):
    assert supports_hourly_silent(firmware) is supported


@pytest.mark.parametrize("payload,expected", [(None, False), ({}, False),
                                           ({"telegramHourlySilent": True}, True),
                                           ({"telegramHourlySilent": False}, False)])
def test_dashboard_uses_device_report_and_defaults_legacy_devices_to_sound(payload, expected):
    session = AsyncMock()
    latest = MagicMock()
    latest.scalar_one_or_none.return_value = None if payload is None else SimpleNamespace(id=1, wifi_rssi=-60, payload=payload)
    empty = MagicMock()
    empty.scalars.return_value = []
    session.execute.side_effect = [latest, empty, empty]
    session.get.return_value = None
    device = Device(id="garden", last_seen_at=datetime.now(timezone.utc))
    assert asyncio.run(latest_snapshot(session, device))["telegramHourlySilent"] is expected
