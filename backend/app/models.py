from datetime import datetime

from sqlalchemy import BigInteger, Boolean, DateTime, Float, ForeignKey, Index, Integer, String, Text, func
from sqlalchemy.dialects.postgresql import JSONB
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column


class Base(DeclarativeBase):
    pass


class Device(Base):
    __tablename__ = "devices"

    id: Mapped[str] = mapped_column(String(100), primary_key=True)
    firmware_version: Mapped[str | None] = mapped_column(String(100))
    boot_id: Mapped[str | None] = mapped_column(String(100))
    schema_version: Mapped[int] = mapped_column(Integer, default=1)
    config_revision: Mapped[int] = mapped_column(Integer, default=0)
    automatic_watering_enabled: Mapped[bool] = mapped_column(Boolean, default=False)
    last_seen_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False, index=True)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)


class TelemetrySample(Base):
    __tablename__ = "telemetry_samples"
    __table_args__ = (
        Index("ix_telemetry_device_received", "device_id", "received_at"),
        Index("ux_telemetry_device_boot_sequence", "device_id", "boot_id", "sequence", unique=True),
    )

    id: Mapped[int] = mapped_column(BigInteger, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id", ondelete="CASCADE"))
    boot_id: Mapped[str] = mapped_column(String(100))
    sequence: Mapped[int] = mapped_column(BigInteger)
    uptime_ms: Mapped[int] = mapped_column(BigInteger)
    config_revision: Mapped[int] = mapped_column(Integer)
    wifi_rssi: Mapped[int] = mapped_column(Integer)
    received_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    payload: Mapped[dict] = mapped_column(JSONB)


class ZoneSample(Base):
    __tablename__ = "zone_samples"
    __table_args__ = (Index("ix_zone_device_zone_received", "device_id", "zone_id", "received_at"),)

    id: Mapped[int] = mapped_column(BigInteger, primary_key=True, autoincrement=True)
    telemetry_id: Mapped[int] = mapped_column(ForeignKey("telemetry_samples.id", ondelete="CASCADE"), index=True)
    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id", ondelete="CASCADE"))
    zone_id: Mapped[int] = mapped_column(Integer)
    received_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    raw: Mapped[int] = mapped_column(Integer)
    filtered_raw: Mapped[int] = mapped_column(Integer)
    moisture_percent: Mapped[float] = mapped_column(Float)
    sensor_valid: Mapped[bool] = mapped_column(Boolean)
    phase: Mapped[str] = mapped_column(String(40))
    relay_on: Mapped[bool] = mapped_column(Boolean)
    watering_on_ms_this_cycle: Mapped[int] = mapped_column(BigInteger)
    fault: Mapped[str | None] = mapped_column(String(100))


class WateringEvent(Base):
    __tablename__ = "watering_events"
    __table_args__ = (Index("ix_event_device_zone_started", "device_id", "zone_id", "started_at"),)

    id: Mapped[int] = mapped_column(BigInteger, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id", ondelete="CASCADE"))
    zone_id: Mapped[int] = mapped_column(Integer)
    started_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    ended_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    duration_ms: Mapped[int | None] = mapped_column(BigInteger)
    source: Mapped[str] = mapped_column(String(30), default="automatic")
    status: Mapped[str] = mapped_column(String(30), default="running")


class MainTankState(Base):
    __tablename__ = "main_tank_states"

    device_id: Mapped[str] = mapped_column(
        ForeignKey("devices.id", ondelete="CASCADE"), primary_key=True
    )
    is_low: Mapped[bool] = mapped_column(Boolean, nullable=False)
    last_changed_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)
    last_reported_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False)


class TelegramDelivery(Base):
    __tablename__ = "telegram_deliveries"
    __table_args__ = (
        Index("ix_telegram_delivery_device_updated", "device_id", "updated_at"),
        Index("ix_telegram_delivery_request", "request_id"),
    )

    event_id: Mapped[str] = mapped_column(String(100), primary_key=True)
    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id", ondelete="CASCADE"))
    request_id: Mapped[str | None] = mapped_column(String(100))
    kind: Mapped[str] = mapped_column(String(40))
    status: Mapped[str] = mapped_column(String(40))
    update_sequence: Mapped[int] = mapped_column(Integer)
    attempt: Mapped[int] = mapped_column(Integer, default=0)
    device_uptime_ms: Mapped[int] = mapped_column(BigInteger)
    pending_count: Mapped[int] = mapped_column(Integer, default=0)
    http_status: Mapped[int | None] = mapped_column(Integer)
    error_stage: Mapped[str | None] = mapped_column(String(40))
    telegram_error_code: Mapped[int | None] = mapped_column(Integer)
    telegram_message_id: Mapped[int | None] = mapped_column(BigInteger)
    first_observed_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    updated_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    sent_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))


class Command(Base):
    __tablename__ = "commands"

    id: Mapped[str] = mapped_column(String(36), primary_key=True)
    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id", ondelete="CASCADE"), index=True)
    command: Mapped[str] = mapped_column(String(100))
    parameters: Mapped[dict] = mapped_column(JSONB, default=dict)
    status: Mapped[str] = mapped_column(String(30), default="queued", index=True)
    result: Mapped[str | None] = mapped_column(Text)
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    sent_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))
    acknowledged_at: Mapped[datetime | None] = mapped_column(DateTime(timezone=True))


class UserSession(Base):
    __tablename__ = "user_sessions"

    token_hash: Mapped[str] = mapped_column(String(64), primary_key=True)
    google_sub: Mapped[str] = mapped_column(String(255), index=True)
    email: Mapped[str] = mapped_column(String(320), index=True)
    name: Mapped[str] = mapped_column(String(255))
    created_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    last_seen_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), server_default=func.now(), nullable=False)
    expires_at: Mapped[datetime] = mapped_column(DateTime(timezone=True), nullable=False, index=True)
