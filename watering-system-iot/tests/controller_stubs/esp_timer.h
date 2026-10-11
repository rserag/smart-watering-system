#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include "Arduino.h"

constexpr int ESP_OK = 0;
constexpr int ESP_TIMER_TASK = 0;
struct esp_timer_create_args_t {
  void (*callback)(void *);
  void *arg;
  int dispatch_method;
  const char *name;
};
namespace controller_test {
struct Timer { esp_timer_create_args_t args{}; uint64_t due = 0; uint64_t period = 0; bool armed = false; };
inline std::deque<Timer> timers;
inline bool timerCreateFails = false;
inline bool timerStartFails = false;
inline const char *timerFailName = nullptr;
inline void fireTimers() {
  for (auto &timer : timers) {
    if (!timer.armed || timer.due > uint64_t(now) * 1000) continue;
    if (timer.period == 0) timer.armed = false;
    else timer.due = uint64_t(now) * 1000 + timer.period;
    timer.args.callback(timer.args.arg);
  }
}
}
using esp_timer_handle_t = controller_test::Timer *;
inline int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *timer) {
  if (controller_test::timerCreateFails) return -1;
  controller_test::timers.push_back({*args, 0, 0, false});
  *timer = &controller_test::timers.back();
  return ESP_OK;
}
inline int esp_timer_stop(esp_timer_handle_t timer) { if (timer != nullptr) timer->armed = false; return ESP_OK; }
inline int esp_timer_start_once(esp_timer_handle_t timer, uint64_t duration) {
  if (timer == nullptr || controller_test::timerStartFails ||
      (controller_test::timerFailName != nullptr && std::strcmp(timer->args.name, controller_test::timerFailName) == 0)) return -1;
  timer->due = uint64_t(controller_test::now) * 1000 + duration;
  timer->period = 0;
  timer->armed = true;
  return ESP_OK;
}
inline int esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t duration) {
  const int result = esp_timer_start_once(timer, duration);
  if (result == ESP_OK) timer->period = duration;
  return result;
}
