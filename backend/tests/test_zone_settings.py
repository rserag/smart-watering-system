from unittest.mock import AsyncMock

import pytest
from fastapi.testclient import TestClient

from app.auth import require_user
from app.database import get_session
from app.main import app, dashboard_hub
from app.models import Device


class Session:
    def __init__(self):
        self.device_exists = True
        self.zone_exists = True
        self.writes = []
        self.committed = False

    async def get(self, model, device_id):
        assert model is Device
        return Device(id=device_id) if self.device_exists else None

    async def scalar(self, query):
        self.lookup = query.compile().params
        return 100 if self.zone_exists else None

    async def execute(self, query):
        self.writes.append(query.compile().params)

    async def commit(self):
        self.committed = True


@pytest.fixture
def client(monkeypatch):
    session = Session()
    broadcast = AsyncMock()

    async def session_dependency():
        yield session

    app.dependency_overrides[get_session] = session_dependency
    app.dependency_overrides[require_user] = lambda: object()
    monkeypatch.setattr(dashboard_hub, "broadcast", broadcast)
    test_client = TestClient(app)
    try:
        yield test_client, session, broadcast
    finally:
        test_client.close()
        app.dependency_overrides.clear()


def test_name_is_trimmed_saved_for_the_device_and_broadcast_after_commit(client):
    http, session, broadcast = client

    async def check_commit(message):
        assert session.committed
        assert message == {"type": "zone.updated", "deviceId": "garden-2", "zone": {"id": 3, "name": "Լոլիկ 🍅"}}

    broadcast.side_effect = check_commit
    response = http.patch("/api/devices/garden-2/zones/3", json={"name": "  Լոլիկ 🍅  "})
    assert response.status_code == 200
    assert response.json() == {"id": 3, "name": "Լոլիկ 🍅"}
    assert session.lookup["device_id_1"] == "garden-2"
    assert session.lookup["zone_id_1"] == 3
    assert session.writes[0]["device_id"] == "garden-2"
    assert session.writes[0]["zone_id"] == 3
    assert session.writes[0]["name"] == "Լոլիկ 🍅"
    broadcast.assert_awaited_once()


def test_blank_name_resets_the_label_without_deleting_its_settings(client):
    http, session, broadcast = client
    response = http.patch("/api/devices/garden-2/zones/3", json={"name": "   "})
    assert response.status_code == 200
    assert response.json() == {"id": 3, "name": None}
    assert session.writes[0]["name"] is None
    broadcast.assert_awaited_once()


@pytest.mark.parametrize("payload", [
    {"name": "a" * 61}, {"name": "Tom\x00atoes"}, {"name": "one\ntwo"},
    {"name": None}, {"name": 123}, {"name": "Tomatoes", "relayOn": True}, {},
])
def test_invalid_names_and_unrelated_changes_are_rejected(client, payload):
    http, session, broadcast = client
    assert http.patch("/api/devices/garden-2/zones/3", json=payload).status_code == 422
    assert not session.writes
    broadcast.assert_not_awaited()


@pytest.mark.parametrize("path", ["0", "17", "invalid"])
def test_invalid_zone_id_is_rejected(client, path):
    http, session, broadcast = client
    assert http.patch(f"/api/devices/garden-2/zones/{path}", json={"name": "Herbs"}).status_code == 422
    assert not session.writes
    broadcast.assert_not_awaited()


@pytest.mark.parametrize("missing", ["device_exists", "zone_exists"])
def test_unknown_device_or_zone_cannot_create_names(client, missing):
    http, session, broadcast = client
    setattr(session, missing, False)
    assert http.patch("/api/devices/garden-2/zones/3", json={"name": "Herbs"}).status_code == 404
    assert not session.writes
    broadcast.assert_not_awaited()


def test_unauthenticated_request_cannot_rename_a_zone(client):
    http, session, broadcast = client
    del app.dependency_overrides[require_user]
    assert http.patch("/api/devices/garden-2/zones/3", json={"name": "Herbs"}).status_code == 401
    assert not session.writes
    broadcast.assert_not_awaited()
