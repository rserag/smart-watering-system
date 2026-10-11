"""Authenticated observation and owner-only, online controller test sessions.

The controller owns the output cutoff and lease. Backend deadlines/locks are
additional checks; a lost ACK is uncertain and must never cause a retry.
"""
import asyncio
from collections import defaultdict, deque
from contextlib import asynccontextmanager
from copy import deepcopy
from datetime import datetime, timedelta, timezone
from typing import Annotated, Any, Literal
from uuid import NAMESPACE_URL, UUID, uuid4, uuid5

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator
from sqlalchemy import delete, select, func
from sqlalchemy.dialects.postgresql import insert
from sqlalchemy.ext.asyncio import AsyncSession

from app.auth import require_user
from app.database import get_session
from app.diagnostics_protocol import DebugAction, DebugSession, DebugSnapshot
from app.hubs import device_hub
from app.models import Device, DeviceConfiguration, DiagnosticAudit, DiagnosticCapture, DiagnosticState, TelemetrySample, UserSession
from app.protocol import CommandAck


OPERATOR_EMAIL = "sahakyan.raphayel@gmail.com"
MAX_AGE = timedelta(seconds=10)
RING_SIZE = 600
AUDIT_DAYS = 30
CAPTURE_DAYS = 7
router = APIRouter(prefix="/api/devices/{device_id}", tags=["diagnostics"])
# DeviceHub is deliberately a single-process transport. These bounded caches
# follow the same architecture; no session tokens enter dashboard broadcasts.
controller_states: dict[str, dict[str, Any]] = {}
snapshot_cache: dict[str, tuple[dict[str, Any], datetime]] = {}
snapshot_rings: dict[str, deque] = defaultdict(lambda: deque(maxlen=RING_SIZE))
command_locks: dict[str, asyncio.Lock] = defaultdict(asyncio.Lock)


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def is_operator(user: UserSession) -> bool:
    return getattr(user, "email", "").casefold() == OPERATOR_EMAIL


def require_operator(user: Annotated[UserSession, Depends(require_user)]) -> UserSession:
    if not is_operator(user):
        raise HTTPException(403, "Only the controller owner may issue diagnostic commands.")
    return user


def redact(value: Any, can_control: bool) -> Any:
    """Captures/audits can also contain nested session IDs, not only snapshots."""
    if isinstance(value, dict):
        return {key: None if key in {"sessionId", "session_id"} and not can_control else redact(item, can_control)
                for key, item in value.items()}
    if isinstance(value, list):
        return [redact(item, can_control) for item in value]
    return value


def remember_state(device_id: str, boot_id: str, version: int | None, state: DebugSession | None, received_at: datetime) -> None:
    previous = controller_states.get(device_id)
    if previous is not None and previous["bootId"] != boot_id:
        snapshot_cache.pop(device_id, None)
    controller_states[device_id] = {"bootId": boot_id, "version": version,
                                    "state": state.model_dump() if state else {}, "receivedAt": received_at}


async def store_state(session: AsyncSession, device_id: str, boot_id: str, version: int | None,
                      state: DebugSession | None, received_at: datetime) -> None:
    stored = await session.get(DiagnosticState, device_id)
    new_state = state.model_dump() if state else {}
    if isinstance(stored, DiagnosticState) and stored.state.get("sessionId") and (
            stored.boot_id != boot_id or stored.state.get("sessionId") != new_state.get("sessionId")):
        previous_session = stored.state["sessionId"]
        event_id = str(uuid5(NAMESPACE_URL, f"{device_id}/{stored.boot_id}/{previous_session}/ended/{boot_id}"))
        if await session.get(DiagnosticAudit, event_id) is None:
            session.add(DiagnosticAudit(id=event_id, device_id=device_id, boot_id=stored.boot_id,
                session_id=previous_session, request_id=event_id, record_type="session", command="debug.session.transition",
                status="cancelled", actor=stored.state.get("owner"), parameters={}, result=new_state,
                message="Controller session ended or rebooted; review automation hold before resuming.",
                created_at=received_at, updated_at=received_at))
    remember_state(device_id, boot_id, version, state, received_at)
    values = {"boot_id": boot_id, "version": version, "state": new_state, "received_at": received_at}
    await session.execute(insert(DiagnosticState).values(device_id=device_id, **values).on_conflict_do_update(
        index_elements=["device_id"], set_=values))


