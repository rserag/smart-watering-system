import asyncio
from copy import deepcopy
from datetime import timedelta
from types import SimpleNamespace
from unittest.mock import AsyncMock
from uuid import uuid4

import pytest

from app import diagnostics as debug
from app import hubs
from app.diagnostics_protocol import DebugSnapshot
from app.hubs import DeviceHub
from app.models import Device, UserSession
from test_diagnostics import BOOT, DEVICE, snapshot


@pytest.fixture(autouse=True)
def clean_observations():
    debug.controller_states.clear()
    debug.snapshot_cache.clear()
    debug.snapshot_rings.clear()
    yield
    debug.controller_states.clear()
    debug.snapshot_cache.clear()
    debug.snapshot_rings.clear()


def test_concurrent_viewers_share_refresh_and_cooldown_preserves_timestamp_and_redaction(monkeypatch):
    async def exercise():
        clock = [100.0]
        monkeypatch.setattr(hubs, "monotonic", lambda: clock[0])
        hub = DeviceHub()
        monkeypatch.setattr(debug, "device_hub", hub)
        monkeypatch.setattr(debug, "audit_history", AsyncMock(return_value=[]))
        monkeypatch.setattr(debug, "capture_history", AsyncMock(return_value=[]))
        device = Device(id=DEVICE, boot_id=BOOT, config_revision=1)
        session = AsyncMock()
        session.get.return_value = device
        owner = UserSession(email=debug.OPERATOR_EMAIL)
        viewer = UserSession(email="viewer@example.com")
        session_id = str(uuid4())
        at = debug.utcnow()
        monkeypatch.setattr(debug, "utcnow", lambda: at)
        debug.remember_snapshot(DebugSnapshot.model_validate(snapshot(1, "simulation", session_id)),
                                at - timedelta(seconds=3))
        sent = asyncio.Event()
        release = asyncio.Event()
        writes = []

        async def send_json(message):
            writes.append(deepcopy(message))
            sent.set()
            await release.wait()
            hub.resolve_reply(socket, {**snapshot(2, "simulation", session_id), "requestId": message["requestId"]})

        socket = SimpleNamespace(send_json=send_json)
        await hub.register(DEVICE, socket)
        tasks = [asyncio.create_task(debug.diagnostics(DEVICE, user, session)) for user in (owner, viewer, viewer)]
        await sent.wait()
        await asyncio.sleep(0)
        assert len(writes) == 1
        release.set()
        results = await asyncio.gather(*tasks)
        assert results[0]["snapshot"]["session"]["sessionId"] == session_id
        assert all(item["snapshot"]["session"]["sessionId"] is None for item in results[1:])
        assert all(item["receivedAt"] == at.isoformat() and not item["stale"] for item in results)
        assert all(item["snapshot"]["sequence"] == 2 for item in results)

        # Wall-clock freshness is independent of the monotonic wire cooldown.
        at += timedelta(seconds=11)
        clock[0] += 4.9
        cooled = await debug.diagnostics(DEVICE, viewer, session)
        assert len(writes) == 1
        assert cooled["receivedAt"] == results[0]["receivedAt"]
        assert cooled["stale"] is True
        assert cooled["snapshot"]["session"]["sessionId"] is None
        assert debug.snapshot_cache[DEVICE][0]["session"]["sessionId"] == session_id
        await hub.unregister(DEVICE, socket)

    asyncio.run(exercise())


def test_read_timeouts_do_not_repeat_until_five_second_cooldown_and_command_reads_bypass_it(monkeypatch):
    async def exercise():
        clock = [100.0]
        monkeypatch.setattr(hubs, "monotonic", lambda: clock[0])
        hub = DeviceHub()
        writes = []
        reply_enabled = False

        async def send_json(message):
            writes.append(deepcopy(message))
            if reply_enabled:
                hub.resolve_reply(socket, {**snapshot(len(writes)), "requestId": message["requestId"]})

        socket = SimpleNamespace(send_json=send_json)
        await hub.register(DEVICE, socket)
        message = {"type": "debug.snapshot.get"}
        errors = await asyncio.gather(*(hub.request_observation(DEVICE, message, "debug.snapshot", timeout=.01)
                                       for _ in range(4)), return_exceptions=True)
        assert all(isinstance(error, TimeoutError) for error in errors)
        assert len(writes) == 1 and not hub._pending
        for advance in (0, 1, 3.999):
            clock[0] = 100 + advance
            assert await hub.request_observation(DEVICE, message, "debug.snapshot", timeout=.01) is None
        assert len(writes) == 1

        # Correlated command readbacks must not be suppressed by observation.
        reply_enabled = True
        monkeypatch.setattr(debug, "device_hub", hub)
        device = Device(id=DEVICE, boot_id=BOOT)
        result = await debug.read_snapshot(device)
        assert result[0]["sequence"] == 2 and len(writes) == 2
        reply_enabled = False
        clock[0] = 105
        with pytest.raises(TimeoutError):
            await hub.request_observation(DEVICE, message, "debug.snapshot", timeout=.01)
        assert len(writes) == 3 and not hub._pending
        await hub.unregister(DEVICE, socket)
        assert not hub._observations

    asyncio.run(exercise())


