import asyncio
from copy import deepcopy
from datetime import datetime, timedelta, timezone
from types import SimpleNamespace
from unittest.mock import AsyncMock, MagicMock
from uuid import uuid4

import pytest
from fastapi.testclient import TestClient
from pydantic import ValidationError

from app import diagnostics as debug
from app import main
from app.auth import require_user
from app.database import get_session
from app.diagnostics_protocol import DebugAction, DebugSession, DebugSnapshot
from app.hubs import DeviceHub, device_hub
from app.models import Command, Device, DiagnosticAudit, DiagnosticCapture, DiagnosticState, TelemetrySample, UserSession
from app.protocol import TelemetryMessage


DEVICE = "diagnostic-test"
BOOT = "test-boot"
BASE = f"/api/devices/{DEVICE}"


def snapshot(sequence=1, mode="observe", session_id=None):
    return {"type": "debug.snapshot", "schemaVersion": 1, "deviceId": DEVICE, "bootId": BOOT,
        "sequence": sequence, "uptimeMs": sequence * 1000, "configRevision": 1,
        "session": {"mode": mode, "sessionId": session_id, "owner": debug.OPERATOR_EMAIL if session_id else None,
                    "leaseRemainingMs": 30000 if session_id else 0, "automationHeld": bool(session_id)},
        "hardware": {"board": "esp32dev", "relayPins": [27, 26, 25, 33], "sensorPins": [34, 35, 36, 39],
            "tankPin": 23, "relayActiveHigh": True, "oled": {"sda": 21, "scl": 22, "address": 60, "available": True}},
        "tank": {"physicalLow": False, "effectiveLow": False, "low": False, "pendingLow": False,
                 "debounceRemainingMs": 0, "overrideLow": None},
        "zones": [{"id": zone_id, "physicalRaw": 2000, "raw": 2000, "samples": [2000] * 16,
            "minRaw": 2000, "maxRaw": 2000, "filteredRaw": 2000, "relativeMoisturePercent": 44.0,
            "unclampedPercent": 44.0, "sensorValid": True, "validSamples": 3, "invalidSamples": 0,
            "dryCount": 0, "wetCount": 0, "sampleAgeMs": 0, "phase": "idle", "fault": None,
            "enabled": True, "paused": False, "relayOn": False, "virtualRelayOn": False,
            "decisionReason": "waiting", "phaseElapsedMs": 1000, "pulseRemainingMs": 0,
            "wateringOnMsThisCycle": 0, "overrideRaw": None, "dryRaw": 2600, "wetRaw": 1250,
            "startWateringPercent": 30, "stopWateringPercent": 63, "pulseMs": 8000,
            "soakMs": 90000, "maxOnMs": 60000} for zone_id in range(1, 5)]}


@pytest.fixture(autouse=True)
def clean_caches():
    debug.controller_states.clear()
    debug.snapshot_cache.clear()
    debug.snapshot_rings.clear()
    debug.command_locks.clear()
    yield
    debug.controller_states.clear()
    debug.snapshot_cache.clear()
    debug.snapshot_rings.clear()
    debug.command_locks.clear()


