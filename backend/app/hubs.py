import asyncio
from dataclasses import dataclass
from time import monotonic
from typing import Any
from uuid import uuid4

from fastapi import WebSocket, WebSocketDisconnect


@dataclass
class ObservationRefresh:
    connection: WebSocket
    next_attempt: float = 0
    task: asyncio.Task[tuple[str, dict[str, Any]]] | None = None


class DeviceHub:
    def __init__(self) -> None:
        self._connections: dict[str, WebSocket] = {}
        self._lock = asyncio.Lock()
        self._pending: dict[str, tuple[WebSocket, str, asyncio.Future]] = {}
        # Observation slots live only as long as their online connection.
        self._observations: dict[str, ObservationRefresh] = {}

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
        return await self._request(websocket, device_id, message, reply_type, timeout)

    async def _request(self, websocket: WebSocket, device_id: str, message: dict[str, Any],
                       reply_type: str, timeout: float) -> dict[str, Any]:
        # Audited actions provide their stored command ID. Other requests still
        # receive an ID here, so every reply is bound to one pending operation.
        request_id = message.get("requestId") or str(uuid4())
        if not isinstance(request_id, str) or request_id in self._pending:
            raise ValueError("Invalid or already pending request ID")
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

    async def request_observation(self, device_id: str, message: dict[str, Any], reply_type: str,
                                  timeout: float = 1) -> tuple[str, dict[str, Any]] | None:
        """Share one read and limit attempts to every five seconds per connection.

        A read timeout only removes its backend waiter; it cannot remove bytes
        already sent to the controller. Keep the cooldown even after failure so
        polling and additional viewers cannot accumulate reads on a slow link.
        Mutating requests and their confirmation reads use request() directly.
        """
        async with self._lock:
            websocket = self._connections.get(device_id)
            if websocket is None:
                raise ConnectionError("Controller is offline")
            refresh = self._observations.get(device_id)
            if refresh is None:
                refresh = ObservationRefresh(websocket)
                self._observations[device_id] = refresh
            task = refresh.task
            if task is None or task.done():
                now = monotonic()
                if now < refresh.next_attempt:
                    return None
                refresh.next_attempt = now + 5
                request_id = message.get("requestId") or str(uuid4())
                task = asyncio.create_task(self._observe(
                    websocket, device_id, {**message, "requestId": request_id}, reply_type, timeout))
                refresh.task = task
                task.add_done_callback(lambda finished: self._finish_observation(refresh, finished))
        # An HTTP client leaving must not cancel a read shared by other viewers
        # or permit a second read while the first one remains on the wire.
        return await asyncio.shield(task)

    async def _observe(self, websocket: WebSocket, device_id: str, message: dict[str, Any],
                       reply_type: str, timeout: float) -> tuple[str, dict[str, Any]]:
        async with self._lock:
            if self._connections.get(device_id) is not websocket:
                raise ConnectionError("Controller connection changed")
        reply = await self._request(websocket, device_id, message, reply_type, timeout)
        return message["requestId"], reply

    @staticmethod
    def _finish_observation(refresh: ObservationRefresh, task: asyncio.Task) -> None:
        if refresh.task is task:
            refresh.task = None
        if not task.cancelled():
            task.exception()  # Consume failures even if every HTTP client left.

    async def register(self, device_id: str, websocket: WebSocket) -> None:
        async with self._lock:
            previous = self._connections.get(device_id)
            self._connections[device_id] = websocket
            if previous is not websocket:
                self._observations.pop(device_id, None)
        if previous is not None and previous is not websocket:
            self._disconnect_requests(previous)
            await previous.close(code=4002, reason="Replaced by a newer connection")

    async def unregister(self, device_id: str, websocket: WebSocket) -> None:
        self._disconnect_requests(websocket)
        async with self._lock:
            if self._connections.get(device_id) is websocket:
                self._connections.pop(device_id, None)
                self._observations.pop(device_id, None)

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