async def reported_state(session: AsyncSession, device_id: str) -> dict[str, Any]:
    current = controller_states.get(device_id)
    if current is not None:
        return current
    stored = await session.get(DiagnosticState, device_id)
    if isinstance(stored, DiagnosticState):
        return {"bootId": stored.boot_id, "version": stored.version, "state": stored.state, "receivedAt": stored.received_at}
    return {"bootId": None, "version": None, "state": {}}


async def guard_conflicting_write(session: AsyncSession, device_id: str, user: UserSession | None,
                                  command: str, *, queued: bool = False) -> None:
    state = (await reported_state(session, device_id))["state"]
    if not state.get("automationHeld") and state.get("mode", "observe") == "observe":
        return
    if queued:
        raise HTTPException(409, "Queued control was cancelled because diagnostics holds this controller.")
    if user is None or not is_operator(user):
        raise HTTPException(403, "Only the controller owner may control a controller held by diagnostics.")
    if command in {"zone.stop", "system.stopAll", "telemetry.request", "config.get"}:
        return
    raise HTTPException(409, "End diagnostics and explicitly resume automation before changing watering or configuration.")


def remember_snapshot(message: DebugSnapshot, received_at: datetime | None = None) -> bool:
    received_at = received_at or utcnow()
    state = controller_states.get(message.deviceId)
    if state is not None and state["bootId"] != message.bootId:
        return False
    previous = snapshot_cache.get(message.deviceId)
    if previous and previous[0]["bootId"] == message.bootId and message.sequence <= previous[0]["sequence"]:
        return False
    data = message.model_dump()
    snapshot_cache[message.deviceId] = (data, received_at)
    snapshot_rings[message.deviceId].append({"snapshot": deepcopy(data), "receivedAt": received_at.isoformat()})
    remember_state(message.deviceId, message.bootId, 1, message.session, received_at)
    return True


async def record_snapshot(session: AsyncSession, message: DebugSnapshot) -> bool:
    if not remember_snapshot(message):
        return False
    await store_state(session, message.deviceId, message.bootId, 1, message.session,
                      snapshot_cache[message.deviceId][1])
    await session.commit()
    return True


async def record_action(session: AsyncSession, event: DebugAction) -> None:
    original = await session.get(DiagnosticAudit, event.requestId)
    # A lifecycle event is evidence only for one audited request from this boot
    # and lease. Never attach an unsolicited event to a different user's action.
    if original is None or original.device_id != event.deviceId or original.boot_id != event.bootId:
        return
    if original.command != event.command or original.session_id != event.sessionId:
        return
    event_id = str(uuid5(NAMESPACE_URL, f"{event.deviceId}/{event.bootId}/{event.requestId}/{event.status}/{event.uptimeMs}"))
    if await session.get(DiagnosticAudit, event_id) is not None:
        return
    at = utcnow()
    data = event.model_dump()
    session.add(DiagnosticAudit(id=event_id, device_id=event.deviceId, boot_id=event.bootId,
        session_id=event.sessionId, request_id=event.requestId, record_type="event", command=event.command,
        status=event.status, actor=original.actor, parameters={}, result=data, message=event.message,
        created_at=at, updated_at=at))
    if original.status not in {"completed", "cancelled"}:
        original.status = event.status
        original.result = data
        original.message = event.message
        original.updated_at = at
    await session.commit()


async def purge_diagnostics(session: AsyncSession, now: datetime | None = None) -> None:
    """Each pass removes at most 500 rows/table; historical watering is untouched."""
    now = now or utcnow()
    for model, days in ((DiagnosticAudit, AUDIT_DAYS), (DiagnosticCapture, CAPTURE_DAYS)):
        old_ids = select(model.id).where(model.created_at < now - timedelta(days=days)).order_by(model.created_at).limit(500)
        await session.execute(delete(model).where(model.id.in_(old_ids)))
    await session.commit()