@pytest.fixture
def client(monkeypatch):
    device = Device(id=DEVICE, boot_id=BOOT, config_revision=1, firmware_version="0.8.0", last_seen_at=debug.utcnow())
    session = AsyncMock()
    objects = {}
    reports = {"snapshot": snapshot(), "online": True, "ack_status": "accepted", "ack_error": None,
               "read_error": None, "ack_change": {}, "writes": []}
    debug.remember_snapshot(DebugSnapshot.model_validate(reports["snapshot"]))

    async def get(model, key):
        if model is Device:
            return device if key == DEVICE else None
        return objects.get((model, key))

    async def execute(statement):
        model = statement.column_descriptions[0].get("entity") if hasattr(statement, "column_descriptions") else None
        result = MagicMock()
        result.scalar_one_or_none.return_value = None
        result.scalars.return_value = [item for (kind, _), item in objects.items() if kind is model]
        return result

    session.get.side_effect = get
    session.execute.side_effect = execute
    session.add = MagicMock(side_effect=lambda item: objects.__setitem__((type(item), item.id), item))

    async def request(device_id, message, reply_type, timeout=10):
        assert device_id == DEVICE
        if message["type"] == "debug.snapshot.get":
            if reports["read_error"]:
                raise reports["read_error"]
            reports["snapshot"]["sequence"] += 1
            reports["snapshot"]["requestId"] = message["requestId"]
            return deepcopy(reports["snapshot"])
        reports["writes"].append(deepcopy(message))
        if reports["ack_error"]:
            raise reports["ack_error"]
        current = reports["snapshot"]["session"]
        if reports["ack_status"] == "accepted":
            if message["type"] == "debug.session.start":
                current.update(mode=message["mode"], sessionId=message["sessionId"], owner=message["owner"],
                               leaseRemainingMs=30000, automationHeld=True)
            elif message["type"] == "debug.session.end":
                current.update(mode="observe", sessionId=None, owner=None, leaseRemainingMs=0, automationHeld=True)
            elif message["type"] == "debug.automation.resume":
                current["automationHeld"] = False
        return {"type": "command.ack", "schemaVersion": 1, "deviceId": DEVICE, "requestId": message["requestId"],
                "command": message["type"], "status": reports["ack_status"], "message": "controller outcome", **reports["ack_change"]}

    async def dependency():
        yield session

    user = UserSession(email=debug.OPERATOR_EMAIL, google_sub="owner", name="Owner")
    main.app.dependency_overrides[get_session] = dependency
    main.app.dependency_overrides[require_user] = lambda: user
    monkeypatch.setattr(device_hub, "request", AsyncMock(side_effect=request))
    monkeypatch.setattr(device_hub, "connected_ids", AsyncMock(side_effect=lambda: [DEVICE] if reports["online"] else []))
    http = TestClient(main.app)
    try:
        yield SimpleNamespace(http=http, session=session, reports=reports, objects=objects, device=device, user=user)
    finally:
        http.close()
        main.app.dependency_overrides.clear()


def start(client, mode="simulation"):
    response = client.http.post(BASE + "/debug/session", json={"mode": mode, "expectedBootId": BOOT})
    assert response.status_code == 200, response.text
    return response.json()["sessionId"]


def action(client, command, session_id=None, parameters=None):
    return client.http.post(BASE + "/debug/actions", json={"command": command, "expectedBootId": BOOT,
        "sessionId": session_id, "parameters": parameters or {}})


def test_owner_authorization_has_no_development_bypass(client):
    client.user.email = "viewer@example.com"
    assert client.http.get(BASE + "/diagnostics").status_code == 200
    assert client.http.get(BASE + "/diagnostics").json()["canControl"] is False
    assert client.http.post(BASE + "/debug/session", json={"mode": "hardware", "expectedBootId": BOOT}).status_code == 403
    assert client.http.post(BASE + "/debug/captures", json={"name": "capture"}).status_code == 403
    for path, method, payload in [("/commands", "post", {"command": "zone.water", "parameters": {"zoneId": 1}}),
                                 ("/zones/1/pulse", "patch", {"pulseOnMs": 1000, "expectedRevision": 1}),
                                 ("/zones/1/recovery", "post", {"action": "resume", "expectedBootId": BOOT, "expectedRecoveryGeneration": 0}),
                                 ("/telegram/hourly-silent", "patch", {"enabled": True}),
                                 ("/telegram/debug", "post", {})]:
        assert getattr(client.http, method)(BASE + path, json=payload).status_code == 403
    assert not client.reports["writes"]


def test_session_lifecycle_readback_and_automation_hold(client):
    session_id = start(client, "hardware")
    UUID = __import__("uuid").UUID
    UUID(session_id)
    command = client.reports["writes"][0]
    assert command["owner"] == debug.OPERATOR_EMAIL
    assert command["expectedBootId"] == BOOT
    assert int(debug.utcnow().timestamp()) + 25 <= command["expiresAtEpoch"] <= int(debug.utcnow().timestamp()) + 30
    assert client.http.post(BASE + "/debug/session", json={"mode": "simulation", "expectedBootId": BOOT}).status_code == 409
    assert client.http.post(BASE + "/commands", json={"command": "config.set", "parameters": {}}).status_code == 409
    assert action(client, "debug.automation.resume").status_code == 409
    renewed = client.http.post(BASE + "/debug/session/renew", json={"sessionId": session_id, "expectedBootId": BOOT})
    assert renewed.status_code == 200
    ended = client.http.post(BASE + "/debug/session/end", json={"sessionId": session_id, "expectedBootId": BOOT})
    assert ended.status_code == 200
    assert ended.json()["snapshot"]["session"]["automationHeld"] is True
    assert action(client, "debug.automation.resume").json()["snapshot"]["session"]["automationHeld"] is False


