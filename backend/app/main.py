import asyncio
import csv
import hmac
import io
import logging
from contextlib import asynccontextmanager
from datetime import datetime, timedelta, timezone
from typing import Annotated, Any
from uuid import uuid4

from fastapi import Depends, FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse
from pydantic import ValidationError
from sqlalchemy import select, text
from sqlalchemy.exc import IntegrityError
from sqlalchemy.ext.asyncio import AsyncSession
from starlette.middleware.sessions import SessionMiddleware

from app.auth import lookup_user_session, require_user, router as auth_router
from app.config import get_settings
from app.database import SessionLocal, engine, get_session
from app.hubs import dashboard_hub, device_hub
from app.models import (
    Base,
    Command,
    Device,
    MainTankState,
    TelemetrySample,
    TelegramDelivery,
    UserSession,
    WateringEvent,
    ZoneSample,
)
from app.protocol import (
    CommandAck,
    CommandRequest,
    DeviceHello,
    TelegramDeliveryMessage,
    TelemetryMessage,
)


logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s")
logger = logging.getLogger("watering-server")
settings = get_settings()


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


@asynccontextmanager
async def lifespan(_: FastAPI):
    async with engine.begin() as connection:
        await connection.run_sync(Base.metadata.create_all)
    yield
    await engine.dispose()


app = FastAPI(title="Watering System Server", version="0.4.0", lifespan=lifespan)
app.add_middleware(
    SessionMiddleware,
    secret_key=settings.session_secret,
    same_site="lax",
    https_only=True,
)
app.add_middleware(
    CORSMiddleware,
    allow_origins=settings.cors_origin_list,
    allow_credentials=True,
    allow_methods=["GET", "POST"],
    allow_headers=["Content-Type"],
)
app.include_router(auth_router)


@app.get("/health")
async def health(session: AsyncSession = Depends(get_session)) -> dict[str, Any]:
    await session.execute(text("SELECT 1"))
    return {
        "status": "ok",
        "database": "ok",
        "connectedDevices": await device_hub.connected_ids(),
        "authMode": settings.auth_mode,
        "serverConfigVersion": settings.server_config_version,
        "telegramDelivery": "device-direct",
    }


async def latest_snapshot(session: AsyncSession, device: Device) -> dict[str, Any]:
    latest_result = await session.execute(
        select(TelemetrySample)
        .where(TelemetrySample.device_id == device.id)
        .order_by(TelemetrySample.received_at.desc())
        .limit(1)
    )
    latest = latest_result.scalar_one_or_none()
    zones: list[dict[str, Any]] = []
    wifi_rssi: int | None = None
    telegram_status: dict[str, Any] = {
        "directTelegram": False,
        "telegramDebugEnabled": False,
        "telegramConfigured": False,
        "telegramPendingMessages": 0,
        "telegramLastSendSucceeded": False,
        "telegramWorkerRunning": False,
        "telegramTimeReady": False,
        "telegramLastFailureStage": None,
    }
    if latest is not None:
        wifi_rssi = latest.wifi_rssi
        telegram_status = {
            "directTelegram": bool(latest.payload.get("directTelegram", False)),
            "telegramDebugEnabled": bool(latest.payload.get("telegramDebugEnabled", False)),
            "telegramConfigured": bool(latest.payload.get("telegramConfigured", False)),
            "telegramPendingMessages": int(latest.payload.get("telegramPendingMessages", 0)),
            "telegramLastSendSucceeded": bool(latest.payload.get("telegramLastSendSucceeded", False)),
            "telegramWorkerRunning": bool(latest.payload.get("telegramWorkerRunning", False)),
            "telegramTimeReady": bool(latest.payload.get("telegramTimeReady", False)),
            "telegramLastFailureStage": latest.payload.get("telegramLastFailureStage"),
        }
        zone_result = await session.execute(
            select(ZoneSample).where(ZoneSample.telemetry_id == latest.id).order_by(ZoneSample.zone_id)
        )
        for zone in zone_result.scalars():
            event_result = await session.execute(
                select(WateringEvent)
                .where(WateringEvent.device_id == device.id, WateringEvent.zone_id == zone.zone_id)
                .order_by(WateringEvent.started_at.desc())
                .limit(1)
            )
            event = event_result.scalar_one_or_none()
            zones.append(
                {
                    "id": zone.zone_id,
                    "raw": zone.raw,
                    "filteredRaw": zone.filtered_raw,
                    "moisturePercent": zone.moisture_percent,
                    "sensorValid": zone.sensor_valid,
                    "phase": zone.phase,
                    "relayOn": zone.relay_on,
                    "wateringOnMsThisCycle": zone.watering_on_ms_this_cycle,
                    "fault": zone.fault,
                    "lastWateredAt": event.started_at.isoformat() if event else None,
                }
            )
    tank_state = await session.get(MainTankState, device.id)
    connected = device.id in await device_hub.connected_ids()
    return {
        "id": device.id,
        "online": connected,
        "firmwareVersion": device.firmware_version,
        "bootId": device.boot_id,
        "schemaVersion": device.schema_version,
        "configRevision": device.config_revision,
        "automaticWateringEnabled": device.automatic_watering_enabled,
        "lastSeenAt": device.last_seen_at.isoformat(),
        "wifiRssi": wifi_rssi,
        "mainTankLow": tank_state.is_low if tank_state is not None else None,
        "mainTankLastChangedAt": tank_state.last_changed_at.isoformat() if tank_state is not None else None,
        **telegram_status,
        "zones": zones,
    }


