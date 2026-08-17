#pragma once

#include "thread.h"
#include "util_time.h"

namespace dxvk {

  class Sleep {

  public:

    using TimePoint = dxvk::high_resolution_clock::time_point;

    ~Sleep();

    template<typename Rep, typename Period>
    static TimePoint sleepFor(TimePoint t0, std::chrono::duration<Rep, Period> duration) {
      return s_instance.sleep(t0, std::chrono::duration_cast<TimerDuration>(duration));
    }

    static TimePoint sleepUntil(TimePoint t0, TimePoint t1) {
      return sleepFor(t0, t1 - t0);
    }

  private:

    static Sleep s_instance;

    dxvk::mutex       m_mutex;
    std::atomic<bool> m_initialized = { false };

#ifdef _WIN32
    using TimerDuration = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
    using NtQueryTimerResolutionProc = LONG (NTAPI *) (ULONG*, ULONG*, ULONG*);
    using NtSetTimerResolutionProc = LONG (NTAPI *) (ULONG, BOOLEAN, ULONG*);
    using NtDelayExecutionProc = LONG (NTAPI *) (BOOLEAN, LARGE_INTEGER*);
    NtDelayExecutionProc NtDelayExecution = nullptr;
#else
    using TimerDuration = std::chrono::nanoseconds;
#endif

    TimerDuration m_sleepGranularity = TimerDuration::zero();
    TimerDuration m_sleepThreshold   = TimerDuration::zero();

    Sleep();

    void initialize();

    void initializePlatformSpecifics();

    TimePoint sleep(TimePoint t0, TimerDuration duration);

    void systemSleep(TimerDuration duration);

  };

}
