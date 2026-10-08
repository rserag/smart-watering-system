import asyncio
from typing import Any
from uuid import uuid4

from fastapi import WebSocket, WebSocketDisconnect


class DeviceHub:
    def __init__(self) -> None:
        self._connections: dict[str, WebSocket] = {}
        self._lock = asyncio.Lock()
        self._pending: dict[str, tuple[WebSocket, str, asyncio.Future]] = {}

    def _disconnect_requests(self, websocket: WebSocket) -> None:
        for connection, _, future in self._pending.values():
            if connection is websocket and not future.done():
                future.set_exception(ConnectionError("Controller disconnected"))

    def resolve_reply(self, websocket: WebSocket, message: dict[str, Any]) -> None:
        request_id = message.get("requestId")
        if not isinstance(request_id, str):
            return
        pending = self._pending.get(request_id)
        if pending is None:
            return
        connection, expected_type, future = pending
        if connection is websocket and message.get("type") == expected_type and not future.done():
            future.set_result(message)

    async def request(self, device_id: str, message: dict[str, Any], reply_type: str, timeout: float = 10) -> dict[str, Any]:
        async with self._lock:
            websocket = self._connections.get(device_id)
        if websocket is None:
            raise ConnectionError("Controller is offline")
        request_id = str(uuid4())
        future = asyncio.get_running_loop().create_future()
        self._pending[request_id] = (websocket, reply_type, future)
        try:
            async with asyncio.timeout(timeout):
                await websocket.send_json({**message, "deviceId": device_id, "schemaVersion": 1, "requestId": request_id})
                return await future
        except TimeoutError:
            raise
        except (RuntimeError, OSError, WebSocketDisconnect) as exc:
            raise ConnectionError("Controller disconnected") from exc
        finally:
            self._pending.pop(request_id, None)
            if not future.done():
                future.cancel()
            elif not future.cancelled():
                future.exception()  # Retrieve disconnect errors even when sending also failed.

    async def register(self, device_id: str, websocket: WebSocket) -> None:
        async with self._lock:
            previous = self._connections.get(device_id)
            self._connections[device_id] = websocket
        if previous is not None and previous is not websocket:
            self._disconnect_requests(previous)
            await previous.close(code=4002, reason="Replaced by a newer connection")

    async def unregister(self, device_id: str, websocket: WebSocket) -> None:
        self._disconnect_requests(websocket)
        async with self._lock:
            if self._connections.get(device_id) is websocket:
                self._connections.pop(device_id, None)

    async def send(self, device_id: str, message: dict[str, Any]) -> bool:
        async with self._lock:
            websocket = self._connections.get(device_id)
        if websocket is None:
            return False
        try:
            await websocket.send_json(message)
            return True
        except (RuntimeError, OSError, WebSocketDisconnect):
            await self.unregister(device_id, websocket)
            return False

    async def connected_ids(self) -> list[str]:
        async with self._lock:
            return sorted(self._connections)


class DashboardHub:
    def __init__(self) -> None:
        self._connections: set[WebSocket] = set()
        self._lock = asyncio.Lock()

    async def register(self, websocket: WebSocket) -> None:
        async with self._lock:
            self._connections.add(websocket)

    async def unregister(self, websocket: WebSocket) -> None:
        async with self._lock:
            self._connections.discard(websocket)

    async def broadcast(self, message: dict[str, Any]) -> None:
        async with self._lock:
            connections = list(self._connections)
        failed: list[WebSocket] = []
        for websocket in connections:
            try:
                await websocket.send_json(message)
            except (RuntimeError, OSError, WebSocketDisconnect):
                failed.append(websocket)
        for websocket in failed:
            await self.unregister(websocket)


device_hub = DeviceHub()
dashboard_hub = DashboardHub()
