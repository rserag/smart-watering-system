import asyncio
import csv
import io
from datetime import datetime, timezone

import pytest
from fastapi.testclient import TestClient

from app import history_export
from app.auth import require_user
from app.main import app


class Stream:
    def __init__(self, count=100005):
        self.count = count
        self.closed = False

    async def partitions(self, size):
        for start in range(0, self.count, size):
            yield [(datetime(2026, 9, 1, tzinfo=timezone.utc), "garden", 1, index,
                    index, 50, True, "monitoring", False, 0, 'fault, "quoted"' if index == 0 else None)
                   for index in range(start, min(start + size, self.count))]

    async def close(self):
        self.closed = True


class Session:
    def __init__(self, result):
        self.result = result
        self.closed = False

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        self.closed = True

    async def stream(self, query):
        assert "LIMIT" not in str(query)
        self.params = query.compile().params
        return self.result


def test_export_includes_rows_beyond_old_limit_and_escapes_csv(monkeypatch):
    session = Session(Stream())
    monkeypatch.setattr(history_export, "SessionLocal", lambda: session)
    app.dependency_overrides[require_user] = lambda: object()
    http = TestClient(app)
    try:
        response = http.get("/api/devices/garden/history.csv", params={
            "from": "2026-09-01T00:00:00Z", "to": "2026-09-08T00:00:00Z", "zone_id": 1,
        })
        assert response.status_code == 200
        rows = list(csv.reader(io.StringIO(response.text)))
        assert len(rows) == 100006
        assert rows[-1][3] == "100004"
        assert rows[1][-1] == 'fault, "quoted"'
        assert session.params["device_id_1"] == "garden"
        assert session.params["zone_id_1"] == 1
        assert session.closed and session.result.closed
    finally:
        http.close()
        app.dependency_overrides.clear()


def test_export_closes_database_cursor_when_cancelled(monkeypatch):
    session = Session(Stream())
    monkeypatch.setattr(history_export, "SessionLocal", lambda: session)

    async def run():
        stream = history_export.history_csv("garden", 1, datetime.now(timezone.utc), datetime.now(timezone.utc))
        await anext(stream)
        chunk = await anext(stream)
        assert len(chunk.splitlines()) == 1000
        await stream.aclose()
        assert session.closed and session.result.closed

    asyncio.run(run())


@pytest.mark.parametrize("start,end", [
    ("2026-09-08T00:00:00Z", "2026-09-01T00:00:00Z"),
    ("2024-01-01T00:00:00Z", "2026-01-01T00:00:00Z"),
    ("2026-09-01T00:00:00", "2026-09-08T00:00:00Z"),
])
def test_export_rejects_invalid_ranges(start, end):
    app.dependency_overrides[require_user] = lambda: object()
    http = TestClient(app)
    try:
        assert http.get("/api/devices/garden/history.csv", params={"from":start,"to":end}).status_code == 400
    finally:
        http.close()
        app.dependency_overrides.clear()


def test_export_requires_authentication():
    http = TestClient(app)
    try:
        assert http.get("/api/devices/garden/history.csv", params={
            "from":"2026-09-01T00:00:00Z", "to":"2026-09-08T00:00:00Z",
        }).status_code == 401
    finally:
        http.close()