@pytest.mark.parametrize("change", ["offline", "boot", "stale", "expired", "other_session"])
def test_actions_reject_offline_stale_boot_or_lease(client, change):
    session_id = start(client)
    if change == "offline":
        client.reports["online"] = False
    elif change == "boot":
        client.device.boot_id = "new-boot"
    elif change == "stale":
        cached = debug.snapshot_cache[DEVICE]
        debug.snapshot_cache[DEVICE] = (cached[0], debug.utcnow() - timedelta(seconds=11))
        client.reports["read_error"] = TimeoutError()
    elif change == "expired":
        debug.controller_states[DEVICE]["state"]["leaseRemainingMs"] = 0
    else:
        session_id = str(uuid4())
    count = len(client.reports["writes"])
    assert action(client, "debug.output.pulse", session_id, {"zoneId": 1, "durationMs": 1000}).status_code == 409
    assert len(client.reports["writes"]) == count


def test_stop_available_stale_and_without_active_session(client):
    cached = debug.snapshot_cache[DEVICE]
    debug.snapshot_cache[DEVICE] = (cached[0], debug.utcnow() - timedelta(minutes=1))
    assert action(client, "debug.output.stop").status_code == 200
    assert len(client.reports["writes"]) == 1


def test_stop_can_cancel_an_expired_reported_lease(client):
    session_id = start(client, "hardware")
    debug.controller_states[DEVICE]["state"]["leaseRemainingMs"] = 0
    assert action(client, "debug.output.stop", session_id).status_code == 200


@pytest.mark.parametrize("parameters", [{"zoneId": 0, "durationMs": 1000}, {"zoneId": 5, "durationMs": 1000},
    {"zoneId": True, "durationMs": 1000}, {"zoneId": 1, "durationMs": 99}, {"zoneId": 1, "durationMs": 60001},
    {"zoneId": 1, "durationMs": "1000"}, {"zoneId": 1, "durationMs": 1000, "type": "config.set"}])
def test_strict_action_validation_never_delivers(client, parameters):
    session_id = start(client)
    count = len(client.reports["writes"])
    assert action(client, "debug.output.pulse", session_id, parameters).status_code == 422
    assert len(client.reports["writes"]) == count


def test_input_validation_and_hardware_cannot_inject(client):
    session_id = start(client, "hardware")
    assert action(client, "debug.input.set", session_id, {"input": "tank", "low": False}).status_code == 409
    assert action(client, "debug.input.set", session_id, {"input": "tank", "low": "false"}).status_code == 422
    assert action(client, "debug.input.set", session_id, {"input": "soil", "raw": 4096, "zoneId": 1}).status_code == 422
    assert action(client, "debug.input.clear", session_id, {"input": "tank", "zoneId": 1}).status_code == 422


@pytest.mark.parametrize("failure", [ConnectionError(), TimeoutError(), "wrong_ack", "readback"])
def test_uncertain_ack_or_readback_is_audited_without_retry(client, failure):
    session_id = start(client)
    if failure == "wrong_ack":
        client.reports["ack_change"] = {"requestId": "different-request"}
    elif failure == "readback":
        client.reports["read_error"] = TimeoutError()
    else:
        client.reports["ack_error"] = failure
    count = len(client.reports["writes"])
    response = action(client, "debug.output.pulse", session_id, {"zoneId": 1, "durationMs": 1000})
    assert response.status_code in {503, 504}
    assert response.json()["detail"]["status"] == "unconfirmed"
    assert len(client.reports["writes"]) == count + 1
    record = client.objects[(DiagnosticAudit, response.json()["detail"]["requestId"])]
    assert record.status == "unconfirmed"


