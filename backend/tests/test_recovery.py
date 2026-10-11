import asyncio
from datetime import datetime, timedelta, timezone
from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock

import pytest
from fastapi.testclient import TestClient
from pydantic import ValidationError

from app.auth import require_user
from app.database import get_session
from app.hubs import DeviceHub, device_hub
from app.main import app, command_wire_message, deliver_queued_commands, latest_snapshot, record_command_ack
from app.models import Command, Device, TelemetrySample, UserSession, ZoneSample
from app.protocol import CommandAck, ZoneTelemetry
from app.recovery import supports_paused_recovery


URL = "/api/devices/garden/zones/2/recovery"
PAYLOAD = {"action": "clear", "expectedBootId": "boot-1", "expectedRecoveryGeneration": 7}


@pytest.fixture
def client(monkeypatch):
    now = datetime.now(timezone.utc)
    device = Device(id="garden", firmware_version="0.7.0", boot_id="boot-1", last_seen_at=now)
    zone = {"id": 2, "fault": "maximum_watering_reached", "paused": False,
            "phase": "fault", "recoveryGeneration": 7}
    sample = TelemetrySample(id=1, device_id="garden", boot_id="boot-1", received_at=now,
                             payload={"zones": [zone], "mainTankLow": False})
    result = MagicMock()
    result.scalar_one_or_none.return_value = sample
    session = AsyncMock()
    session.add = MagicMock()
    session.get.return_value = device
    session.execute.return_value = result
    ack = {"type": "command.ack", "schemaVersion": 1, "deviceId": "garden",
           "command": "fault.clearPaused", "status": "accepted"}

    async def reply(_, message, reply_type):
        assert reply_type == "command.ack"
        return {"requestId": message["requestId"], **ack}

    request = AsyncMock(side_effect=reply)
    connected = AsyncMock(return_value=["garden"])
    monkeypatch.setattr(device_hub, "request", request)
    monkeypatch.setattr(device_hub, "connected_ids", connected)

    async def dependency():
        yield session

    app.dependency_overrides[get_session] = dependency
    user = UserSession(email="operator@example.com", google_sub="operator-1")
    app.dependency_overrides[require_user] = lambda: user
    http = TestClient(app)
    try:
        yield SimpleNamespace(http=http, session=session, device=device, sample=sample, zone=zone,
                              ack=ack, request=request, connected=connected)
    finally:
        http.close()
        app.dependency_overrides.clear()


def test_clear_waits_for_acceptance_and_audits_without_offline_queue(client):
    response = client.http.post(URL, json=PAYLOAD)
    assert response.status_code == 200
    assert response.json() == {"zoneId": 2, "action": "clear", "paused": True, "status": "confirmed"}
    command = client.session.add.call_args.args[0]
    assert command.command == "fault.clearPaused"
    assert command.status == "acknowledged"
    assert command.parameters["source"] == "dashboard"
    assert command.parameters["actor"] == {"email": "operator@example.com", "subject": "operator-1"}
    assert command.sent_at is not None and command.acknowledged_at is not None
    message = client.request.call_args.args[1]
    assert message == {"type": "fault.clearPaused", "requestId": command.id, "zoneId": 2,
                       "expectedBootId": "boot-1", "expectedRecoveryGeneration": 7,
                       "expiresAtEpoch": command.parameters["expiresAtEpoch"]}
    assert int(datetime.now(timezone.utc).timestamp()) + 25 <= message["expiresAtEpoch"] <= int(datetime.now(timezone.utc).timestamp()) + 30
    assert client.session.commit.await_count == 2


def test_resume_is_separate_and_requires_pause_without_fault(client):
    client.zone.update(paused=True, fault=None, phase="paused")
    client.ack["command"] = "zone.resume"
    response = client.http.post(URL, json={**PAYLOAD, "action": "resume"})
    assert response.status_code == 200
    assert response.json()["paused"] is False
    assert client.request.call_args.args[1]["type"] == "zone.resume"


