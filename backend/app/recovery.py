from datetime import datetime, timedelta, timezone
from typing import Literal
from uuid import uuid4

from fastapi import HTTPException
from pydantic import BaseModel, ConfigDict, Field, ValidationError
from sqlalchemy.ext.asyncio import AsyncSession

from app.hubs import device_hub
from app.models import Command, Device, TelemetrySample, UserSession
from app.protocol import CommandAck


RECOVERY_TELEMETRY_MAX_AGE = timedelta(seconds=30)
RECOVERY_COMMANDS = frozenset({"fault.clearPaused", "zone.resume"})
UNSAFE_QUEUED_RECOVERY_COMMANDS = RECOVERY_COMMANDS | {"fault.clear"}


class RecoveryRequest(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    action: Literal["clear", "resume"]
    expectedBootId: str = Field(min_length=1, max_length=100)
    expectedRecoveryGeneration: int = Field(ge=0, le=4294967295)


def supports_paused_recovery(firmware_version: str | None) -> bool:
    if not firmware_version:
        return False
    parts = firmware_version.split(".")
    if len(parts) != 3 or not all(part.isascii() and part.isdigit() for part in parts):
        return False
    return tuple(map(int, parts)) >= (0, 7, 0)


def validate_recovery_state(
    device: Device, latest: TelemetrySample | None, zone_id: int,
    request: RecoveryRequest, observed_at: datetime,
) -> None:
    if latest is None or not latest.received_at or latest.received_at.tzinfo is None:
        raise HTTPException(409, "Current controller readings are unavailable. Refresh before recovering this channel.")
    age = observed_at - latest.received_at
    if age < timedelta(seconds=-5) or age > RECOVERY_TELEMETRY_MAX_AGE:
        raise HTTPException(409, "Controller readings are stale. Wait for fresh readings before recovering this channel.")
    if device.boot_id != request.expectedBootId or latest.boot_id != request.expectedBootId:
        raise HTTPException(409, "The controller restarted. Review its current channel state before trying again.")
    zone = next((item for item in latest.payload.get("zones", []) if item.get("id") == zone_id), None)
    if zone is None:
        raise HTTPException(404, "Unknown channel")
    generation = zone.get("recoveryGeneration")
    if type(generation) is not int or generation != request.expectedRecoveryGeneration:
        raise HTTPException(409, "The channel state changed. Review its current state before trying again.")
    if request.action == "clear":
        if zone.get("fault") != "maximum_watering_reached":
            raise HTTPException(409, "Only a watering limit fault can be cleared and kept paused.")
    elif zone.get("paused") is not True or zone.get("fault") or zone.get("phase") == "disabled":
        raise HTTPException(409, "Resume requires an enabled, paused channel with no fault.")
    elif latest.payload.get("mainTankLow") is not False:
        detail = ("Refill the main tank before resuming this channel."
                  if latest.payload.get("mainTankLow") is True
                  else "Current main tank readings are unavailable. Wait for fresh readings before resuming this channel.")
        raise HTTPException(409, detail)


async def recover_zone(
    session: AsyncSession, device: Device, latest: TelemetrySample | None,
    zone_id: int, request: RecoveryRequest, user: UserSession, observed_at: datetime,
) -> dict:
    validate_recovery_state(device, latest, zone_id, request, observed_at)
    command_type = "fault.clearPaused" if request.action == "clear" else "zone.resume"
    wire_parameters = {
        "zoneId": zone_id,
        "expectedBootId": request.expectedBootId,
        "expectedRecoveryGeneration": request.expectedRecoveryGeneration,
        "expiresAtEpoch": int(observed_at.timestamp()) + 30,
    }
    command = Command(
        id=str(uuid4()), device_id=device.id, command=command_type,
        parameters={**wire_parameters, "source": "dashboard", "actor": {"email": user.email, "subject": user.google_sub}},
        # Recovery actions are never queued for a later controller connection.
        status="sent", sent_at=observed_at,
    )
    session.add(command)
    await session.commit()
    try:
        raw = await device_hub.request(
            device.id, {"type": command_type, "requestId": command.id, **wire_parameters}, "command.ack",
        )
        ack = CommandAck.model_validate(raw)
        if ack.device_id != device.id or ack.command != command_type or ack.request_id != command.id:
            raise HTTPException(409, "The controller did not return a matching recovery confirmation.")
        if ack.status != "accepted":
            command.status = "rejected"
            raise HTTPException(409, ack.message or "The controller did not confirm this recovery. Refresh its state before trying again.")
    except (ConnectionError, TimeoutError) as exc:
        command.status = "unconfirmed"
        command.result = "Recovery could not be confirmed. Refresh the controller state before trying again."
        await session.commit()
        raise HTTPException(504 if isinstance(exc, TimeoutError) else 503, command.result) from exc
    except (HTTPException, ValidationError) as exc:
        command.status = "rejected" if command.status == "rejected" else "unconfirmed"
        command.result = exc.detail if isinstance(exc, HTTPException) else "The controller did not return a valid recovery confirmation."
        command.acknowledged_at = datetime.now(timezone.utc)
        await session.commit()
        if isinstance(exc, HTTPException):
            raise
        raise HTTPException(409, command.result) from exc
    command.status = "acknowledged"
    command.result = ack.message or ("Fault cleared; channel kept paused" if request.action == "clear" else "Channel resumed; sensor checks restarted")
    command.acknowledged_at = datetime.now(timezone.utc)
    await session.commit()
    return {"zoneId": zone_id, "action": request.action, "paused": request.action == "clear", "status": "confirmed"}