class StrictRequest(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)


class BootRequest(StrictRequest):
    expectedBootId: str = Field(min_length=1, max_length=100)


class SessionStart(BootRequest):
    mode: Literal["hardware", "simulation"]


class SessionRequest(BootRequest):
    sessionId: str = Field(min_length=1, max_length=100)

    @model_validator(mode="after")
    def validate_id(self):
        UUID(self.sessionId)
        return self


class PulseParameters(StrictRequest):
    zoneId: int = Field(ge=1, le=4)
    durationMs: int = Field(ge=100, le=60000)


class StopParameters(StrictRequest):
    zoneId: int | None = Field(default=None, ge=1, le=4)


class InputParameters(StrictRequest):
    input: Literal["soil", "tank"]
    zoneId: int | None = Field(default=None, ge=1, le=4)
    raw: int | None = Field(default=None, ge=0, le=4095)
    low: bool | None = None

    @model_validator(mode="after")
    def validate_input(self):
        if self.input == "soil" and (self.zoneId is None or self.raw is None or self.low is not None):
            raise ValueError("Soil input requires zoneId and raw only")
        if self.input == "tank" and (self.low is None or self.zoneId is not None or self.raw is not None):
            raise ValueError("Tank input requires low only")
        return self


class ClearParameters(StrictRequest):
    input: Literal["soil", "tank", "all"]
    zoneId: int | None = Field(default=None, ge=1, le=4)

    @model_validator(mode="after")
    def validate_input(self):
        if self.input != "soil" and self.zoneId is not None:
            raise ValueError("zoneId applies only to soil")
        return self


class OledParameters(StrictRequest):
    pattern: Literal["checkerboard", "text", "off"]
    durationMs: int = Field(ge=100, le=10000)


PARAMETERS = {"debug.automation.resume": StrictRequest, "debug.output.pulse": PulseParameters,
              "debug.output.stop": StopParameters, "debug.input.set": InputParameters,
              "debug.input.clear": ClearParameters, "debug.sample": StrictRequest,
              "debug.oled.test": OledParameters, "debug.i2c.scan": StrictRequest}


class ActionRequest(BootRequest):
    command: Literal["debug.automation.resume", "debug.output.pulse", "debug.output.stop", "debug.input.set",
                     "debug.input.clear", "debug.sample", "debug.oled.test", "debug.i2c.scan"]
    sessionId: str | None = Field(default=None, max_length=100)
    parameters: dict[str, Any] = Field(default_factory=dict)

    @model_validator(mode="after")
    def validate_parameters(self):
        if self.sessionId is not None:
            UUID(self.sessionId)
        self.parameters = PARAMETERS[self.command].model_validate(self.parameters).model_dump(exclude_none=True)
        return self


class CaptureRequest(StrictRequest):
    name: str = Field(min_length=1, max_length=80)

    @model_validator(mode="after")
    def clean_name(self):
        self.name = self.name.strip()
        if not self.name:
            raise ValueError("Capture name is required")
        return self


async def device_or_404(session: AsyncSession, device_id: str) -> Device:
    device = await session.get(Device, device_id)
    if device is None:
        raise HTTPException(404, "Unknown device")
    return device


def wire(command: str, device_id: str, boot_id: str, request_id: str, parameters: dict) -> dict:
    return {**parameters, "type": command, "deviceId": device_id, "schemaVersion": 1,
            "expectedBootId": boot_id, "requestId": request_id, "expiresAtEpoch": int(utcnow().timestamp()) + 30}


async def read_snapshot(device: Device, timeout: float = 2) -> tuple[dict, datetime]:
    request_id = str(uuid4())
    raw = await device_hub.request(device.id, wire("debug.snapshot.get", device.id, device.boot_id, request_id, {}),
                                   "debug.snapshot", timeout=timeout)
    message = DebugSnapshot.model_validate(raw)
    if message.deviceId != device.id or message.bootId != device.boot_id or message.requestId != request_id:
        raise ValueError("Controller returned a snapshot for a different boot or request")
    remember_snapshot(message)
    cached = snapshot_cache.get(device.id)
    if cached is None or cached[0]["bootId"] != device.boot_id:
        raise ValueError("Current diagnostics are unavailable")
    return cached


