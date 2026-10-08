from typing import Literal

from fastapi import HTTPException
from pydantic import BaseModel, ConfigDict, Field, ValidationError

from app.device_configuration import ConfigurationValues, ZoneThresholds
from app.hubs import device_hub


class PulseRequest(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    pulseOnMs: int = Field(ge=1000, le=60000)
    expectedRevision: int = Field(ge=1, lt=4294967295)


class CompleteZone(ZoneThresholds):
    # Preserve every reported setting when replacing the configuration.
    model_config = ConfigDict(extra="allow", strict=True)
    enabled: bool
    startWateringPercent: int = Field(ge=0, le=100)
    stopWateringPercent: int = Field(ge=0, le=100)
    dryRaw: int
    wetRaw: int
    dryConfirmationSamples: int
    wetConfirmationSamples: int
    pulseOnMs: int = Field(ge=1000, le=60000)
    soakMs: int = Field(ge=10000, le=1800000)
    maxWateringOnMsPerCycle: int = Field(ge=1000, le=180000)
    cooldownMs: int


class CompleteConfiguration(ConfigurationValues):
    model_config = ConfigDict(extra="allow", strict=True)
    automaticWateringEnabled: bool
    sampleIntervalMs: int
    telemetryIntervalMs: int
    maxConcurrentZones: Literal[1]
    zones: list[CompleteZone] = Field(min_length=4, max_length=4)


class CompleteSnapshot(BaseModel):
    model_config = ConfigDict(strict=True)
    revision: int = Field(ge=1, le=4294967295)
    schemaVersion: Literal[1]
    config: CompleteConfiguration


class ConfigurationAck(BaseModel):
    model_config = ConfigDict(strict=True)
    type: Literal["config.ack"]
    deviceId: str
    schemaVersion: Literal[1]
    requestId: str
    revision: int = Field(ge=1, le=4294967295)
    status: Literal["applied", "rejected"]
    message: str | None = None


async def read_configuration(device_id: str) -> CompleteSnapshot:
    raw = await device_hub.request(device_id, {"type": "config.get"}, "config.snapshot")
    try:
        snapshot = CompleteSnapshot.model_validate(raw)
        if {zone.id for zone in snapshot.config.zones} != {1, 2, 3, 4}:
            raise ValueError("Unsupported zones")
        return snapshot
    except (ValidationError, ValueError) as exc:
        raise HTTPException(409, "The controller did not report a complete, supported configuration.") from exc


def pulse_values(snapshot: CompleteSnapshot, zone_id: int) -> dict:
    zone = next((item for item in snapshot.config.zones if item.id == zone_id), None)
    if zone is None:
        raise HTTPException(404, "Unknown zone")
    return {"zoneId": zone_id, "revision": snapshot.revision, "pulseOnMs": zone.pulseOnMs,
            "soakMs": zone.soakMs, "maxPulseOnMs": min(60000, zone.maxWateringOnMsPerCycle)}


async def configure_pulse(device_id: str, zone_id: int, request: PulseRequest | None = None) -> dict:
    writing = False
    try:
        snapshot = await read_configuration(device_id)
        current = pulse_values(snapshot, zone_id)
        if request is None:
            return current
        if snapshot.revision != request.expectedRevision:
            raise HTTPException(409, "Settings changed since you opened this editor. Reload settings before saving.")
        if request.pulseOnMs > current["maxPulseOnMs"]:
            raise HTTPException(422, "Pulse exceeds this zone’s total watering limit.")
        if request.pulseOnMs == current["pulseOnMs"]:
            return current

        # Work from a fresh device report, never dashboard defaults or a partial database snapshot.
        config = snapshot.config.model_dump()
        for zone in config["zones"]:
            if zone["id"] == zone_id:
                zone["pulseOnMs"] = request.pulseOnMs
        revision = snapshot.revision + 1
        writing = True
        ack = ConfigurationAck.model_validate(await device_hub.request(
            device_id, {"type": "config.set", "revision": revision, "config": config}, "config.ack",
        ))
        if ack.revision != revision or ack.status != "applied":
            raise HTTPException(409, ack.message or "The controller rejected this change. Reload settings before saving.")
        confirmed = pulse_values(await read_configuration(device_id), zone_id)
        if confirmed["revision"] != revision or confirmed["pulseOnMs"] != request.pulseOnMs:
            raise HTTPException(409, "Settings changed during verification. Reload settings to see the current pulse.")
        return confirmed
    except (ConnectionError, TimeoutError) as exc:
        detail = ("Save could not be confirmed. Reload settings to check the controller before trying again."
                  if writing else "Controller is unavailable. Reconnect and reload settings.")
        raise HTTPException(504 if isinstance(exc, TimeoutError) else 503, detail) from exc