async def all_snapshots(session: AsyncSession) -> list[dict[str, Any]]:
    result = await session.execute(select(Device).order_by(Device.id))
    return [await latest_snapshot(session, device) for device in result.scalars()]


@app.get("/api/devices")
async def list_devices(
    _: Annotated[UserSession, Depends(require_user)],
    session: Annotated[AsyncSession, Depends(get_session)],
) -> list[dict[str, Any]]:
    return await all_snapshots(session)


@app.get("/api/devices/{device_id}/latest")
async def get_latest(
    device_id: str,
    _: Annotated[UserSession, Depends(require_user)],
    session: Annotated[AsyncSession, Depends(get_session)],
) -> dict[str, Any]:
    device = await session.get(Device, device_id)
    if device is None:
        raise HTTPException(status_code=404, detail="Unknown device")
    return await latest_snapshot(session, device)


def telegram_delivery_dict(delivery: TelegramDelivery) -> dict[str, Any]:
    return {
        "eventId": delivery.event_id,
        "deviceId": delivery.device_id,
        "requestId": delivery.request_id,
        "kind": delivery.kind,
        "status": delivery.status,
        "updateSequence": delivery.update_sequence,
        "attempt": delivery.attempt,
        "deviceUptimeMs": delivery.device_uptime_ms,
        "pendingCount": delivery.pending_count,
        "httpStatus": delivery.http_status,
        "errorStage": delivery.error_stage,
        "telegramErrorCode": delivery.telegram_error_code,
        "telegramMessageId": delivery.telegram_message_id,
        "firstObservedAt": delivery.first_observed_at.isoformat(),
        "updatedAt": delivery.updated_at.isoformat(),
        "sentAt": delivery.sent_at.isoformat() if delivery.sent_at else None,
    }


@app.get("/api/devices/{device_id}/telegram/deliveries")
async def get_telegram_deliveries(
    device_id: str,
    _: Annotated[UserSession, Depends(require_user)],
    session: Annotated[AsyncSession, Depends(get_session)],
    limit: int = Query(default=50, ge=1, le=200),
) -> list[dict[str, Any]]:
    if await session.get(Device, device_id) is None:
        raise HTTPException(status_code=404, detail="Unknown device")
    result = await session.execute(
        select(TelegramDelivery)
        .where(TelegramDelivery.device_id == device_id)
        .order_by(TelegramDelivery.updated_at.desc())
        .limit(limit)
    )
    return [telegram_delivery_dict(delivery) for delivery in result.scalars()]


HISTORY_METRICS = {
    "moisturePercent": "moisture_percent",
    "raw": "raw",
    "filteredRaw": "filtered_raw",
    "wateringOnMs": "watering_on_ms_this_cycle",
}


