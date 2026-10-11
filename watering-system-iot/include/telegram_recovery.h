#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace watering {

// Telegram callback data is bounded to 64 bytes by the Bot API. Recovery
// callbacks name a boot, channel, occurrence and (for confirmation) a nonce.
enum class TelegramRecoveryAction {
  Ignore, Invalid, List, ReviewClear, ReviewResume, ClearPaused, Resume, Cancel
};

struct TelegramRecoveryRequest {
  TelegramRecoveryAction action = TelegramRecoveryAction::Ignore;
  uint8_t zoneId = 0;
  uint32_t generation = 0;
  uint32_t nonce = 0;
  char bootId[24]{};
};

struct TelegramRecoveryConfirmation {
  uint32_t nonce = 0;
  uint32_t generation = 0;
  uint32_t expiresAt = 0;
  int64_t actorId = 0;
  uint8_t zoneId = 0;
  TelegramRecoveryAction action = TelegramRecoveryAction::Ignore;
  bool consumed = false;
  char outcome[384]{};
};

enum class TelegramRecoveryConfirmationCheck { Invalid, Expired, AlreadyConsumed, Ready };

inline TelegramRecoveryConfirmationCheck checkTelegramRecoveryConfirmation(
    const TelegramRecoveryRequest &request,
    const TelegramRecoveryConfirmation &confirmation, const char *currentBootId,
    int64_t actorId, uint32_t now) {
  if (currentBootId == nullptr || strcmp(request.bootId, currentBootId) != 0 ||
      confirmation.nonce == 0 || confirmation.nonce != request.nonce ||
      confirmation.actorId != actorId || confirmation.zoneId != request.zoneId ||
      confirmation.generation != request.generation ||
      (request.action != confirmation.action && request.action != TelegramRecoveryAction::Cancel)) {
    return TelegramRecoveryConfirmationCheck::Invalid;
  }
  if (static_cast<int32_t>(now - confirmation.expiresAt) >= 0) {
    return TelegramRecoveryConfirmationCheck::Expired;
  }
  return confirmation.consumed ? TelegramRecoveryConfirmationCheck::AlreadyConsumed
                               : TelegramRecoveryConfirmationCheck::Ready;
}

