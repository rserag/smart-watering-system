import asyncio
from typing import Any

from fastapi import WebSocket, WebSocketDisconnect


class DeviceHub:
    def __init__(self) -> None:
        self._connections: dict[str, WebSocket] = {}
        self._lock = asyncio.Lock()

    async def register(self, device_id: str, websocket: WebSocket) -> None:
        async with self._lock:
            previous = self._connections.get(device_id)
            self._connections[device_id] = websocket
        if previous is not None and previous is not websocket:
            await previous.close(code=4002, reason="Replaced by a newer connection")

    async def unregister(self, device_id: str, websocket: WebSocket) -> None:
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

