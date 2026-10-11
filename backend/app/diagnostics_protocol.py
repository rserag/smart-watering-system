"""Versioned controller diagnostics; values are observations, never UI defaults."""
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, model_validator


class DebugSession(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    mode: Literal["observe", "hardware", "simulation"]
    sessionId: str | None = Field(default=None, max_length=100)
    owner: str | None = Field(default=None, max_length=320)
    leaseRemainingMs: int = Field(ge=0, le=30000)
    automationHeld: bool

    @model_validator(mode="after")
    def validate_session(self):
        if self.mode != "observe" and (not self.sessionId or not self.owner):
            raise ValueError("Active diagnostics require session identity and owner")
        if self.mode == "observe" and (self.sessionId or self.owner or self.leaseRemainingMs):
            raise ValueError("Observation cannot carry an active lease")
        return self


class DebugOled(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    sda: int = Field(ge=0, le=39)
    scl: int = Field(ge=0, le=39)
    address: int = Field(ge=1, le=127)
    available: bool


class DebugHardware(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    board: Literal["esp32dev"]
    relayPins: list[int] = Field(min_length=4, max_length=4)
    sensorPins: list[int] = Field(min_length=4, max_length=4)
    tankPin: int = Field(ge=0, le=39)
    relayActiveHigh: bool
    oled: DebugOled


class DebugTank(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    physicalLow: bool
    effectiveLow: bool
    low: bool
    pendingLow: bool
    debounceRemainingMs: int = Field(ge=0)
    overrideLow: bool | None


class DebugZone(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    id: int = Field(ge=1, le=4)
    physicalRaw: int = Field(ge=0, le=4095)
    raw: int = Field(ge=0, le=4095)
    samples: list[int] = Field(min_length=16, max_length=16)
    minRaw: int = Field(ge=0, le=4095)
    maxRaw: int = Field(ge=0, le=4095)
    filteredRaw: int = Field(ge=0, le=4095)
    relativeMoisturePercent: float = Field(ge=0, le=100)
    unclampedPercent: float
    sensorValid: bool
    validSamples: int = Field(ge=0)
    invalidSamples: int = Field(ge=0)
    dryCount: int = Field(ge=0)
    wetCount: int = Field(ge=0)
    sampleAgeMs: int = Field(ge=0)
    phase: str = Field(max_length=40)
    fault: str | None = Field(max_length=100)
    enabled: bool
    paused: bool
    relayOn: bool
    virtualRelayOn: bool
    decisionReason: str = Field(max_length=200)
    phaseElapsedMs: int = Field(ge=0)
    pulseRemainingMs: int = Field(ge=0)
    wateringOnMsThisCycle: int = Field(ge=0)
    overrideRaw: int | None = Field(ge=0, le=4095)
    dryRaw: int
    wetRaw: int
    startWateringPercent: int = Field(ge=0, le=100)
    stopWateringPercent: int = Field(ge=0, le=100)
    pulseMs: int = Field(ge=0)
    soakMs: int = Field(ge=0)
    maxOnMs: int = Field(ge=0)

    @model_validator(mode="after")
    def validate_samples(self):
        if any(sample < 0 or sample > 4095 for sample in self.samples):
            raise ValueError("ADC conversion out of range")
        return self


class DebugSnapshot(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    type: Literal["debug.snapshot"]
    schemaVersion: Literal[1]
    deviceId: str = Field(min_length=1, max_length=100)
    bootId: str = Field(min_length=1, max_length=100)
    sequence: int = Field(ge=1)
    uptimeMs: int = Field(ge=0)
    configRevision: int = Field(ge=0)
    requestId: str | None = Field(default=None, max_length=100)
    session: DebugSession
    hardware: DebugHardware
    tank: DebugTank
    zones: list[DebugZone] = Field(min_length=4, max_length=4)
    timings: dict[str, int] | None = None

    @model_validator(mode="after")
    def validate_zones(self):
        if {zone.id for zone in self.zones} != {1, 2, 3, 4}:
            raise ValueError("Diagnostics must report each channel exactly once")
        if self.session.mode == "simulation" and any(zone.relayOn for zone in self.zones):
            raise ValueError("Physical outputs cannot run in simulation")
        return self


class DebugAction(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)
    type: Literal["debug.action"]
    schemaVersion: Literal[1]
    deviceId: str = Field(min_length=1, max_length=100)
    bootId: str = Field(min_length=1, max_length=100)
    sessionId: str | None = Field(default=None, max_length=100)
    requestId: str = Field(min_length=1, max_length=100)
    command: str = Field(min_length=1, max_length=100)
    status: Literal["started", "completed", "cancelled"]
    uptimeMs: int = Field(ge=0)
    zoneId: int | None = Field(default=None, ge=1, le=4)
    message: str | None = Field(default=None, max_length=2000)
    result: dict[str, Any] | list[int] | None = None