@app.get("/api/devices/{device_id}/history")
async def get_history(
    device_id: str,
    _: Annotated[UserSession, Depends(require_user)],
    session: Annotated[AsyncSession, Depends(get_session)],
    from_time: datetime = Query(alias="from"),
    to_time: datetime = Query(alias="to"),
    zone_id: int = Query(default=1, ge=1, le=16),
    metric: str = Query(default="moisturePercent"),
    bucket_seconds: int = Query(default=300, ge=10, le=86400),
) -> dict[str, Any]:
    if metric not in HISTORY_METRICS:
        raise HTTPException(status_code=400, detail="Unsupported metric")
    if to_time <= from_time or to_time - from_time > timedelta(days=366):
        raise HTTPException(status_code=400, detail="Invalid history range")
    column = HISTORY_METRICS[metric]
    query = text(
        f"""
        SELECT
          date_bin(INTERVAL '1 second' * :bucket_seconds, received_at, TIMESTAMPTZ '2000-01-01') AS bucket,
          AVG({column}) AS average,
          MIN({column}) AS minimum,
          MAX({column}) AS maximum,
          COUNT(*) AS samples
        FROM zone_samples
        WHERE device_id = :device_id AND zone_id = :zone_id
          AND received_at >= :from_time AND received_at <= :to_time
        GROUP BY bucket
        ORDER BY bucket
        """
    )
    result = await session.execute(
        query,
        {
            "bucket_seconds": bucket_seconds,
            "device_id": device_id,
            "zone_id": zone_id,
            "from_time": from_time,
            "to_time": to_time,
        },
    )
    return {
        "deviceId": device_id,
        "zoneId": zone_id,
        "metric": metric,
        "bucketSeconds": bucket_seconds,
        "points": [
            {
                "timestamp": row.bucket.isoformat(),
                "average": float(row.average),
                "minimum": float(row.minimum),
                "maximum": float(row.maximum),
                "samples": row.samples,
            }
            for row in result
        ],
    }


@app.get("/api/devices/{device_id}/events")
async def get_events(
    device_id: str,
    _: Annotated[UserSession, Depends(require_user)],
    session: Annotated[AsyncSession, Depends(get_session)],
    from_time: datetime = Query(alias="from"),
    to_time: datetime = Query(alias="to"),
    zone_id: int | None = Query(default=None, ge=1, le=16),
) -> list[dict[str, Any]]:
    statement = select(WateringEvent).where(
        WateringEvent.device_id == device_id,
        WateringEvent.started_at >= from_time,
        WateringEvent.started_at <= to_time,
    )
    if zone_id is not None:
        statement = statement.where(WateringEvent.zone_id == zone_id)
    result = await session.execute(statement.order_by(WateringEvent.started_at.desc()).limit(500))
    return [
        {
            "id": event.id,
            "zoneId": event.zone_id,
            "startedAt": event.started_at.isoformat(),
            "endedAt": event.ended_at.isoformat() if event.ended_at else None,
            "durationMs": event.duration_ms,
            "source": event.source,
            "status": event.status,
        }
        for event in result.scalars()
    ]


@app.get("/api/devices/{device_id}/history.csv")
async def export_history(
    device_id: str,
    _: Annotated[UserSession, Depends(require_user)],
    session: Annotated[AsyncSession, Depends(get_session)],
    from_time: datetime = Query(alias="from"),
    to_time: datetime = Query(alias="to"),
    zone_id: int | None = Query(default=None, ge=1, le=16),
) -> StreamingResponse:
    statement = select(ZoneSample).where(
        ZoneSample.device_id == device_id,
        ZoneSample.received_at >= from_time,
        ZoneSample.received_at <= to_time,
    )
    if zone_id is not None:
        statement = statement.where(ZoneSample.zone_id == zone_id)
    result = await session.execute(statement.order_by(ZoneSample.received_at).limit(100000))
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(["received_at", "device_id", "zone_id", "raw", "filtered_raw", "moisture_percent", "sensor_valid", "phase", "relay_on", "watering_ms", "fault"])
    for row in result.scalars():
        writer.writerow([row.received_at.isoformat(), row.device_id, row.zone_id, row.raw, row.filtered_raw, row.moisture_percent, row.sensor_valid, row.phase, row.relay_on, row.watering_on_ms_this_cycle, row.fault or ""])
    return StreamingResponse(
        iter([output.getvalue()]),
        media_type="text/csv",
        headers={"Content-Disposition": f'attachment; filename="{device_id}-history.csv"'},
    )


def command_wire_message(command: Command) -> dict[str, Any]:
    payload = {
        "type": command.command,
        "schemaVersion": 1,
        "deviceId": command.device_id,
        "requestId": command.id,
        **command.parameters,
    }
    if command.command in {"zone.water", "telegram.debug.send"} and "expiresAtEpoch" not in payload:
        payload["expiresAtEpoch"] = int(utcnow().timestamp()) + 60
    return payload


