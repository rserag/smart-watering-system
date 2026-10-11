#include <cassert>
#include <climits>
#include <cstring>
#include "telegram_recovery.h"

int main() {
  using namespace watering;
  // Private-chat controls belong to the actual numeric user. A group chat ID
  // alone must never give every member authority to re-enable a pump.
  assert(authorizedTelegramRecovery("12345", "12345", "12345", "", false, false));
  assert(!authorizedTelegramRecovery("12345", "12345", "54321", "", false, false));
  assert(!authorizedTelegramRecovery("-10012345", "-10012345", "12345", "", false, false));
  assert(authorizedTelegramRecovery("-10012345", "-10012345", "12345", "12345", false, false));
  assert(!authorizedTelegramRecovery("-10012345", "-10012345", "54321", "12345", false, false));
  assert(!authorizedTelegramRecovery("54321", "12345", "12345", "", false, false));
  assert(!authorizedTelegramRecovery("12345", "12345", "12345", "", true, false));
  assert(!authorizedTelegramRecovery("12345", "12345", "12345", "", false, true));
  assert(!authorizedTelegramRecovery("", "", "12345", "", false, false));
  assert(!authorizedTelegramRecovery("@group", "@group", "12345", "12345", false, false));
  assert(!authorizedTelegramRecovery("12345", "12345", "", "", false, false));
  assert(!telegramPositiveId("9223372036854775808"));
  assert(telegramPositiveId("9223372036854775807"));
  assert(!telegramPositiveId("012345"));

  assert(telegramRecoveryRequest("/faults", false).action == TelegramRecoveryAction::List);
  const auto clear = telegramRecoveryRequest("/clearfault 2", false);
  assert(clear.action == TelegramRecoveryAction::ReviewClear && clear.zoneId == 2 && clear.nonce == 0);
  assert(telegramRecoveryRequest("/resume 4", false).action == TelegramRecoveryAction::ReviewResume);
  assert(telegramRecoveryRequest("/resume 0", false).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("/clearfault 5", false).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("/clearfault 2 extra", false).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("/clearfault -1", false).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("/clearfault", false).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("/resume 4294967296", false).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("/hourly_silent on", false).action == TelegramRecoveryAction::Ignore);

  char callback[65]{};
  assert(telegramRecoveryCallback(callback, sizeof(callback), 'c', "abcdef01-1234", 4, UINT32_MAX, UINT32_MAX));
  assert(strlen(callback) <= 64);
  auto request = telegramRecoveryRequest(callback, true);
  assert(request.action == TelegramRecoveryAction::ClearPaused);
  assert(request.zoneId == 4 && request.generation == UINT32_MAX && request.nonce == UINT32_MAX);
  assert(strcmp(request.bootId, "abcdef01-1234") == 0);
  assert(telegramRecoveryRequest(callback, false).action == TelegramRecoveryAction::Ignore);
  assert(telegramRecoveryRequest("/faults", true).action == TelegramRecoveryAction::Ignore);
  assert(telegramRecoveryRequest("wr:c:abcdef01-1234:4:1:0", true).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("wr:v:abcdef01-1234:4:1:15", true).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("wr:c:abcdef01-1234:4:4294967296:1", true).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("wr:c:abcdef01-1234:4:1:1:extra", true).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("wr:c:abcdef01-1234:5:1:1", true).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("wr:c:invalid:4:1:1", true).action == TelegramRecoveryAction::Invalid);
  assert(telegramRecoveryRequest("wr:c:abcdef01-1234:4:00000000000000000000000000000000000000000000000000:1", true).action == TelegramRecoveryAction::Invalid);
  assert(!telegramRecoveryCallback(callback, 8, 'c', "abcdef01-1234", 4, 1, 1));
  assert(!telegramRecoveryCallback(callback, sizeof(callback), 'c', "abcdef01-1234", 4, 1));
  assert(telegramRecoveryCallback(callback, sizeof(callback), 'v', "abcdef01-1234", 4, 1));

  // The confirmation gate used by the main task rejects another actor, boot,
  // occurrence, channel, action or nonce. Read-only review never actuates.
  TelegramRecoveryConfirmation confirmation{};
  confirmation.nonce = 123;
  confirmation.generation = 7;
  confirmation.zoneId = 2;
  confirmation.actorId = 12345;
  confirmation.expiresAt = 1000;
  confirmation.action = TelegramRecoveryAction::ClearPaused;
  assert(telegramRecoveryCallback(callback, sizeof(callback), 'c', "abcdef01-1234", 2, 7, 123));
  request = telegramRecoveryRequest(callback, true);
  auto check = [&](int64_t actor, const char *boot, uint32_t now) {
    return checkTelegramRecoveryConfirmation(request, confirmation, boot, actor, now);
  };
  assert(check(12345, "abcdef01-1234", 999) == TelegramRecoveryConfirmationCheck::Ready);
  assert(check(54321, "abcdef01-1234", 999) == TelegramRecoveryConfirmationCheck::Invalid);
  assert(check(12345, "abcdef01-ffff", 999) == TelegramRecoveryConfirmationCheck::Invalid);
  assert(check(12345, "abcdef01-1234", 1000) == TelegramRecoveryConfirmationCheck::Expired);
  request.generation = 8;
  assert(check(12345, "abcdef01-1234", 999) == TelegramRecoveryConfirmationCheck::Invalid);
  request.generation = 7;
  request.zoneId = 1;
  assert(check(12345, "abcdef01-1234", 999) == TelegramRecoveryConfirmationCheck::Invalid);
  request.zoneId = 2;
  request.nonce = 124;
  assert(check(12345, "abcdef01-1234", 999) == TelegramRecoveryConfirmationCheck::Invalid);
  request.nonce = 123;
  request.action = TelegramRecoveryAction::Resume;
  assert(check(12345, "abcdef01-1234", 999) == TelegramRecoveryConfirmationCheck::Invalid);
  request.action = TelegramRecoveryAction::ReviewClear;
  assert(check(12345, "abcdef01-1234", 999) == TelegramRecoveryConfirmationCheck::Invalid);
  request.action = TelegramRecoveryAction::Cancel;
  assert(check(12345, "abcdef01-1234", 999) == TelegramRecoveryConfirmationCheck::Ready);
  request.action = TelegramRecoveryAction::ClearPaused;
  confirmation.consumed = true;
  assert(check(12345, "abcdef01-1234", 999) == TelegramRecoveryConfirmationCheck::AlreadyConsumed);
  // TTL checks remain correct across the uint32_t uptime wrap.
  confirmation.consumed = false;
  confirmation.expiresAt = 30;
  assert(check(12345, "abcdef01-1234", UINT32_MAX - 10) == TelegramRecoveryConfirmationCheck::Ready);
  assert(check(12345, "abcdef01-1234", 30) == TelegramRecoveryConfirmationCheck::Expired);
}
