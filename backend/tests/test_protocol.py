import pytest
from pydantic import ValidationError

from app.protocol import DeviceHello, TelemetryMessage


def test_firmware_hello_shape() -> None:
    hello = DeviceHello.model_validate(
        {
            "type": "device.hello",
            "schemaVersion": 1,
            "deviceId": "watering-system-01",
            "firmwareVersion": "0.2.0",
            "bootId": "boot-1",
            "configRevision": 7,
            "automaticWateringEnabled": True,
            "uptimeMs": 1200,
        }
    )
    assert hello.device_id == "watering-system-01"
    assert hello.automatic_watering_enabled is True


def test_telemetry_rejects_invalid_rssi() -> None:
    with pytest.raises(ValidationError):
        TelemetryMessage.model_validate(
            {
                "type": "telemetry",
                "schemaVersion": 1,
                "deviceId": "watering-system-01",
                "bootId": "boot-1",
                "sequence": 1,
                "uptimeMs": 1200,
                "configRevision": 7,
                "wifiRssi": 10,
                "zones": [],
            }
        )


def test_device_id_rejects_path_characters() -> None:
    with pytest.raises(ValidationError):
        DeviceHello.model_validate(
            {
                "type": "device.hello",
                "schemaVersion": 1,
                "deviceId": "../controller",
                "firmwareVersion": "0.2.0",
                "bootId": "boot-1",
                "configRevision": 1,
                "automaticWateringEnabled": True,
                "uptimeMs": 1,
            }
        )