@app.post("/api/devices/{device_id}/commands", status_code=202)
async def create_command(
    device_id: str,
    request: CommandRequest,
    _: Annotated[UserSession, Depends(require_user)],
    session: Annotated[AsyncSession, Depends(get_session)],
) -> dict[str, Any]:
    if await session.get(Device, device_id) is None:
        raise HTTPException(status_code=404, detail="Unknown device")
    command = Command(id=str(uuid4()), device_id=device_id, command=request.command, parameters=request.parameters, status="queued")
    session.add(command)
    await session.commit()
    if await device_hub.send(device_id, command_wire_message(command)):
        command.status = "sent"
        command.sent_at = utcnow()
        await session.commit()
    return {"commandId": command.id, "status": command.status}


@app.post("/api/devices/{device_id}/telegram/debug", status_code=202)
async def send_telegram_debug(
    device_id: str,
    _: Annotated[UserSession, Depends(require_user)],
    session: Annotated[AsyncSession, Depends(get_session)],
) -> dict[str, Any]:
    if await session.get(Device, device_id) is None:
        raise HTTPException(status_code=404, detail="Unknown device")
    if device_id not in await device_hub.connected_ids():
        raise HTTPException(status_code=409, detail="Controller is offline")
    command = Command(
        id=str(uuid4()),
        device_id=device_id,
        command="telegram.debug.send",
        parameters={"expiresAtEpoch": int(utcnow().timestamp()) + 60},
        status="queued",
    )
    session.add(command)
    await session.commit()
    if not await device_hub.send(device_id, command_wire_message(command)):
        command.status = "failed"
        command.result = "Controller disconnected before command delivery"
        await session.commit()
        raise HTTPException(status_code=409, detail="Controller disconnected")
    command.status = "sent"
    command.sent_at = utcnow()
    await session.commit()
    return {"commandId": command.id, "status": command.status}


async def deliver_queued_commands(device_id: str) -> None:
    async with SessionLocal() as session:
        result = await session.execute(
            select(Command).where(Command.device_id == device_id, Command.status == "queued").order_by(Command.created_at)
        )
        for command in result.scalars():
            if not await device_hub.send(device_id, command_wire_message(command)):
                break
            command.status = "sent"
            command.sent_at = utcnow()
        await session.commit()


async def record_hello(hello: DeviceHello) -> None:
    async with SessionLocal() as session:
        device = await session.get(Device, hello.device_id)
        if device is None:
            device = Device(id=hello.device_id)
            session.add(device)
        device.firmware_version = hello.firmware_version
        device.boot_id = hello.boot_id
        device.schema_version = hello.schema_version
        device.config_revision = hello.config_revision
        device.automatic_watering_enabled = hello.automatic_watering_enabled
        device.last_seen_at = utcnow()
        await session.commit()


async def update_watering_event(
    session: AsyncSession,
    device_id: str,
    zone_id: int,
    relay_on: bool,
    received_at: datetime,
) -> None:
    previous_result = await session.execute(
        select(ZoneSample)
        .where(ZoneSample.device_id == device_id, ZoneSample.zone_id == zone_id)
        .order_by(ZoneSample.received_at.desc())
        .limit(1)
    )
    previous = previous_result.scalar_one_or_none()
    previous_on = previous.relay_on if previous is not None else False
    if relay_on and not previous_on:
        session.add(WateringEvent(device_id=device_id, zone_id=zone_id, started_at=received_at, source="automatic", status="running"))
    elif not relay_on and previous_on:
        event_result = await session.execute(
            select(WateringEvent)
            .where(WateringEvent.device_id == device_id, WateringEvent.zone_id == zone_id, WateringEvent.status == "running")
            .order_by(WateringEvent.started_at.desc())
            .limit(1)
        )
        event = event_result.scalar_one_or_none()
        if event is not None:
            event.ended_at = received_at
            event.duration_ms = max(0, int((received_at - event.started_at).total_seconds() * 1000))
            event.status = "completed"


async def update_main_tank_state(
    session: AsyncSession,
    device_id: str,
    is_low: bool | None,
    received_at: datetime,
) -> None:
    if is_low is None:
        return

    state = await session.get(MainTankState, device_id)
    if state is None:
        session.add(
            MainTankState(
                device_id=device_id,
                is_low=is_low,
                last_changed_at=received_at,
                last_reported_at=received_at,
            )
        )
        return

    state.last_reported_at = received_at
    if state.is_low != is_low:
        state.is_low = is_low
        state.last_changed_at = received_at


