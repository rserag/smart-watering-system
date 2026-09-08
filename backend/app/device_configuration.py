from typing import Literal

from pydantic import BaseModel, Field, model_validator
from sqlalchemy.dialects.postgresql import insert

from app.models import DeviceConfiguration


class ZoneThresholds(BaseModel):
    id: int = Field(ge=1, le=16)
    startWateringPercent: float = Field(ge=0, le=100)
    stopWateringPercent: float = Field(ge=0, le=100)

    @model_validator(mode="after")
    def validate_order(self):
        if self.startWateringPercent >= self.stopWateringPercent:
            raise ValueError("Stop threshold must be above the start threshold")
        return self


class ConfigurationValues(BaseModel):
    zones: list[ZoneThresholds] = Field(min_length=1, max_length=16)

    @model_validator(mode="after")
    def unique_zones(self):
        if len({zone.id for zone in self.zones}) != len(self.zones):
            raise ValueError("Duplicate zone identifiers")
        return self


class ConfigurationSnapshot(BaseModel):
    type: Literal["config.snapshot"]
    deviceId: str = Field(min_length=1, max_length=100)
    revision: int = Field(ge=1)
    config: ConfigurationValues


def save_configuration_query(snapshot: ConfigurationSnapshot):
    values = {"revision": snapshot.revision, "zones": [zone.model_dump() for zone in snapshot.config.zones]}
    return insert(DeviceConfiguration).values(device_id=snapshot.deviceId, **values).on_conflict_do_update(
        index_elements=["device_id"], set_=values,
    )


def active_thresholds(configuration: DeviceConfiguration | None, revision: int):
    if configuration is None or configuration.revision != revision:
        return {}
    return {zone["id"]: {key: zone[key] for key in ("startWateringPercent", "stopWateringPercent")} for zone in configuration.zones}
