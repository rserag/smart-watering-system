import pytest
from pydantic import ValidationError

from app.protocol import CommandRequest, DeviceHello, TelegramDeliveryMessage, TelemetryMessage


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
            "directTelegram": True,
            "telegramDebugEnabled": True,
            "telegramConfigured": True,
            "uptimeMs": 1200,
        }
    )
    assert hello.device_id == "watering-system-01"
    assert hello.automatic_watering_enabled is True
    assert hello.direct_telegram is True
    assert hello.telegram_debug_enabled is True


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
            "directTelegram": True,
            "telegramDebugEnabled": True,
            "telegramConfigured": True,
            "telegramPendingMessages": 2,
            "telegramLastSendSucceeded": False,
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
    assert telemetry.telegram_pending_messages == 2


def test_telegram_debug_command_is_supported() -> None:
    command = CommandRequest.model_validate(
        {"command": "telegram.debug.set", "parameters": {"enabled": True}}
    )
    assert command.parameters["enabled"] is True


def test_manual_telegram_debug_command_is_supported() -> None:
    command = CommandRequest.model_validate(
        {"command": "telegram.debug.send", "parameters": {}}
    )
    assert command.command == "telegram.debug.send"


def test_telegram_delivery_report_shape() -> None:
    delivery = TelegramDeliveryMessage.model_validate(
        {
            "type": "telegram.delivery",
            "schemaVersion": 1,
            "deviceId": "watering-system-01",
            "eventId": "boot-1-telegram-7",
            "requestId": "command-1",
            "updateSequence": 4,
            "kind": "manual_debug",
            "status": "sent",
            "attempt": 1,
            "uptimeMs": 3200,
            "pendingCount": 0,
            "httpStatus": 200,
            "telegramErrorCode": None,
            "telegramMessageId": 42,
        }
    )
    assert delivery.event_id == "boot-1-telegram-7"
    assert delivery.status == "sent"
    assert delivery.telegram_message_id == 42
