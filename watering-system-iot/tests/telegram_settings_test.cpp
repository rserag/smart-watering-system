#include <cassert>
#include "telegram_settings.h"

int main() {
  using namespace watering;
  assert(authorizedTelegramSettings("12345", "12345", false, false));
  assert(authorizedTelegramSettings("-10012345", "-10012345", false, false));
  assert(!authorizedTelegramSettings("54321", "12345", false, false));
  assert(!authorizedTelegramSettings("", "", false, false));
  assert(!authorizedTelegramSettings("12345", "12345", true, false));
  assert(!authorizedTelegramSettings("12345", "12345", false, true));
  // The preference must never silence safety, pump, or on-demand reports.
  const char *kinds[] = {"tank_low", "tank_restored", "pump_started", "manual_debug"};
  for (const char *kind : kinds) {
    assert(!silentTelegramNotification(kind, true));
    assert(!silentTelegramNotification(kind, false));
  }
  assert(silentTelegramNotification("hourly_debug", true));
  assert(!silentTelegramNotification("hourly_debug", false));
  // Both chat commands and buttons set explicit values, so replay is idempotent.
  assert(telegramSettingsAction("/hourly_silent on", false) == TelegramSettingsAction::SilentOn);
  assert(telegramSettingsAction("hourly_silent:on", true) == TelegramSettingsAction::SilentOn);
  assert(telegramSettingsAction("/hourly_silent off", false) == TelegramSettingsAction::SilentOff);
  assert(telegramSettingsAction("hourly_silent:off", true) == TelegramSettingsAction::SilentOff);
  assert(telegramSettingsAction("/settings", false) == TelegramSettingsAction::Show);
  assert(telegramSettingsAction("/start", false) == TelegramSettingsAction::Show);
  assert(telegramSettingsAction("/hourly_silent", false) == TelegramSettingsAction::Show);
  assert(telegramSettingsAction("/hourly_silent toggle", false) == TelegramSettingsAction::Ignore);
  assert(telegramSettingsAction("/settings", true) == TelegramSettingsAction::Ignore);
  assert(telegramSettingsAction("hourly_silent:on", false) == TelegramSettingsAction::Ignore);
  assert(telegramSettingsAction("/water", false) == TelegramSettingsAction::Ignore);
}