async def record_telemetry(message: TelemetryMessage) -> dict[str, Any] | None:
    received_at = utcnow()
    async with SessionLocal() as session:
        device = await session.get(Device, message.device_id)
        if device is None:
            return None
        device.boot_id = message.boot_id
        device.config_revision = message.config_revision
        device.last_seen_at = received_at
        sample = TelemetrySample(
            device_id=message.device_id,
            boot_id=message.boot_id,
            sequence=message.sequence,
            uptime_ms=message.uptime_ms,
            config_revision=message.config_revision,
            wifi_rssi=message.wifi_rssi,
            received_at=received_at,
            payload=message.model_dump(by_alias=True),
        )
        session.add(sample)
        try:
            await session.flush()
            await update_main_tank_state(
                session, message.device_id, message.main_tank_low, received_at
            )
            for zone in message.zones:
                await update_watering_event(session, message.device_id, zone.id, zone.relay_on, received_at)
                session.add(
                    ZoneSample(
                        telemetry_id=sample.id,
                        device_id=message.device_id,
                        zone_id=zone.id,
                        received_at=received_at,
                        raw=zone.raw,
                        filtered_raw=zone.filtered_raw,
                        moisture_percent=zone.relative_moisture_percent,
                        sensor_valid=zone.sensor_valid,
                        phase=zone.phase,
                        relay_on=zone.relay_on,
                        watering_on_ms_this_cycle=zone.watering_on_ms_this_cycle,
                        fault=zone.fault,
                    )
                )
            await session.commit()
        except IntegrityError:
            await session.rollback()
            return None
        device = await session.get(Device, message.device_id)
        return await latest_snapshot(session, device)


async def record_command_ack(device_id: str, message: CommandAck) -> None:
    async with SessionLocal() as session:
        command = await session.get(Command, message.request_id)
        if command is None or command.device_id != device_id:
            return
        command.status = "acknowledged" if message.status in {"accepted", "duplicate"} else "rejected"
        command.result = message.message
        command.acknowledged_at = utcnow()
        await session.commit()


async def record_telegram_delivery(
    device_id: str, message: TelegramDeliveryMessage
) -> dict[str, Any] | None:
    observed_at = utcnow()
    async with SessionLocal() as session:
        if await session.get(Device, device_id) is None:
            return None
        delivery = await session.get(TelegramDelivery, message.event_id)
        applied = False
        if delivery is None:
            delivery = TelegramDelivery(
                event_id=message.event_id,
                device_id=device_id,
                request_id=message.request_id,
                kind=message.kind,
                status=message.status,
                update_sequence=message.update_sequence,
                attempt=message.attempt,
                device_uptime_ms=message.uptime_ms,
                pending_count=message.pending_count,
                http_status=message.http_status,
                error_stage=message.error_stage,
                telegram_error_code=message.telegram_error_code,
                telegram_message_id=message.telegram_message_id,
                first_observed_at=observed_at,
                updated_at=observed_at,
                sent_at=observed_at if message.status == "sent" else None,
            )
            session.add(delivery)
            applied = True
        elif message.update_sequence >= delivery.update_sequence:
            delivery.request_id = message.request_id or delivery.request_id
            delivery.kind = message.kind
            delivery.status = message.status
            delivery.update_sequence = message.update_sequence
            delivery.attempt = message.attempt
            delivery.device_uptime_ms = message.uptime_ms
            delivery.pending_count = message.pending_count
            delivery.http_status = message.http_status
            delivery.error_stage = message.error_stage
            delivery.telegram_error_code = message.telegram_error_code
            delivery.telegram_message_id = message.telegram_message_id
            delivery.updated_at = observed_at
            if message.status == "sent":
                delivery.sent_at = observed_at
            applied = True
        if applied and message.request_id:
            command = await session.get(Command, message.request_id)
            if command is not None and command.device_id == device_id:
                command.result = f"Telegram delivery {message.status}"
        await session.commit()
        return telegram_delivery_dict(delivery)


def valid_device_authorization(websocket: WebSocket) -> bool:
    supplied = websocket.headers.get("authorization", "")
    expected = f"Bearer {settings.device_shared_token}"
    return hmac.compare_digest(supplied, expected)


