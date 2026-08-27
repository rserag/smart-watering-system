import pytest
from pydantic import ValidationError

from app.notifications import main_tank_alert_text
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


def test_telemetry_accepts_main_tank_low_state() -> None:
    telemetry = TelemetryMessage.model_validate(
        {
            "type": "telemetry",
            "schemaVersion": 1,
            "deviceId": "watering-system-01",
            "bootId": "boot-1",
            "sequence": 2,
            "uptimeMs": 2200,
            "configRevision": 7,
            "wifiRssi": -61,
            "mainTankLow": True,
            "zones": [
                {
                    "id": 1,
                    "raw": 2300,
                    "filteredRaw": 2290,
                    "relativeMoisturePercent": 42,
                    "sensorValid": True,
                    "phase": "monitoring",
                    "relayOn": False,
                    "wateringOnMsThisCycle": 0,
                    "fault": None,
                }
            ],
        }
    )
    assert telemetry.main_tank_low is True


def test_main_tank_alerts_explain_cutoff_and_recovery() -> None:
    assert "Watering stopped" in main_tank_alert_text("controller-1", True)
    assert "controller-1" in main_tank_alert_text("controller-1", True)
    assert "level restored" in main_tank_alert_text("controller-1", False)