@pytest.mark.parametrize("payload", [{}, {**PAYLOAD, "action": "clearall"},
                                      {**PAYLOAD, "expectedRecoveryGeneration": True},
                                      {**PAYLOAD, "expectedRecoveryGeneration": "7"},
                                      {**PAYLOAD, "expectedRecoveryGeneration": -1},
                                      {**PAYLOAD, "expectedRecoveryGeneration": 4294967296},
                                      {**PAYLOAD, "expectedBootId": ""},
                                      {**PAYLOAD, "zoneId": 3}])
def test_invalid_inputs_never_reach_controller(client, payload):
    assert client.http.post(URL, json=payload).status_code == 422
    client.request.assert_not_awaited()
    client.session.add.assert_not_called()


@pytest.mark.parametrize("firmware", [None, "development", "0.6.0", "0.7", "0.7.-1"])
def test_requires_firmware_with_pause_support(client, firmware):
    client.device.firmware_version = firmware
    assert client.http.post(URL, json=PAYLOAD).status_code == 409
    client.request.assert_not_awaited()
    client.session.add.assert_not_called()


def test_offline_recovery_is_never_queued(client):
    client.connected.return_value = []
    assert client.http.post(URL, json=PAYLOAD).status_code == 409
    client.request.assert_not_awaited()
    client.session.add.assert_not_called()


@pytest.mark.parametrize("age", [31, -10])
def test_stale_or_clock_invalid_readings_reject_recovery(client, age):
    client.sample.received_at = datetime.now(timezone.utc) - timedelta(seconds=age)
    assert client.http.post(URL, json=PAYLOAD).status_code == 409
    client.request.assert_not_awaited()


def test_missing_readings_reject_recovery(client):
    client.session.execute.return_value.scalar_one_or_none.return_value = None
    assert client.http.post(URL, json=PAYLOAD).status_code == 409
    client.request.assert_not_awaited()


@pytest.mark.parametrize("changed", ["device_boot", "sample_boot", "new_fault", "unsupported_fault"])
def test_confirmation_cannot_clear_a_different_fault_occurrence(client, changed):
    if changed == "device_boot":
        client.device.boot_id = "boot-2"
    elif changed == "sample_boot":
        client.sample.boot_id = "old-boot"
    elif changed == "new_fault":
        client.zone["recoveryGeneration"] = 8
    else:
        client.zone["fault"] = "sensor_invalid"
    assert client.http.post(URL, json=PAYLOAD).status_code == 409
    client.request.assert_not_awaited()


@pytest.mark.parametrize("changes", [{"paused": False}, {"fault": "sensor_invalid"}, {"phase": "disabled"}])
def test_resume_cannot_bypass_fault_or_channel_disable(client, changes):
    client.zone.update({"paused": True, "fault": None, "phase": "paused", **changes})
    assert client.http.post(URL, json={**PAYLOAD, "action": "resume"}).status_code == 409
    client.request.assert_not_awaited()


def test_resume_requires_refilling_low_tank(client):
    client.zone.update(paused=True, fault=None, phase="paused")
    client.sample.payload["mainTankLow"] = True
    response = client.http.post(URL, json={**PAYLOAD, "action": "resume"})
    assert response.status_code == 409
    assert "Refill" in response.json()["detail"]
    client.request.assert_not_awaited()


@pytest.mark.parametrize("missing", [True, False])
def test_resume_requires_confirmed_nonlow_tank_reading(client, missing):
    client.zone.update(paused=True, fault=None, phase="paused")
    if missing:
        del client.sample.payload["mainTankLow"]
    else:
        client.sample.payload["mainTankLow"] = None
    response = client.http.post(URL, json={**PAYLOAD, "action": "resume"})
    assert response.status_code == 409
    assert "main tank readings are unavailable" in response.json()["detail"]
    client.request.assert_not_awaited()


