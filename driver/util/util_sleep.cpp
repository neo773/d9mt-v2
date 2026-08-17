#include "util_sleep.h"
#include "util_string.h"

#include "./log/log.h"

using namespace std::chrono_literals;

namespace dxvk {

  Sleep Sleep::s_instance;


  Sleep::Sleep() {

  }


  Sleep::~Sleep() {

  }


  void Sleep::initialize() {
    std::lock_guard lock(m_mutex);

    if (m_initialized.load())
      return;

    initializePlatformSpecifics();
    m_sleepThreshold = 4 * m_sleepGranularity;

    m_initialized.store(true);
  }


  void Sleep::initializePlatformSpecifics() {
#ifdef _WIN32
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");

    if (ntdll) {
      NtDelayExecution = reinterpret_cast<NtDelayExecutionProc>(
        ::GetProcAddress(ntdll, "NtDelayExecution"));
      auto NtQueryTimerResolution = reinterpret_cast<NtQueryTimerResolutionProc>(
        ::GetProcAddress(ntdll, "NtQueryTimerResolution"));
      auto NtSetTimerResolution = reinterpret_cast<NtSetTimerResolutionProc>(
        ::GetProcAddress(ntdll, "NtSetTimerResolution"));

      ULONG min, max, cur;

      if (NtQueryTimerResolution && !NtQueryTimerResolution(&min, &max, &cur)) {
        m_sleepGranularity = TimerDuration(cur);

        if (NtSetTimerResolution && !NtSetTimerResolution(max, TRUE, &cur)) {
          Logger::info(str::format("Setting timer interval to ", (double(max) / 10.0), " us"));
          m_sleepGranularity = TimerDuration(max);
        }
      }
    } else {
      m_sleepGranularity = TimerDuration(1ms);
    }
#else
    m_sleepGranularity = TimerDuration(500us);
#endif
  }


  Sleep::TimePoint Sleep::sleep(TimePoint t0, TimerDuration duration) {
    if (duration <= TimerDuration::zero())
      return t0;

    if (!m_initialized.load())
      initialize();

    TimerDuration sleepThreshold = m_sleepThreshold;

    if (m_sleepGranularity != TimerDuration::zero())
      sleepThreshold += duration / 6;

    TimerDuration remaining = duration;
    TimePoint t1 = t0;

    while (remaining > sleepThreshold) {
      TimerDuration sleepDuration = remaining - sleepThreshold;

      systemSleep(sleepDuration);

      t1 = dxvk::high_resolution_clock::now();
      remaining -= std::chrono::duration_cast<TimerDuration>(t1 - t0);
      t0 = t1;
    }

    while (remaining > TimerDuration::zero()) {
      t1 = dxvk::high_resolution_clock::now();
      remaining -= std::chrono::duration_cast<TimerDuration>(t1 - t0);
      t0 = t1;
    }

    return t1;
  }


  void Sleep::systemSleep(TimerDuration duration) {
#ifdef _WIN32
    if (NtDelayExecution) {
      LARGE_INTEGER ticks;
      ticks.QuadPart = -duration.count();

      NtDelayExecution(FALSE, &ticks);
    } else {
      std::this_thread::sleep_for(duration);
    }
#else
    std::this_thread::sleep_for(duration);
#endif
  }

}
