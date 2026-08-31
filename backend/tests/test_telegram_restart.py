import asyncio
from datetime import datetime, timezone

from app.main import (
    close_interrupted_telegram_deliveries,
    interrupted_telegram_deliveries_query,
    supports_manual_telegram_debug,
)
from app.models import Command, TelegramDelivery


def test_manual_telegram_debug_requires_fixed_firmware() -> None:
    assert supports_manual_telegram_debug("0.5.0") is False
    assert supports_manual_telegram_debug("0.5.1") is True
    assert supports_manual_telegram_debug("0.6.0") is True
    assert supports_manual_telegram_debug(None) is False
    assert supports_manual_telegram_debug("development") is False


class FakeScalarResult:
    def __init__(self, deliveries: list[TelegramDelivery]):
        self.deliveries = deliveries

    def scalars(self) -> list[TelegramDelivery]:
        return self.deliveries


class FakeSession:
    def __init__(
        self, deliveries: list[TelegramDelivery], commands: dict[str, Command]
    ):
        self.deliveries = deliveries
        self.commands = commands

    async def execute(self, _statement: object) -> FakeScalarResult:
        return FakeScalarResult(self.deliveries)

    async def get(self, model: type[object], key: str) -> object | None:
        assert model is Command
        return self.commands.get(key)


def test_interrupted_query_selects_inflight_events_from_older_boots() -> None:
    statement = interrupted_telegram_deliveries_query("controller-1", "new-boot")
    compiled = statement.compile()

    assert "telegram_deliveries.status IN" in str(compiled)
    assert "telegram_deliveries.event_id NOT LIKE" in str(compiled)
    assert "controller-1" in compiled.params.values()
    assert "new-boot:" in compiled.params.values()


def test_controller_restart_closes_delivery_and_fails_linked_command() -> None:
    observed_at = datetime.now(timezone.utc)
    delivery = TelegramDelivery(
        event_id="old-boot:3",
        device_id="controller-1",
        request_id="request-1",
        kind="manual_debug",
        status="sending",
        update_sequence=2,
        attempt=1,
        device_uptime_ms=1234,
        pending_count=0,
    )
    command = Command(
        id="request-1",
        device_id="controller-1",
        command="telegram.debug.send",
        parameters={},
        status="sent",
    )
    session = FakeSession([delivery], {command.id: command})

    asyncio.run(
        close_interrupted_telegram_deliveries(
            session, "controller-1", "new-boot", observed_at
        )
    )

    assert delivery.status == "dropped"
    assert delivery.update_sequence == 3
    assert delivery.error_stage == "controller_restart"
    assert delivery.updated_at == observed_at
    assert command.status == "failed"
    assert command.result == "Controller restarted during Telegram delivery"