@pytest.mark.parametrize("status", ["rejected", "duplicate"])
def test_only_explicit_acceptance_confirms_recovery(client, status):
    client.ack.update(status=status, message="Confirmation expired")
    response = client.http.post(URL, json=PAYLOAD)
    assert response.status_code == 409
    assert response.json()["detail"] == "Confirmation expired"
    assert client.session.add.call_args.args[0].status == "rejected"


@pytest.mark.parametrize("field,value", [("command", "zone.resume"), ("deviceId", "other"),
                                        ("requestId", "other"), ("status", "invalid")])
def test_mismatched_or_malformed_ack_never_confirms_recovery(client, field, value):
    client.ack[field] = value
    assert client.http.post(URL, json=PAYLOAD).status_code == 409
    assert client.session.add.call_args.args[0].status == "unconfirmed"


@pytest.mark.parametrize("failure,status", [(ConnectionError(), 503), (TimeoutError(), 504)])
def test_disconnect_or_timeout_audits_uncertainty_and_never_queues(client, failure, status):
    client.request.side_effect = failure
    response = client.http.post(URL, json=PAYLOAD)
    assert response.status_code == status
    assert "could not be confirmed" in response.json()["detail"]
    assert client.session.add.call_args.args[0].status == "unconfirmed"


def test_authentication_unknown_device_and_unknown_channel(client):
    assert client.http.post(URL.replace("/2/", "/3/"), json=PAYLOAD).status_code == 404
    client.session.get.return_value = None
    assert client.http.post(URL, json=PAYLOAD).status_code == 404
    del app.dependency_overrides[require_user]
    assert client.http.post(URL, json=PAYLOAD).status_code == 401
    client.request.assert_not_awaited()


def test_generic_commands_cannot_bypass_protected_recovery(client):
    url = "/api/devices/garden/commands"
    assert client.http.post(url, json={"command": "fault.clear", "parameters": {"zoneId": 2}}).status_code == 409
    for command in ("fault.clearPaused", "zone.resume"):
        assert client.http.post(url, json={"command": command, "parameters": {"zoneId": 2}}).status_code == 422
    client.session.add.assert_not_called()
    client.request.assert_not_awaited()


def test_generic_parameters_cannot_override_command_envelope():
    command = Command(id="trusted-request", device_id="garden", command="telemetry.request",
                      parameters={"type": "zone.resume", "deviceId": "other", "requestId": "forged", "schemaVersion": 8})
    assert command_wire_message(command) == {"type": "telemetry.request", "deviceId": "garden", "requestId": "trusted-request", "schemaVersion": 1}


@pytest.mark.parametrize("firmware,supported", [("0.7.0", True), ("0.7.1", True), ("1.0.0", True),
                                               ("0.6.99", False), ("0.7", False), ("0.7.-1", False)])
def test_firmware_capability(firmware, supported):
    assert supports_paused_recovery(firmware) is supported


@pytest.mark.parametrize("recovery", [{}, {"paused": True, "recoveryGeneration": 9}])
def test_snapshot_preserves_recovery_state_and_exposes_reading_freshness(recovery):
    now = datetime.now(timezone.utc)
    session = AsyncMock()
    sample = SimpleNamespace(id=1, wifi_rssi=-60, received_at=now, boot_id="boot-1", payload={"zones": [{"id": 2, **recovery}]})
    latest = MagicMock()
    latest.scalar_one_or_none.return_value = sample
    names = MagicMock()
    names.scalars.return_value = []
    zones = MagicMock()
    zones.scalars.return_value = [ZoneSample(zone_id=2, raw=2000, filtered_raw=2000,
        moisture_percent=20, sensor_valid=True, phase="paused", relay_on=False,
        watering_on_ms_this_cycle=0, fault=None)]
    event = MagicMock()
    event.scalar_one_or_none.return_value = None
    session.execute.side_effect = [latest, names, zones, event]
    session.get.return_value = None
    snapshot = asyncio.run(latest_snapshot(session, Device(id="garden", boot_id="boot-1", last_seen_at=now)))
    assert snapshot["telemetryReceivedAt"] == now.isoformat()
    assert snapshot["telemetryBootId"] == "boot-1"
    assert snapshot["zones"][0]["paused"] == recovery.get("paused", False)
    assert snapshot["zones"][0]["recoveryGeneration"] == recovery.get("recoveryGeneration", 0)


