import asyncio
from copy import deepcopy
from unittest.mock import AsyncMock

import pytest
from fastapi.testclient import TestClient

from app.auth import require_user
from app.database import get_session
from app.hubs import DeviceHub, device_hub
from app.main import app
from app.models import Device


def configuration():
    return {"type": "config.snapshot", "schemaVersion": 1, "deviceId": "garden", "revision": 2,
            "config": {"automaticWateringEnabled": True, "sampleIntervalMs": 5000,
                       "telemetryIntervalMs": 5000, "maxConcurrentZones": 1,
                       "zones": [{"id": i, "enabled": True, "dryRaw": 2600, "wetRaw": 1250,
                                  "startWateringPercent": 30, "stopWateringPercent": 63,
                                  "dryConfirmationSamples": 3, "wetConfirmationSamples": 2,
                                  "pulseOnMs": 8000, "soakMs": 90000,
                                  "maxWateringOnMsPerCycle": 45000, "cooldownMs": 1800000}
                                 for i in range(1, 5)]}}


@pytest.fixture
def client(monkeypatch):
    session = AsyncMock()
    session.get.return_value = Device(id="garden")
    state = {"snapshot": configuration(), "writes": [], "reject": False, "fail": None, "after_write": None, "verification_revision": None}

    async def request(device_id, message, reply_type):
        assert device_id == "garden"
        if state["fail"]:
            raise state["fail"]
        if message["type"] == "config.get":
            assert reply_type == "config.snapshot"
            if state["writes"] and state["verification_revision"]:
                state["snapshot"]["revision"] = state["verification_revision"]
            return deepcopy(state["snapshot"])
        assert reply_type == "config.ack"
        state["writes"].append(deepcopy(message))
        if not state["reject"]:
            state["snapshot"].update(revision=message["revision"], config=deepcopy(message["config"]))
        state["fail"] = state["after_write"]
        return {"type": "config.ack", "schemaVersion": 1, "deviceId": device_id, "requestId": "test",
                "revision": message["revision"], "status": "rejected" if state["reject"] else "applied",
                "message": "Device rejected settings" if state["reject"] else None}

    async def dependency():
        yield session

    app.dependency_overrides[get_session] = dependency
    app.dependency_overrides[require_user] = lambda: object()
    monkeypatch.setattr(device_hub, "request", AsyncMock(side_effect=request))
    http = TestClient(app)
    try:
        yield http, state, session
    finally:
        http.close()
        app.dependency_overrides.clear()


URL = "/api/devices/garden/zones/2/pulse"


def test_read_returns_actual_pulse_and_zone_limit(client):
    http, state, _ = client
    response = http.get(URL)
    assert response.status_code == 200
    assert response.json() == {"zoneId": 2, "revision": 2, "pulseOnMs": 8000, "soakMs": 90000, "maxPulseOnMs": 45000}
    assert not state["writes"]


def test_save_preserves_all_other_settings_and_verifies_device(client):
    http, state, _ = client
    state["snapshot"]["config"]["futureSetting"] = {"keep": True}
    state["snapshot"]["config"]["zones"][3]["futureZoneSetting"] = 7
    original = deepcopy(state["snapshot"]["config"])
    response = http.patch(URL, json={"pulseOnMs": 12500, "expectedRevision": 2})
    assert response.status_code == 200
    assert response.json()["pulseOnMs"] == 12500
    assert response.json()["revision"] == 3
    original["zones"][1]["pulseOnMs"] = 12500
    assert state["writes"] == [{"type": "config.set", "revision": 3, "config": original}]
    assert device_hub.request.await_count == 3