async def current_control(session: AsyncSession, device_id: str, expected_boot: str,
                          allow_stale: bool = False) -> tuple[Device, dict]:
    device = await device_or_404(session, device_id)
    state = await reported_state(session, device_id)
    if state["version"] != 1:
        raise HTTPException(409, "This firmware does not support diagnostic controls. Install diagnostics firmware first.")
    if device_id not in await device_hub.connected_ids():
        raise HTTPException(409, "Controller is offline; diagnostic commands are never queued.")
    if device.boot_id != expected_boot or state["bootId"] != expected_boot:
        raise HTTPException(409, "The controller restarted. Review its current state before trying again.")
    if not allow_stale:
        cached = snapshot_cache.get(device_id)
        if cached is None or cached[0]["bootId"] != expected_boot or utcnow() - cached[1] > MAX_AGE:
            try:
                await read_snapshot(device)
            except (ConnectionError, TimeoutError, ValidationError, ValueError) as exc:
                raise HTTPException(409, "Fresh controller diagnostics are unavailable. Wait for current readings.") from exc
        cached = snapshot_cache[device_id]
        age = utcnow() - cached[1]
        if age < timedelta(seconds=-5) or age > MAX_AGE:
            raise HTTPException(409, "Controller diagnostics are stale. Wait for current readings.")
        state = await reported_state(session, device_id)
    return device, state["state"]


def validate_lease(state: dict, session_id: str | None, *, optional: bool = False) -> None:
    active = state.get("mode", "observe") != "observe"
    if not active:
        if optional and session_id is None:
            return
        raise HTTPException(409, "The diagnostic session ended. Start a new session.")
    if state.get("sessionId") != session_id or state.get("owner") != OPERATOR_EMAIL:
        raise HTTPException(409, "The diagnostic session changed. Refresh before issuing commands.")
    if not optional and state.get("leaseRemainingMs", 0) <= 0:
        raise HTTPException(409, "The diagnostic lease expired. Start a new session.")


@asynccontextmanager
async def serialize(device_id: str, stop: bool = False):
    # Stop/end must not wait behind a request whose ACK is lost.
    if stop:
        yield
    else:
        async with command_locks[device_id]:
            yield


async def issue(session: AsyncSession, device: Device, user: UserSession, command: str,
                parameters: dict, session_id: str | None, record_type: str = "command") -> dict:
    request_id = str(uuid4())
    at = utcnow()
    # Freeze the deadline before database/network work. An overloaded backend
    # cannot extend an old operator intent by calculating expiry at delivery.
    command_message = wire(command, device.id, device.boot_id, request_id, parameters)
    audit = DiagnosticAudit(id=request_id, device_id=device.id, boot_id=device.boot_id,
        session_id=session_id, request_id=request_id, record_type=record_type, command=command,
        status="sent", actor=user.email, parameters={**deepcopy(parameters),
            "expiresAtEpoch": command_message["expiresAtEpoch"], "expectedBootId": device.boot_id}, result={},
        created_at=at, updated_at=at)
    session.add(audit)
    await session.commit()
    try:
        ack = CommandAck.model_validate(await device_hub.request(device.id, command_message, "command.ack"))
        if ack.device_id != device.id or ack.request_id != request_id or ack.command != command:
            raise ValueError("Controller did not return a matching diagnostic acknowledgement")
        if ack.status != "accepted":
            audit.status = "rejected"
            audit.message = ack.message or "Controller rejected the diagnostic action"
            audit.updated_at = utcnow()
            await session.commit()
            raise HTTPException(409, {"status": "rejected", "requestId": request_id, "message": audit.message})
        # Acceptance alone is not evidence of output execution or completion.
        # A full snapshot can take several seconds over the controller's TLS
        # link. Keep observation polling short, but allow bounded readback
        # after an accepted command without reissuing the mutation.
        snapshot, received_at = await read_snapshot(device, timeout=5)
        observed = snapshot["session"]
        if command == "debug.session.start" and (observed["sessionId"] != session_id or observed["mode"] != parameters["mode"]):
            raise ValueError("Session start was accepted but current session could not be confirmed")
        if command == "debug.session.renew" and observed["sessionId"] != session_id:
            raise ValueError("Session renewal could not be confirmed")
        if command == "debug.session.end" and (observed["sessionId"] is not None or not observed["automationHeld"]):
            raise ValueError("Session termination could not be confirmed")
        if command == "debug.automation.resume" and (observed["sessionId"] is not None or observed["automationHeld"]):
            raise ValueError("Automation resume could not be confirmed")
        # Lifecycle events may already have arrived while the request was waiting.
        await session.refresh(audit)
        if audit.status not in {"started", "completed", "cancelled"}:
            audit.status = "accepted"
        audit.message = ack.message
        audit.updated_at = utcnow()
        await session.commit()
        return {"requestId": request_id, "command": command, "status": "accepted", "message": ack.message,
                "sessionId": session_id, "snapshot": snapshot, "receivedAt": received_at.isoformat()}
    except HTTPException:
        raise
    except (ConnectionError, TimeoutError, ValidationError, ValueError) as exc:
        await session.refresh(audit)
        # Preserve a delivered terminal event even if the following snapshot failed.
        if audit.status not in {"completed", "cancelled"}:
            audit.status = "unconfirmed"
        audit.message = "Action could not be confirmed. Refresh controller state before trying again."
        audit.updated_at = utcnow()
        await session.commit()
        raise HTTPException(503 if isinstance(exc, ConnectionError) else 504,
                            {"status": "unconfirmed", "requestId": request_id, "message": audit.message}) from exc