def test_delayed_renewal_readback_uses_five_seconds_but_observation_keeps_two(monkeypatch):
    async def exercise():
        hub = DeviceHub()
        hub.request = AsyncMock(wraps=hub.request)
        session_id = str(uuid4())
        device = Device(id=DEVICE, boot_id=BOOT, config_revision=1)
        user = UserSession(email=debug.OPERATOR_EMAIL, google_sub="operator")
        session = AsyncMock()
        session.add = MagicMock()
        writes = []
        replies = []

        async def send_json(message):
            writes.append(deepcopy(message))
            if message["type"] == "debug.session.renew":
                hub.resolve_reply(socket, {"type": "command.ack", "schemaVersion": 1,
                    "deviceId": DEVICE, "requestId": message["requestId"],
                    "command": "debug.session.renew", "status": "accepted"})
            else:
                assert message["type"] == "debug.snapshot.get"

                async def delayed_snapshot():
                    # Exercise the real DeviceHub asyncio deadline: this would
                    # time out under the former two-second command readback.
                    await asyncio.sleep(2.1)
                    reply = snapshot(mode="simulation", session_id=session_id)
                    reply["sequence"] = len(writes)
                    reply["requestId"] = message["requestId"]
                    hub.resolve_reply(socket, reply)

                replies.append(asyncio.create_task(delayed_snapshot()))

        socket = SimpleNamespace(send_json=send_json)
        await hub.register(DEVICE, socket)
        monkeypatch.setattr(debug, "device_hub", hub)
        try:
            result = await debug.issue(session, device, user, "debug.session.renew",
                                       {"sessionId": session_id}, session_id, "session")
            assert result["status"] == "accepted"
            assert result["snapshot"]["session"]["sessionId"] == session_id
            assert session.add.call_args.args[0].status == "accepted"
            snapshot_calls = [call for call in hub.request.await_args_list
                              if call.args[2] == "debug.snapshot"]
            assert snapshot_calls[0].kwargs["timeout"] == 5

            with pytest.raises(TimeoutError):
                await debug.read_snapshot(device)
            assert hub.request.await_args_list[-1].kwargs["timeout"] == 2
            assert sum(message["type"] == "debug.session.renew" for message in writes) == 1
            assert not hub._pending
        finally:
            for reply in replies:
                reply.cancel()
            await asyncio.gather(*replies, return_exceptions=True)

    asyncio.run(exercise())


@pytest.mark.parametrize("status", ["rejected", "duplicate"])
def test_duplicate_or_rejected_ack_never_confirms_actuation(client, status):
    session_id = start(client)
    client.reports["ack_status"] = status
    response = action(client, "debug.output.pulse", session_id, {"zoneId": 1, "durationMs": 1000})
    assert response.status_code == 409
    assert response.json()["detail"]["status"] == "rejected"


def test_bounded_capture_and_nested_viewer_redaction(client):
    session_id = start(client)
    for sequence in range(3, 608):
        debug.remember_snapshot(DebugSnapshot.model_validate(snapshot(sequence, "simulation", session_id)))
    assert len(debug.snapshot_rings[DEVICE]) == 600
    response = client.http.post(BASE + "/debug/captures", json={"name": "  Dry sensor  "})
    assert response.status_code == 201
    capture = response.json()
    assert capture["name"] == "Dry sensor" and capture["sampleCount"] == 600
    client.user.email = "viewer@example.com"
    diagnostics = client.http.get(BASE + "/diagnostics").json()
    assert diagnostics["snapshot"]["session"]["sessionId"] is None
    assert diagnostics["actions"][0]["parameters"]["sessionId"] is None
    captured = client.http.get(BASE + "/debug/captures/" + capture["id"]).json()
    assert captured["snapshots"][0]["snapshot"]["session"]["sessionId"] is None
    assert client.objects[(DiagnosticCapture, capture["id"])].snapshots[0]["snapshot"]["session"]["sessionId"] == session_id
    item = client.objects[(DiagnosticCapture, capture["id"])]
    item.created_at = debug.utcnow() - timedelta(days=8)
    assert client.http.get(BASE + "/debug/captures/" + capture["id"]).status_code == 404


def test_legacy_fallback_is_truthful_and_has_no_controls(client):
    debug.controller_states[DEVICE]["version"] = None
    debug.controller_states[DEVICE]["state"] = {}
    result = client.http.get(BASE + "/diagnostics").json()
    assert result["supported"] is False
    assert result["snapshot"]["hardware"]["source"] == "documentedFallback"
    assert result["snapshot"]["hardware"]["oled"]["available"] is None
    assert result["snapshot"]["zones"] == []
    assert result["receivedAt"] is None
    assert client.http.post(BASE + "/debug/session", json={"mode": "hardware", "expectedBootId": BOOT}).status_code == 409


