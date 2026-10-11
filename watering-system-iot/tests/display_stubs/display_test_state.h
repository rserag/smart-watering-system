#pragma once

#include <Arduino.h>

namespace display_test {
inline uint32_t renderDelayMs = 0;
inline uint32_t renderFinishedAt = 0;
inline unsigned renderCount = 0;
inline uint32_t networkDelayMs = 0;
inline uint32_t ackDelayMs = 0;
inline uint32_t telegramDelayMs = 0;
inline uint32_t telegramReceivedAt = 0;
inline uint32_t startedEventUptime = 0;
inline bool startOnNextNetworkLoop = false;
inline constexpr char sessionId[] = "12345678-1234-1234-1234-123456789abc";
inline constexpr char requestId[] = "oled-regression";
inline constexpr char owner[] = "sahakyan.raphayel@gmail.com";
}
