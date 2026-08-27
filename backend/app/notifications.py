import httpx


def main_tank_alert_text(device_id: str, is_low: bool) -> str:
    if is_low:
        return (
            "⚠️ Watering stopped: the main tank water is low.\n"
            f"Controller: {device_id}\n"
            "All watering is blocked until the tank is refilled."
        )
    return (
        "✅ Main tank level restored.\n"
        f"Controller: {device_id}\n"
        "The low-water safety interlock has cleared."
    )


async def send_main_tank_alert(
    *, bot_token: str, chat_id: str, device_id: str, is_low: bool
) -> None:
    url = f"https://api.telegram.org/bot{bot_token.strip()}/sendMessage"
    async with httpx.AsyncClient(timeout=10.0, trust_env=False) as client:
        response = await client.post(
            url,
            json={
                "chat_id": chat_id.strip(),
                "text": main_tank_alert_text(device_id, is_low),
            },
        )
        response.raise_for_status()
        payload = response.json()
        if payload.get("ok") is not True:
            raise RuntimeError("Telegram rejected the alert")
