from app.main import latest_telemetry_query


def test_latest_telemetry_uses_insertion_order_not_wall_clock() -> None:
    statement = str(latest_telemetry_query("controller-1"))
    assert "ORDER BY telemetry_samples.id DESC" in statement
    assert "telemetry_samples.received_at DESC" not in statement