def test_debug_lifecycle_events_bound_to_original_request_and_terminal_never_regresses(client):
    session_id = start(client)
    response = action(client, "debug.output.pulse", session_id, {"zoneId": 1, "durationMs": 1000})
    request_id = response.json()["requestId"]
    data = {"type": "debug.action", "schemaVersion": 1, "deviceId": DEVICE, "bootId": BOOT,
            "sessionId": session_id, "requestId": request_id, "command": "debug.output.pulse", "uptimeMs": 1000}
    asyncio.run(debug.record_action(client.session, DebugAction.model_validate({**data, "status": "completed"})))
    asyncio.run(debug.record_action(client.session, DebugAction.model_validate({**data, "status": "started"})))
    original = client.objects[(DiagnosticAudit, request_id)]
    assert original.status == "completed"
    count = len(client.objects)
    asyncio.run(debug.record_action(client.session, DebugAction.model_validate({**data, "status": "completed"})))
    assert len(client.objects) == count
    asyncio.run(debug.record_action(client.session, DebugAction.model_validate({**data, "sessionId": str(uuid4()), "status": "cancelled"})))
    assert len(client.objects) == count


def test_snapshot_validation_rejects_mixed_or_unsafe_observations():
    data = snapshot(mode="simulation", session_id=str(uuid4()))
    data["zones"][0]["relayOn"] = True
    with pytest.raises(ValidationError):
        DebugSnapshot.model_validate(data)
    data = snapshot()
    data["zones"][1]["id"] = 1
    with pytest.raises(ValidationError):
        DebugSnapshot.model_validate(data)
    data = snapshot()
    data["zones"][0]["samples"] = [4096] * 16
    with pytest.raises(ValidationError):
        DebugSnapshot.model_validate(data)


def test_snapshot_sequence_replay_does_not_refresh_freshness_and_reboot_clears_cache():
    past = debug.utcnow() - timedelta(minutes=1)
    debug.remember_snapshot(DebugSnapshot.model_validate(snapshot(3)), past)
    assert not debug.remember_snapshot(DebugSnapshot.model_validate(snapshot(2)))
    assert debug.snapshot_cache[DEVICE][1] == past
    debug.remember_state(DEVICE, "new-boot", 1, DebugSession.model_validate(snapshot()["session"]), debug.utcnow())
    assert DEVICE not in debug.snapshot_cache
    assert not debug.remember_snapshot(DebugSnapshot.model_validate(snapshot(4)))


def test_retention_is_bounded_and_only_deletes_additive_diagnostic_tables():
    session = AsyncMock()
    asyncio.run(debug.purge_diagnostics(session))
    assert session.execute.await_count == 2
    statements = [call.args[0] for call in session.execute.await_args_list]
    assert "DELETE FROM diagnostic_audit" in str(statements[0])
    assert "DELETE FROM diagnostic_captures" in str(statements[1])
    assert all(500 in statement.compile().params.values() for statement in statements)
    assert all("telemetry_samples" not in str(statement) for statement in statements)


def test_controller_lease_termination_is_audited_without_a_dashboard_end_request():
    session_id = str(uuid4())
    stored = DiagnosticState(device_id=DEVICE, boot_id=BOOT, version=1,
        state=snapshot(mode="hardware", session_id=session_id)["session"], received_at=debug.utcnow())
    session = AsyncMock()
    session.get.side_effect = lambda model, key: stored if model is DiagnosticState else None
    session.add = MagicMock()
    ended = DebugSession.model_validate({"mode": "observe", "sessionId": None, "owner": None,
                                        "leaseRemainingMs": 0, "automationHeld": True})
    asyncio.run(debug.store_state(session, DEVICE, BOOT, 1, ended, debug.utcnow()))
    event = session.add.call_args.args[0]
    assert event.record_type == "session" and event.status == "cancelled"
    assert event.session_id == session_id
    assert event.result["automationHeld"] is True


def test_capture_polling_queries_metadata_without_loading_trace_or_actions():
    session = AsyncMock()
    result = MagicMock()
    result.all.return_value = []
    session.execute.return_value = result
    asyncio.run(debug.capture_history(session, DEVICE))
    query = str(session.execute.call_args.args[0])
    assert "jsonb_array_length(diagnostic_captures.snapshots)" in query
    assert "diagnostic_captures.actions" not in query


