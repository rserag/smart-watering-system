import unicodedata
from typing import Annotated

from pydantic import BaseModel, ConfigDict, StringConstraints, field_validator
from sqlalchemy import literal, select
from sqlalchemy.dialects.postgresql import insert

from app.models import Device, ZoneSettings


class ZoneNameRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")
    name: Annotated[str, StringConstraints(strip_whitespace=True, max_length=60)]

    @field_validator("name")
    @classmethod
    def reject_control_characters(cls, value: str) -> str:
        if any(unicodedata.category(character).startswith("C") for character in value):
            raise ValueError("Zone names cannot contain control characters")
        return value


def legacy_zone_names_query():
    # Preserve the former dashboard's Zone 1 label for existing devices only.
    # Conflict handling also preserves a user's explicit reset to the default.
    return insert(ZoneSettings).from_select(
        ["device_id", "zone_id", "name"],
        select(Device.id, literal(1), literal("Tomatoes")),
    ).on_conflict_do_nothing(index_elements=["device_id", "zone_id"])


def save_zone_name_query(device_id: str, zone_id: int, name: str | None):
    return insert(ZoneSettings).values(
        device_id=device_id, zone_id=zone_id, name=name,
    ).on_conflict_do_update(
        index_elements=["device_id", "zone_id"], set_={"name": name},
    )
