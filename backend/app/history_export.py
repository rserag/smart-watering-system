import csv
import io
from collections.abc import AsyncIterator
from datetime import datetime

from sqlalchemy import select

from app.database import SessionLocal
from app.models import ZoneSample


async def history_csv(device_id: str, zone_id: int | None, from_time: datetime, to_time: datetime) -> AsyncIterator[str]:
    """Keep both the database cursor and CSV buffer bounded for large exports."""
    query = select(
        ZoneSample.received_at, ZoneSample.device_id, ZoneSample.zone_id,
        ZoneSample.raw, ZoneSample.filtered_raw, ZoneSample.moisture_percent,
        ZoneSample.sensor_valid, ZoneSample.phase, ZoneSample.relay_on,
        ZoneSample.watering_on_ms_this_cycle, ZoneSample.fault,
    ).where(
        ZoneSample.device_id == device_id,
        ZoneSample.received_at >= from_time, ZoneSample.received_at <= to_time,
    ).order_by(ZoneSample.received_at, ZoneSample.id)
    if zone_id is not None:
        query = query.where(ZoneSample.zone_id == zone_id)
    # The generator owns its session through completion or client cancellation.
    async with SessionLocal() as session:
        result = await session.stream(query.execution_options(yield_per=1000))
        try:
            buffer = io.StringIO()
            writer = csv.writer(buffer)
            writer.writerow(["received_at", "device_id", "zone_id", "raw", "filtered_raw", "moisture_percent", "sensor_valid", "phase", "relay_on", "watering_ms", "fault"])
            yield buffer.getvalue()
            async for rows in result.partitions(1000):
                buffer.seek(0)
                buffer.truncate(0)
                for row in rows:
                    writer.writerow([row[0].isoformat(), *row[1:10], row[10] or ""])
                yield buffer.getvalue()
        finally:
            await result.close()