@pytest.mark.parametrize("mode,explicit", [("simulation", False), ("observe", True)])
def test_simulation_never_updates_ordinary_samples_tank_or_watering(monkeypatch, mode, explicit):
    session = AsyncMock()
    session.add = MagicMock()
    device = Device(id=DEVICE, boot_id=BOOT)
    session.get.return_value = device
    context = MagicMock()
    context.__aenter__ = AsyncMock(return_value=session)
    context.__aexit__ = AsyncMock()
    monkeypatch.setattr(main, "SessionLocal", lambda: context)
    monkeypatch.setattr(main, "latest_snapshot", AsyncMock(return_value={"id": DEVICE}))
    tank = AsyncMock()
    watering = AsyncMock()
    monkeypatch.setattr(main, "update_main_tank_state", tank)
    monkeypatch.setattr(main, "update_watering_event", watering)
    debug_state = snapshot(mode=mode, session_id=str(uuid4()) if mode == "simulation" else None)["session"]
    telemetry = TelemetryMessage.model_validate({"type": "telemetry", "schemaVersion": 1, "deviceId": DEVICE,
        "bootId": BOOT, "sequence": 1, "uptimeMs": 1000, "configRevision": 1, "wifiRssi": -50,
        "mainTankLow": True, "diagnosticsVersion": 1, "debug": debug_state, "simulation": explicit,
        "zones": [{"id": 1, "raw": 4095, "filteredRaw": 4095, "relativeMoisturePercent": 0,
                   "sensorValid": True, "phase": "pulse", "relayOn": False, "wateringOnMsThisCycle": 1000}]})
    asyncio.run(main.record_telemetry(telemetry))
    assert session.add.call_count == 0
    tank.assert_not_awaited()
    watering.assert_not_awaited()


def test_real_diagnostic_watering_is_labelled_diagnostic(monkeypatch):
    session = AsyncMock()
    session.add = MagicMock()
    session.get.return_value = Device(id=DEVICE, boot_id=BOOT)
    context = MagicMock()
    context.__aenter__ = AsyncMock(return_value=session)
    context.__aexit__ = AsyncMock()
    monkeypatch.setattr(main, "SessionLocal", lambda: context)
    monkeypatch.setattr(main, "latest_snapshot", AsyncMock(return_value={"id": DEVICE}))
    monkeypatch.setattr(main, "update_main_tank_state", AsyncMock())
    watering = AsyncMock()
    monkeypatch.setattr(main, "update_watering_event", watering)
    telemetry = TelemetryMessage.model_validate({"type": "telemetry", "schemaVersion": 1, "deviceId": DEVICE,
        "bootId": BOOT, "sequence": 1, "uptimeMs": 1000, "configRevision": 1, "wifiRssi": -50,
        "mainTankLow": False, "diagnosticsVersion": 1,
        "debug": snapshot(mode="hardware", session_id=str(uuid4()))["session"],
        "zones": [{"id": 1, "raw": 2000, "filteredRaw": 2000, "relativeMoisturePercent": 44,
                   "sensorValid": True, "phase": "pulse", "relayOn": True, "wateringOnMsThisCycle": 1000}]})
    asyncio.run(main.record_telemetry(telemetry))
    assert watering.await_args.kwargs["source"] == "diagnostic"
    assert any(isinstance(call.args[0], TelemetrySample) for call in session.add.call_args_list)


def test_queued_watering_cannot_cross_durable_hold(monkeypatch):
    debug.remember_state(DEVICE, BOOT, 1, DebugSession.model_validate({"mode": "observe", "sessionId": None,
        "owner": None, "leaseRemainingMs": 0, "automationHeld": True}), debug.utcnow())
    command = Command(id=str(uuid4()), device_id=DEVICE, command="zone.water", status="queued", parameters={})
    session = AsyncMock()
    result = MagicMock()
    result.scalars.return_value = [command]
    session.execute.return_value = result
    context = MagicMock()
    context.__aenter__ = AsyncMock(return_value=session)
    context.__aexit__ = AsyncMock()
    monkeypatch.setattr(main, "SessionLocal", lambda: context)
    send = AsyncMock()
    monkeypatch.setattr(device_hub, "send", send)
    asyncio.run(main.deliver_queued_commands(DEVICE))
    assert command.status == "failed"
    send.assert_not_awaited()