@app.websocket("/ws/device")
async def device_websocket(websocket: WebSocket) -> None:
    await websocket.accept()
    if not valid_device_authorization(websocket):
        await websocket.close(code=4001, reason="Authentication failed")
        return
    device_id: str | None = None
    try:
        try:
            hello = DeviceHello.model_validate(
                await asyncio.wait_for(websocket.receive_json(), timeout=settings.hello_timeout_seconds)
            )
        except (TimeoutError, ValidationError, ValueError):
            await websocket.close(code=4000, reason="A valid device.hello message is required")
            return
        device_id = hello.device_id
        await record_hello(hello)
        await device_hub.register(device_id, websocket)
        await websocket.send_json(
            {
                "type": "device.ready",
                "schemaVersion": hello.schema_version,
                "deviceId": device_id,
                "serverTime": utcnow().isoformat(),
                "configRevision": settings.server_config_version,
            }
        )
        await deliver_queued_commands(device_id)
        async with SessionLocal() as session:
            device = await session.get(Device, device_id)
            await dashboard_hub.broadcast({"type": "device.status", "device": await latest_snapshot(session, device)})
        logger.info("device connected device_id=%s", device_id)

        while True:
            raw = await websocket.receive_json()
            message_type = raw.get("type") if isinstance(raw, dict) else None
            try:
                if message_type == "telemetry":
                    telemetry = TelemetryMessage.model_validate(raw)
                    if telemetry.device_id != device_id:
                        await websocket.close(code=4003, reason="Device ID changed")
                        return
                    snapshot = await record_telemetry(telemetry)
                    if snapshot is not None:
                        await dashboard_hub.broadcast({"type": "telemetry", "device": snapshot})
                elif message_type == "command.ack":
                    ack = CommandAck.model_validate(raw)
                    if ack.device_id == device_id:
                        await record_command_ack(device_id, ack)
                elif message_type == "telegram.delivery":
                    delivery_message = TelegramDeliveryMessage.model_validate(raw)
                    if delivery_message.device_id != device_id:
                        await websocket.close(code=4003, reason="Device ID changed")
                        return
                    delivery = await record_telegram_delivery(device_id, delivery_message)
                    await websocket.send_json(
                        {
                            "type": "telegram.delivery.ack",
                            "schemaVersion": delivery_message.schema_version,
                            "deviceId": device_id,
                            "eventId": delivery_message.event_id,
                            "updateSequence": delivery_message.update_sequence,
                        }
                    )
                    if delivery is not None:
                        await dashboard_hub.broadcast(
                            {"type": "telegram.delivery", "delivery": delivery}
                        )
                elif message_type in {"config.ack", "config.snapshot"}:
                    logger.info("device configuration message device_id=%s type=%s", device_id, message_type)
                else:
                    logger.warning("unsupported device message device_id=%s type=%s", device_id, message_type)
            except ValidationError as exc:
                logger.warning("invalid device message device_id=%s errors=%s", device_id, exc.errors(include_url=False))
    except (WebSocketDisconnect, ValueError):
        pass
    finally:
        if device_id is not None:
            await device_hub.unregister(device_id, websocket)
            async with SessionLocal() as session:
                device = await session.get(Device, device_id)
                if device is not None:
                    await dashboard_hub.broadcast({"type": "device.status", "device": await latest_snapshot(session, device)})
            logger.info("device disconnected device_id=%s", device_id)


@app.websocket("/ws/dashboard")
async def dashboard_websocket(websocket: WebSocket) -> None:
    raw_token = websocket.cookies.get(settings.session_cookie_name)
    async with SessionLocal() as session:
        user = await lookup_user_session(session, raw_token)
        if user is None:
            await websocket.accept()
            await websocket.close(code=4401, reason="Authentication required")
            return
        snapshots = await all_snapshots(session)
    await websocket.accept()
    await dashboard_hub.register(websocket)
    await websocket.send_json({"type": "snapshot", "devices": snapshots})
    try:
        while True:
            message = await websocket.receive_json()
            if isinstance(message, dict) and message.get("type") == "ping":
                await websocket.send_json({"type": "pong", "serverTime": utcnow().isoformat()})
    except (WebSocketDisconnect, ValueError):
        pass
    finally:
        await dashboard_hub.unregister(websocket)