def audit_dict(item: DiagnosticAudit) -> dict:
    return {"id": item.id, "requestId": item.request_id, "bootId": item.boot_id, "sessionId": item.session_id,
            "recordType": item.record_type, "command": item.command, "status": item.status, "actor": item.actor,
            "parameters": item.parameters, "result": item.result, "message": item.message,
            "createdAt": item.created_at.isoformat(), "updatedAt": item.updated_at.isoformat()}


async def audit_history(session: AsyncSession, device_id: str, can_control: bool) -> list[dict]:
    result = await session.execute(select(DiagnosticAudit).where(DiagnosticAudit.device_id == device_id,
        DiagnosticAudit.created_at >= utcnow() - timedelta(days=AUDIT_DAYS)).order_by(DiagnosticAudit.created_at.desc()).limit(200))
    return redact([audit_dict(item) for item in result.scalars()], can_control)


def capture_metadata(item: DiagnosticCapture) -> dict:
    return {"id": item.id, "name": item.name, "createdAt": item.created_at.isoformat(), "sampleCount": len(item.snapshots)}


async def capture_history(session: AsyncSession, device_id: str) -> list[dict]:
    # Polling metadata must not load up to 100 full 600-sample capture blobs.
    result = await session.execute(select(DiagnosticCapture.id, DiagnosticCapture.name, DiagnosticCapture.created_at,
        func.jsonb_array_length(DiagnosticCapture.snapshots).label("sample_count")).where(DiagnosticCapture.device_id == device_id,
        DiagnosticCapture.created_at >= utcnow() - timedelta(days=CAPTURE_DAYS)).order_by(DiagnosticCapture.created_at.desc()).limit(100))
    return [{"id": row.id, "name": row.name, "createdAt": row.created_at.isoformat(), "sampleCount": row.sample_count}
            for row in result.all()]


