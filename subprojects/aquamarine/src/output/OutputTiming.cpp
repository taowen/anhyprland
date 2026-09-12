#include "OutputTiming.hpp"

std::optional<std::chrono::nanoseconds> Aquamarine::OutputTiming::predictNextVBlank(std::chrono::nanoseconds lastVblank, std::chrono::nanoseconds now,
                                                                                    std::chrono::nanoseconds refreshPeriod) {
    if (lastVblank.count() < 0 || now.count() < 0 || refreshPeriod.count() <= 0 || lastVblank > now)
        return std::nullopt;

    if (now - lastVblank >= refreshPeriod || lastVblank > std::chrono::nanoseconds::max() - refreshPeriod)
        return std::nullopt;

    return lastVblank + refreshPeriod;
}

std::optional<std::chrono::steady_clock::time_point> Aquamarine::OutputTiming::toSteadyClock(std::chrono::nanoseconds target, std::chrono::nanoseconds monotonicNow,
                                                                                             std::chrono::steady_clock::time_point steadyBefore,
                                                                                             std::chrono::steady_clock::time_point steadyAfter) {
    if (steadyAfter < steadyBefore || target <= monotonicNow)
        return std::nullopt;

    const auto RESULT = steadyBefore + (steadyAfter - steadyBefore) / 2 + (target - monotonicNow);
    if (RESULT <= steadyAfter)
        return std::nullopt;

    return RESULT;
}