@pytest.mark.parametrize("payload,status", [
    ({"pulseOnMs": 10000, "expectedRevision": 1}, 409),
    ({"pulseOnMs": 45001, "expectedRevision": 2}, 422),
    ({"pulseOnMs": 999, "expectedRevision": 2}, 422),
    ({"pulseOnMs": 60001, "expectedRevision": 2}, 422),
    ({"pulseOnMs": 8000.5, "expectedRevision": 2}, 422),
    ({"pulseOnMs": "10000", "expectedRevision": 2}, 422),
    ({"pulseOnMs": 10000, "expectedRevision": 2, "soakMs": 10000}, 422),
])
def test_invalid_or_stale_changes_never_reach_controller(client, payload, status):
    http, state, _ = client
    assert http.patch(URL, json=payload).status_code == status
    assert not state["writes"]


def test_unchanged_value_does_not_interrupt_watering(client):
    http, state, _ = client
    assert http.patch(URL, json={"pulseOnMs": 8000, "expectedRevision": 2}).status_code == 200
    assert not state["writes"]


def test_incomplete_config_cannot_be_replaced(client):
    http, state, _ = client
    del state["snapshot"]["config"]["zones"][0]["dryRaw"]
    assert http.patch(URL, json={"pulseOnMs": 10000, "expectedRevision": 2}).status_code == 409
    assert not state["writes"]


def test_device_rejection_is_not_reported_as_saved(client):
    http, state, _ = client
    state["reject"] = True
    response = http.patch(URL, json={"pulseOnMs": 10000, "expectedRevision": 2})
    assert response.status_code == 409
    assert response.json()["detail"] == "Device rejected settings"


@pytest.mark.parametrize("failure,status", [(ConnectionError(), 503), (TimeoutError(), 504)])
def test_offline_or_timeout_never_queues_a_change(client, failure, status):
    http, state, _ = client
    state["fail"] = failure
    assert http.patch(URL, json={"pulseOnMs": 10000, "expectedRevision": 2}).status_code == status
    assert not state["writes"]


def test_missing_verification_requires_reload_without_resending(client):
    http, state, _ = client
    state["after_write"] = TimeoutError()
    response = http.patch(URL, json={"pulseOnMs": 10000, "expectedRevision": 2})
    assert response.status_code == 504
    assert "Save could not be confirmed" in response.json()["detail"]
    assert len(state["writes"]) == 1


def test_another_edit_during_verification_is_reported_as_a_conflict(client):
    http, state, _ = client
    state["verification_revision"] = 4
    response = http.patch(URL, json={"pulseOnMs": 10000, "expectedRevision": 2})
    assert response.status_code == 409
    assert "Settings changed during verification" in response.json()["detail"]
    assert len(state["writes"]) == 1


def test_authentication_and_device_existence(client):
    http, state, session = client
    session.get.return_value = None
    assert http.get(URL).status_code == 404
    del app.dependency_overrides[require_user]
    assert http.get(URL).status_code == 401
    assert http.patch(URL, json={"pulseOnMs": 10000, "expectedRevision": 2}).status_code == 401
    assert not state["writes"]


def test_request_correlates_connection_type_and_id_and_cleans_up():
    async def run():
        hub = DeviceHub()
        socket = AsyncMock()
        await hub.register("garden", socket)

        async def reply(message):
            wrong_type = {**message, "type": "config.ack"}
            hub.resolve_reply(socket, wrong_type)
            assert not hub._pending[message["requestId"]][2].done()
            good = {**message, "type": "config.snapshot"}
            hub.resolve_reply(AsyncMock(), good)
            assert not hub._pending[message["requestId"]][2].done()
            hub.resolve_reply(socket, good)

        socket.send_json.side_effect = reply
        result = await hub.request("garden", {"type": "config.get"}, "config.snapshot")
        assert result["deviceId"] == "garden"
        assert not hub._pending
        socket.send_json.side_effect = None
        with pytest.raises(TimeoutError):
            await hub.request("garden", {"type": "config.get"}, "config.snapshot", timeout=.001)
        assert not hub._pending

        async def disconnect(_):
            await hub.unregister("garden", socket)

        socket.send_json.side_effect = disconnect
        with pytest.raises(ConnectionError):
            await hub.request("garden", {"type": "config.get"}, "config.snapshot")
        assert not hub._pending
    asyncio.run(run())
