import asyncio
from datetime import datetime, timedelta, timezone

from app.main import update_main_tank_state
from app.models import MainTankState


class FakeSession:
    def __init__(self, state: MainTankState | None = None):
        self.state = state
        self.added: list[object] = []

    async def get(self, model: type[object], _: str) -> object | None:
        assert model is MainTankState
        return self.state

    def add(self, value: object) -> None:
        self.added.append(value)


def test_low_tank_state_is_tracked_without_backend_delivery() -> None:
    first_seen = datetime.now(timezone.utc)
    session = FakeSession()
    asyncio.run(update_main_tank_state(session, "controller-1", True, first_seen))

    state = next(item for item in session.added if isinstance(item, MainTankState))
    assert state.is_low is True

    repeated = FakeSession(state)
    asyncio.run(
        update_main_tank_state(
            repeated, "controller-1", True, first_seen + timedelta(seconds=5)
        )
    )
    assert not repeated.added

    restored = FakeSession(state)
    asyncio.run(
        update_main_tank_state(
            restored, "controller-1", False, first_seen + timedelta(minutes=1)
        )
    )
    assert not restored.added
    assert state.is_low is False
    assert state.last_changed_at == first_seen + timedelta(minutes=1)