inline bool telegramUnsigned(const char *value, size_t length, uint32_t &out) {
  if (length == 0 || length > 10) return false;
  uint32_t result = 0;
  for (size_t index = 0; index < length; ++index) {
    if (value[index] < '0' || value[index] > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(value[index] - '0');
    if (result > (std::numeric_limits<uint32_t>::max() - digit) / 10) return false;
    result = result * 10 + digit;
  }
  out = result;
  return true;
}

inline bool telegramPositiveId(const char *value) {
  if (value == nullptr || value[0] < '1' || value[0] > '9') return false;
  const size_t length = strlen(value);
  if (length > 19) return false;
  uint64_t result = 0;
  for (size_t index = 0; index < length; ++index) {
    if (value[index] < '0' || value[index] > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(value[index] - '0');
    if (result > (static_cast<uint64_t>(INT64_MAX) - digit) / 10) return false;
    result = result * 10 + digit;
  }
  return true;
}

inline bool authorizedTelegramRecovery(const char *chatId,
                                       const char *configuredChatId,
                                       const char *senderId,
                                       const char *operatorUserId,
                                       bool forwarded, bool senderIsBot) {
  if (chatId == nullptr || configuredChatId == nullptr || senderId == nullptr ||
      operatorUserId == nullptr || chatId[0] == '\0' ||
      strcmp(chatId, configuredChatId) != 0 || forwarded || senderIsBot ||
      !telegramPositiveId(senderId)) return false;
  // Private destinations belong to their numeric user ID. Groups must opt in
  // with an explicit numeric operator instead of trusting every member.
  if (telegramPositiveId(configuredChatId)) return strcmp(senderId, configuredChatId) == 0;
  return configuredChatId[0] == '-' && telegramPositiveId(configuredChatId + 1) &&
         telegramPositiveId(operatorUserId) && strcmp(senderId, operatorUserId) == 0;
}

inline TelegramRecoveryRequest telegramRecoveryRequest(const char *text,
                                                        bool callback,
                                                        uint8_t zoneCount = 4) {
  TelegramRecoveryRequest result{};
  if (text == nullptr) return result;
  if (!callback) {
    if (strcmp(text, "/faults") == 0) {
      result.action = TelegramRecoveryAction::List;
      return result;
    }
    const char *argument = nullptr;
    if (strncmp(text, "/clearfault", 11) == 0) {
      result.action = TelegramRecoveryAction::Invalid;
      if (text[11] != ' ') return result;
      argument = text + 12;
      result.action = TelegramRecoveryAction::ReviewClear;
    } else if (strncmp(text, "/resume", 7) == 0) {
      result.action = TelegramRecoveryAction::Invalid;
      if (text[7] != ' ') return result;
      argument = text + 8;
      result.action = TelegramRecoveryAction::ReviewResume;
    } else {
      return result;
    }
    uint32_t zone = 0;
    if (!telegramUnsigned(argument, strlen(argument), zone) || zone < 1 || zone > zoneCount) {
      result.action = TelegramRecoveryAction::Invalid;
    } else {
      result.zoneId = static_cast<uint8_t>(zone);
    }
    return result;
  }

  if (strncmp(text, "wr:", 3) != 0) return result;
  result.action = TelegramRecoveryAction::Invalid;
  if (strlen(text) > 64 || strlen(text) < 12 || text[4] != ':') return result;
  TelegramRecoveryAction action;
  switch (text[3]) {
    case 'v': action = TelegramRecoveryAction::ReviewClear; break;
    case 'p': action = TelegramRecoveryAction::ReviewResume; break;
    case 'c': action = TelegramRecoveryAction::ClearPaused; break;
    case 's': action = TelegramRecoveryAction::Resume; break;
    case 'x': action = TelegramRecoveryAction::Cancel; break;
    default: return result;
  }
  const char *boot = text + 5;
  const char *bootEnd = strchr(boot, ':');
  if (bootEnd == nullptr || bootEnd == boot ||
      static_cast<size_t>(bootEnd - boot) >= sizeof(result.bootId)) return result;
  for (const char *character = boot; character != bootEnd; ++character) {
    if (!((*character >= '0' && *character <= '9') ||
          (*character >= 'a' && *character <= 'f') || *character == '-')) return result;
  }
  const char *zone = bootEnd + 1;
  const char *zoneEnd = strchr(zone, ':');
  const char *generation = zoneEnd == nullptr ? nullptr : zoneEnd + 1;
  const char *generationEnd = generation == nullptr ? nullptr : strchr(generation, ':');
  if (zoneEnd == nullptr || generationEnd == nullptr) return result;
  uint32_t parsedZone = 0;
  if (!telegramUnsigned(zone, zoneEnd - zone, parsedZone) || parsedZone < 1 || parsedZone > zoneCount ||
      !telegramUnsigned(generation, generationEnd - generation, result.generation) ||
      !telegramUnsigned(generationEnd + 1, strlen(generationEnd + 1), result.nonce)) return result;
  const bool review = action == TelegramRecoveryAction::ReviewClear || action == TelegramRecoveryAction::ReviewResume;
  if ((review && result.nonce != 0) || (!review && result.nonce == 0)) return result;
  memcpy(result.bootId, boot, bootEnd - boot);
  result.zoneId = static_cast<uint8_t>(parsedZone);
  result.action = action;
  return result;
}

inline bool telegramRecoveryCallback(char *output, size_t capacity, char action,
                                     const char *bootId, uint8_t zoneId,
                                     uint32_t generation, uint32_t nonce = 0) {
  if (output == nullptr || capacity == 0 || bootId == nullptr) return false;
  const int written = snprintf(output, capacity, "wr:%c:%s:%u:%lu:%lu", action,
                               bootId, static_cast<unsigned>(zoneId),
                               static_cast<unsigned long>(generation),
                               static_cast<unsigned long>(nonce));
  if (written < 0 || written > 64 || static_cast<size_t>(written) >= capacity) {
    output[0] = '\0';
    return false;
  }
  const auto parsed = telegramRecoveryRequest(output, true);
  if (parsed.action == TelegramRecoveryAction::Ignore || parsed.action == TelegramRecoveryAction::Invalid) {
    output[0] = '\0';
    return false;
  }
  return true;
}

}  // namespace watering
