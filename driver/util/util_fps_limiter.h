#pragma once

#include <array>
#include <optional>

#include "thread.h"
#include "util_time.h"

namespace dxvk {

  class FpsLimiter {

  public:

    FpsLimiter();

    ~FpsLimiter();

    void setTargetFrameRate(double frameRate, uint32_t maxLatency);

    void delay();

  private:

    using TimePoint = dxvk::high_resolution_clock::time_point;
    using TimerDuration = std::chrono::nanoseconds;

    dxvk::mutex     m_mutex;

    TimerDuration   m_targetInterval  = TimerDuration::zero();
    TimePoint       m_nextFrame       = TimePoint();
    uint32_t        m_maxLatency      = 0;

    uint32_t        m_heuristicFrameCount = 0;
    TimePoint       m_heuristicFrameTime  = TimePoint();
    bool            m_heuristicEnable     = false;

    bool testRefreshHeuristic(TimerDuration interval, TimePoint now, uint32_t maxLatency);

  };

}