def test_cancelled_http_waiter_does_not_cancel_shared_wire_read():
    async def exercise():
        hub = DeviceHub()
        sent = asyncio.Event()
        writes = []

        async def send_json(message):
            writes.append(deepcopy(message))
            sent.set()

        socket = SimpleNamespace(send_json=send_json)
        await hub.register(DEVICE, socket)
        first = asyncio.create_task(hub.request_observation(DEVICE, {"type": "debug.snapshot.get"}, "debug.snapshot"))
        await sent.wait()
        second = asyncio.create_task(hub.request_observation(DEVICE, {"type": "debug.snapshot.get"}, "debug.snapshot"))
        first.cancel()
        with pytest.raises(asyncio.CancelledError):
            await first
        await asyncio.sleep(0)
        assert len(writes) == 1 and len(hub._pending) == 1
        request_id = writes[0]["requestId"]
        hub.resolve_reply(socket, {"type": "debug.snapshot", "requestId": request_id})
        correlated_id, reply = await second
        assert correlated_id == reply["requestId"] == request_id
        assert len(writes) == 1 and not hub._pending
        await hub.unregister(DEVICE, socket)

    asyncio.run(exercise())


def test_reconnect_resets_cooldown_and_old_disconnect_cannot_remove_new_slot(monkeypatch):
    async def exercise():
        monkeypatch.setattr(hubs, "monotonic", lambda: 100)
        hub = DeviceHub()
        old_sent = asyncio.Event()

        async def send_old(message):
            old_sent.set()

        old = SimpleNamespace(send_json=send_old, close=AsyncMock())
        await hub.register(DEVICE, old)
        first = asyncio.create_task(hub.request_observation(DEVICE, {"type": "debug.snapshot.get"}, "debug.snapshot"))
        await old_sent.wait()
        writes = []

        async def send_new(message):
            writes.append(deepcopy(message))
            hub.resolve_reply(new, {"type": "debug.snapshot", "requestId": message["requestId"]})

        new = SimpleNamespace(send_json=send_new)
        await hub.register(DEVICE, new)
        with pytest.raises(ConnectionError):
            await first
        assert await hub.request_observation(DEVICE, {"type": "debug.snapshot.get"}, "debug.snapshot") is not None
        assert len(writes) == 1
        await hub.unregister(DEVICE, old)
        assert await hub.request_observation(DEVICE, {"type": "debug.snapshot.get"}, "debug.snapshot") is None
        assert hub._observations[DEVICE].connection is new
        await hub.unregister(DEVICE, new)
        assert not hub._observations and not hub._pending
        with pytest.raises(ConnectionError):
            await hub.request_observation(DEVICE, {"type": "debug.snapshot.get"}, "debug.snapshot")
        assert not hub._observations

    asyncio.run(exercise())


def test_observation_rejects_different_boot_without_refreshing_cache(monkeypatch):
    async def exercise():
        hub = DeviceHub()
        monkeypatch.setattr(debug, "device_hub", hub)
        past = debug.utcnow() - timedelta(seconds=3)
        debug.remember_snapshot(DebugSnapshot.model_validate(snapshot()), past)
        writes = []

        async def send_json(message):
            writes.append(deepcopy(message))
            hub.resolve_reply(socket, {**snapshot(2), "bootId": "other-boot", "requestId": message["requestId"]})

        socket = SimpleNamespace(send_json=send_json)
        await hub.register(DEVICE, socket)
        with pytest.raises(ValueError, match="different boot or request"):
            await debug.refresh_observation(Device(id=DEVICE, boot_id=BOOT))
        await debug.refresh_observation(Device(id=DEVICE, boot_id=BOOT))
        assert len(writes) == 1
        assert debug.snapshot_cache[DEVICE][1] == past
        assert debug.snapshot_cache[DEVICE][0]["bootId"] == BOOT
        await hub.unregister(DEVICE, socket)

    asyncio.run(exercise())
