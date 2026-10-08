#pragma once

#include <cstring>

namespace watering {

enum class TelegramSettingsAction { Ignore, Show, SilentOn, SilentOff };

inline bool authorizedTelegramSettings(const char *chatId,
                                       const char *configuredChatId,
                                       bool forwarded, bool senderIsBot) {
  return chatId[0] != '\0' && strcmp(chatId, configuredChatId) == 0 &&
         !forwarded && !senderIsBot;
}

inline TelegramSettingsAction telegramSettingsAction(const char *text,
                                                     bool callback) {
  if (callback) {
    if (strcmp(text, "hourly_silent:on") == 0) return TelegramSettingsAction::SilentOn;
    if (strcmp(text, "hourly_silent:off") == 0) return TelegramSettingsAction::SilentOff;
  } else {
    if (strcmp(text, "/start") == 0 || strcmp(text, "/settings") == 0 ||
        strcmp(text, "/hourly_silent") == 0) return TelegramSettingsAction::Show;
    if (strcmp(text, "/hourly_silent on") == 0) return TelegramSettingsAction::SilentOn;
    if (strcmp(text, "/hourly_silent off") == 0) return TelegramSettingsAction::SilentOff;
  }
  return TelegramSettingsAction::Ignore;
}

inline bool silentTelegramNotification(const char *kind, bool hourlySilent) {
  return hourlySilent && strcmp(kind, "hourly_debug") == 0;
}

}  // namespace watering
