from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field


class DeviceHello(BaseModel):
    model_config = ConfigDict(extra="ignore")
    type: Literal["device.hello"]
    schema_version: int = Field(alias="schemaVersion", ge=1)
    device_id: str = Field(alias="deviceId", min_length=1, max_length=100, pattern=r"^[A-Za-z0-9._-]+$")
    firmware_version: str = Field(alias="firmwareVersion", max_length=100)
    boot_id: str = Field(alias="bootId", min_length=1, max_length=100)
    config_revision: int = Field(alias="configRevision", ge=0)
    automatic_watering_enabled: bool = Field(alias="automaticWateringEnabled")
    uptime_ms: int = Field(alias="uptimeMs", ge=0)


class ZoneTelemetry(BaseModel):
    model_config = ConfigDict(extra="ignore")
    id: int = Field(ge=1, le=16)
    raw: int
    filtered_raw: int = Field(alias="filteredRaw")
    relative_moisture_percent: float = Field(alias="relativeMoisturePercent")
    sensor_valid: bool = Field(alias="sensorValid")
    phase: str = Field(max_length=40)
    relay_on: bool = Field(alias="relayOn")
    watering_on_ms_this_cycle: int = Field(alias="wateringOnMsThisCycle", ge=0)
    fault: str | None = Field(default=None, max_length=100)


class TelemetryMessage(BaseModel):
    model_config = ConfigDict(extra="ignore")
    type: Literal["telemetry"]
    schema_version: int = Field(alias="schemaVersion", ge=1)
    device_id: str = Field(alias="deviceId", min_length=1, max_length=100)
    boot_id: str = Field(alias="bootId", min_length=1, max_length=100)
    sequence: int = Field(ge=1)
    uptime_ms: int = Field(alias="uptimeMs", ge=0)
    config_revision: int = Field(alias="configRevision", ge=0)
    wifi_rssi: int = Field(alias="wifiRssi", ge=-127, le=0)
    zones: list[ZoneTelemetry] = Field(min_length=1, max_length=16)


class CommandAck(BaseModel):
    model_config = ConfigDict(extra="ignore")
    type: Literal["command.ack"]
    schema_version: int = Field(alias="schemaVersion", ge=1)
    device_id: str = Field(alias="deviceId", min_length=1, max_length=100)
    request_id: str = Field(alias="requestId", min_length=1, max_length=100)
    command: str = Field(max_length=100)
    status: Literal["accepted", "rejected", "duplicate"]
    message: str | None = Field(default=None, max_length=2000)


class CommandRequest(BaseModel):
    command: Literal["zone.water", "zone.stop", "system.stopAll", "fault.clear", "telemetry.request", "config.get", "config.set"]
    parameters: dict[str, Any] = Field(default_factory=dict)