def test_telemetry_accepts_pause_state_and_defaults_legacy_firmware():
    raw = {"id": 1, "raw": 2000, "filteredRaw": 1990, "relativeMoisturePercent": 20,
           "sensorValid": True, "phase": "paused", "relayOn": False, "wateringOnMsThisCycle": 0}
    legacy = ZoneTelemetry.model_validate(raw)
    assert legacy.paused is False and legacy.recovery_generation == 0
    current = ZoneTelemetry.model_validate({**raw, "paused": True, "recoveryGeneration": 4294967295})
    assert current.paused is True and current.recovery_generation == 4294967295
    with pytest.raises(ValidationError):
        ZoneTelemetry.model_validate({**raw, "recoveryGeneration": True})


def test_audited_request_preserves_id_rejects_pending_collision_and_cleans_up():
    async def run():
        hub = DeviceHub()
        socket = AsyncMock()
        await hub.register("garden", socket)

        async def reply(message):
            assert message["requestId"] == "audit-id"
            with pytest.raises(ValueError):
                await hub.request("garden", {"type": "zone.resume", "requestId": "audit-id"}, "command.ack")
            hub.resolve_reply(socket, {**message, "type": "command.ack"})

        socket.send_json.side_effect = reply
        result = await hub.request("garden", {"type": "fault.clearPaused", "requestId": "audit-id"}, "command.ack")
        assert result["requestId"] == "audit-id"
        assert not hub._pending
    asyncio.run(run())


def test_queued_recovery_never_runs_after_reconnection(monkeypatch):
    commands = [Command(id=str(i), device_id="garden", command=kind, parameters={}, status="queued")
                for i, kind in enumerate(("fault.clear", "fault.clearPaused", "zone.resume", "zone.stop"))]
    session = AsyncMock()
    result = MagicMock()
    result.scalars.return_value = commands
    session.execute.return_value = result
    context = MagicMock()
    context.__aenter__ = AsyncMock(return_value=session)
    context.__aexit__ = AsyncMock(return_value=False)
    monkeypatch.setattr("app.main.SessionLocal", lambda: context)
    send = AsyncMock(return_value=True)
    monkeypatch.setattr(device_hub, "send", send)
    asyncio.run(deliver_queued_commands("garden"))
    assert [command.status for command in commands] == ["failed", "failed", "failed", "sent"]
    send.assert_awaited_once()
    assert send.call_args.args[1]["type"] == "zone.stop"


@pytest.mark.parametrize("kind", ["fault.clearPaused", "zone.resume"])
def test_late_ack_cannot_overwrite_protected_audit_outcome(monkeypatch, kind):
    command = Command(id="request-1", device_id="garden", command=kind, parameters={}, status="unconfirmed", result="Timed out")
    session = AsyncMock()
    session.get.return_value = command
    context = MagicMock()
    context.__aenter__ = AsyncMock(return_value=session)
    context.__aexit__ = AsyncMock(return_value=False)
    monkeypatch.setattr("app.main.SessionLocal", lambda: context)
    ack = CommandAck.model_validate({"type": "command.ack", "schemaVersion": 1, "deviceId": "garden", "requestId": "request-1", "command": kind, "status": "duplicate"})
    asyncio.run(record_command_ack("garden", ack))
    assert command.status == "unconfirmed" and command.result == "Timed out"
    session.commit.assert_not_awaited()