async def fallback_snapshot(session: AsyncSession, device: Device, state: dict) -> tuple[dict, datetime | None]:
    result = await session.execute(select(TelemetrySample).where(TelemetrySample.device_id == device.id)
                                   .order_by(TelemetrySample.id.desc()).limit(1))
    latest = result.scalar_one_or_none()
    payload = latest.payload if latest is not None else {}
    configuration = await session.get(DeviceConfiguration, device.id)
    thresholds = {item["id"]: item for item in configuration.zones} if isinstance(configuration, DeviceConfiguration) and configuration.revision == payload.get("configRevision") else {}
    zones = []
    for zone in payload.get("zones", []):
        zones.append({"id": zone["id"], "physicalRaw": zone.get("raw"), "raw": zone.get("raw"), "samples": [],
            "minRaw": None, "maxRaw": None, "filteredRaw": zone.get("filteredRaw"),
            "relativeMoisturePercent": zone.get("relativeMoisturePercent"), "unclampedPercent": None,
            "sensorValid": zone.get("sensorValid"), "validSamples": None, "invalidSamples": None,
            "dryCount": None, "wetCount": None, "sampleAgeMs": None, "phase": zone.get("phase"),
            "fault": zone.get("fault"), "enabled": None, "paused": zone.get("paused"),
            "relayOn": zone.get("relayOn"), "virtualRelayOn": None, "decisionReason": None,
            "phaseElapsedMs": None, "pulseRemainingMs": None, "wateringOnMsThisCycle": zone.get("wateringOnMsThisCycle"),
            "overrideRaw": None, "dryRaw": None, "wetRaw": None,
            "startWateringPercent": thresholds.get(zone["id"], {}).get("startWateringPercent"),
            "stopWateringPercent": thresholds.get(zone["id"], {}).get("stopWateringPercent"),
            "pulseMs": None, "soakMs": None, "maxOnMs": None})
    debug = state.get("state") or {"mode": "observe", "sessionId": None, "owner": None,
                                  "leaseRemainingMs": 0, "automationHeld": False}
    return {"type": "debug.snapshot", "schemaVersion": 1, "deviceId": device.id,
        "bootId": latest.boot_id if latest else device.boot_id, "sequence": payload.get("sequence"),
        "uptimeMs": payload.get("uptimeMs"), "configRevision": payload.get("configRevision", device.config_revision),
        "session": debug, "hardware": {"source": "documentedFallback", "board": "esp32dev",
        "relayPins": [27, 26, 25, 33], "sensorPins": [34, 35, 36, 39], "tankPin": 23,
        "relayActiveHigh": True, "oled": {"sda": 21, "scl": 22, "address": 60, "available": None}},
        "tank": {"physicalLow": None, "effectiveLow": None, "low": payload.get("mainTankLow"),
                 "pendingLow": None, "debounceRemainingMs": None, "overrideLow": None}, "zones": zones}, latest.received_at if latest else None


@router.get("/diagnostics")
async def diagnostics(device_id: str, user: Annotated[UserSession, Depends(require_user)],
                      session: Annotated[AsyncSession, Depends(get_session)]) -> dict:
    device = await device_or_404(session, device_id)
    state = await reported_state(session, device_id)
    online = device_id in await device_hub.connected_ids()
    supported = state["version"] == 1
    cached = snapshot_cache.get(device_id)
    if supported and online and (cached is None or cached[0]["bootId"] != device.boot_id or utcnow() - cached[1] > timedelta(seconds=2)):
        try:
            await read_snapshot(device, timeout=1)
        except (ConnectionError, TimeoutError, ValidationError, ValueError):
            pass
        cached = snapshot_cache.get(device_id)
    if cached is not None and cached[0]["bootId"] == device.boot_id and supported:
        snapshot, received_at = deepcopy(cached)
        snapshot["hardware"]["source"] = "firmware"
    else:
        snapshot, received_at = await fallback_snapshot(session, device, state)
    can_control = is_operator(user)
    return {"canControl": can_control, "supported": supported, "online": online,
        "snapshot": redact(snapshot, can_control), "receivedAt": received_at.isoformat() if received_at else None,
        "stale": received_at is None or utcnow() - received_at > MAX_AGE or
            (supported and snapshot["hardware"]["source"] != "firmware"),
        "actions": await audit_history(session, device_id, can_control), "captures": await capture_history(session, device_id)}


