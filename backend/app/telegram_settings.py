from fastapi import HTTPException
from pydantic import BaseModel, ConfigDict, ValidationError

from app.hubs import device_hub
from app.protocol import CommandAck


class HourlySilentRequest(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    enabled: bool


def supports_hourly_silent(firmware_version: str | None) -> bool:
    if not firmware_version:
        return False
    parts = firmware_version.split(".")
    if len(parts) != 3 or not all(part.isascii() and part.isdigit() for part in parts):
        return False
    return tuple(map(int, parts)) >= (0, 6, 0)


async def configure_hourly_silent(device_id: str, enabled: bool) -> dict:
    try:
        ack = CommandAck.model_validate(await device_hub.request(
            device_id, {"type": "telegram.hourlySilent.set", "enabled": enabled}, "command.ack",
        ))
    except (ConnectionError, TimeoutError) as exc:
        raise HTTPException(
            504 if isinstance(exc, TimeoutError) else 503,
            "Save could not be confirmed. Check the controller’s setting before trying again.",
        ) from exc
    except ValidationError as exc:
        raise HTTPException(409, "The controller did not return a valid confirmation.") from exc
    if ack.device_id != device_id or ack.command != "telegram.hourlySilent.set" or ack.status != "accepted":
        raise HTTPException(409, ack.message or "The controller did not confirm this change.")
    return {"enabled": enabled}
