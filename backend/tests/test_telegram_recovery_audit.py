import asyncio
from unittest.mock import AsyncMock, MagicMock

import pytest

from app.main import record_telegram_delivery
from app.models import Command, Device, TelegramDelivery
from app.protocol import TelegramDeliveryMessage


@pytest.mark.parametrize("kind,request_id", [
    ("maximum_watering_fault", None),
    ("fault_cleared_paused", "telegram:123456789:91234:2"),
    ("channel_resumed", "telegram:123456789:91235:2"),
])
def test_recovery_delivery_kinds_persist_without_dashboard_command(monkeypatch, kind, request_id):
    session = AsyncMock()
    session.add = MagicMock()

    async def get(model, key):
        if model is Device:
            assert key == "garden"
            return Device(id="garden")
        if model is TelegramDelivery:
            assert key == "boot-1:7"
            return None
        assert model is Command and key == request_id
        # Direct Telegram actions have no server-created command row.
        return None

    session.get.side_effect = get
    context = MagicMock()
    context.__aenter__ = AsyncMock(return_value=session)
    context.__aexit__ = AsyncMock(return_value=False)
    monkeypatch.setattr("app.main.SessionLocal", lambda: context)
    message = TelegramDeliveryMessage.model_validate({
        "type": "telegram.delivery", "schemaVersion": 1, "deviceId": "garden",
        "eventId": "boot-1:7", "requestId": request_id, "updateSequence": 1,
        "kind": kind, "status": "queued", "attempt": 0, "uptimeMs": 1234,
        "pendingCount": 1,
    })
    result = asyncio.run(record_telegram_delivery("garden", message))
    delivery = session.add.call_args.args[0]
    assert delivery.kind == kind and delivery.request_id == request_id
    assert result["kind"] == kind and result["requestId"] == request_id
    assert result["eventId"] == "boot-1:7" and result["status"] == "queued"
    session.commit.assert_awaited_once()
