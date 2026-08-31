from functools import lru_cache
from typing import Literal

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", extra="ignore")

    app_name: str = "watering-system-server"
    database_url: str = "postgresql+asyncpg://watering:watering@localhost:5432/watering"
    device_shared_token: str = "change-me"
    server_config_version: int = 1
    hello_timeout_seconds: float = 10.0

    auth_mode: Literal["development", "google"] = "development"
    session_secret: str = "replace-with-a-long-random-value"
    session_cookie_name: str = "watering_session"
    session_ttl_hours: int = 12
    development_user_email: str = "local-reviewer@example.com"
    development_user_name: str = "Local reviewer"
    google_client_id: str = ""
    google_client_secret: str = ""
    allowed_google_emails: str = ""
    frontend_url: str = "http://localhost:3000"
    cors_origins: str = "http://localhost:3000,http://127.0.0.1:3000"

    @property
    def allowed_email_set(self) -> set[str]:
        return {item.strip().lower() for item in self.allowed_google_emails.split(",") if item.strip()}

    @property
    def cors_origin_list(self) -> list[str]:
        return [item.strip() for item in self.cors_origins.split(",") if item.strip()]

@lru_cache
def get_settings() -> Settings:
    return Settings()