@router.post("/debug/session")
async def start_session(device_id: str, request: SessionStart, user: Annotated[UserSession, Depends(require_operator)],
                        session: Annotated[AsyncSession, Depends(get_session)]) -> dict:
    async with serialize(device_id):
        device, state = await current_control(session, device_id, request.expectedBootId)
        if state.get("mode", "observe") != "observe":
            raise HTTPException(409, "A diagnostic session is already active. End it before starting another.")
        session_id = str(uuid4())
        return await issue(session, device, user, "debug.session.start",
                           {"mode": request.mode, "sessionId": session_id, "owner": OPERATOR_EMAIL}, session_id, "session")


async def change_session(device_id: str, request: SessionRequest, user: UserSession, session: AsyncSession, command: str) -> dict:
    async with serialize(device_id, stop=command == "debug.session.end"):
        device, state = await current_control(session, device_id, request.expectedBootId, allow_stale=command == "debug.session.end")
        validate_lease(state, request.sessionId)
        return await issue(session, device, user, command, {"sessionId": request.sessionId}, request.sessionId, "session")


@router.post("/debug/session/renew")
async def renew_session(device_id: str, request: SessionRequest, user: Annotated[UserSession, Depends(require_operator)],
                        session: Annotated[AsyncSession, Depends(get_session)]) -> dict:
    return await change_session(device_id, request, user, session, "debug.session.renew")


@router.post("/debug/session/end")
async def end_session(device_id: str, request: SessionRequest, user: Annotated[UserSession, Depends(require_operator)],
                      session: Annotated[AsyncSession, Depends(get_session)]) -> dict:
    return await change_session(device_id, request, user, session, "debug.session.end")


@router.post("/debug/actions")
async def create_action(device_id: str, request: ActionRequest, user: Annotated[UserSession, Depends(require_operator)],
                        session: Annotated[AsyncSession, Depends(get_session)]) -> dict:
    stop = request.command == "debug.output.stop"
    async with serialize(device_id, stop=stop):
        device, state = await current_control(session, device_id, request.expectedBootId, allow_stale=stop)
        if request.command == "debug.automation.resume":
            if request.sessionId is not None or state.get("mode", "observe") != "observe":
                raise HTTPException(409, "End the diagnostic session before resuming automation.")
        else:
            validate_lease(state, request.sessionId, optional=stop)
        if request.command in {"debug.input.set", "debug.input.clear"} and state.get("mode") != "simulation":
            raise HTTPException(409, "Input overrides are available only in simulation.")
        parameters = {**request.parameters}
        if request.sessionId is not None:
            parameters["sessionId"] = request.sessionId
        return await issue(session, device, user, request.command, parameters, request.sessionId)


@router.get("/debug/actions")
async def get_actions(device_id: str, user: Annotated[UserSession, Depends(require_user)],
                      session: Annotated[AsyncSession, Depends(get_session)]) -> list[dict]:
    await device_or_404(session, device_id)
    return await audit_history(session, device_id, is_operator(user))


@router.post("/debug/captures", status_code=201)
async def create_capture(device_id: str, request: CaptureRequest, user: Annotated[UserSession, Depends(require_operator)],
                         session: Annotated[AsyncSession, Depends(get_session)]) -> dict:
    await device_or_404(session, device_id)
    snapshots = deepcopy(list(snapshot_rings.get(device_id, [])))
    if not snapshots:
        raise HTTPException(409, "No diagnostic samples have been captured yet.")
    item = DiagnosticCapture(id=str(uuid4()), device_id=device_id, name=request.name, snapshots=snapshots,
                             actions=await audit_history(session, device_id, True), created_at=utcnow())
    session.add(item)
    await session.commit()
    return capture_metadata(item)


@router.get("/debug/captures/{capture_id}")
async def get_capture(device_id: str, capture_id: str, user: Annotated[UserSession, Depends(require_user)],
                      session: Annotated[AsyncSession, Depends(get_session)]) -> dict:
    await device_or_404(session, device_id)
    item = await session.get(DiagnosticCapture, capture_id)
    if item is None or item.device_id != device_id or item.created_at < utcnow() - timedelta(days=CAPTURE_DAYS):
        raise HTTPException(404, "Capture not found or expired")
    return {**capture_metadata(item), "snapshots": redact(item.snapshots, is_operator(user)),
            "actions": redact(item.actions, is_operator(user))}
