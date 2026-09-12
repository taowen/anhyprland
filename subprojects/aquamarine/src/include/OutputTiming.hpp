#pragma once

#include <chrono>
#include <optional>

namespace Aquamarine::OutputTiming {
    std::optional<std::chrono::nanoseconds> predictNextVBlank(std::chrono::nanoseconds lastVblank, std::chrono::nanoseconds now, std::chrono::nanoseconds refreshPeriod);
    std::optional<std::chrono::steady_clock::time_point> toSteadyClock(std::chrono::nanoseconds target, std::chrono::nanoseconds monotonicNow,
                                                                       std::chrono::steady_clock::time_point steadyBefore, std::chrono::steady_clock::time_point steadyAfter);
}
