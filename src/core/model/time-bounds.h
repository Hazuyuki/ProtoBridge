/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NS3_TIME_BOUNDS_H
#define NS3_TIME_BOUNDS_H

#include "abort.h"
#include "nstime.h"
#include "simulator.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace ns3::TimeBounds
{

using Wide = __uint128_t;

struct Scale
{
    uint64_t numerator;
    uint64_t denominator;
};

/** Native ticks per second, without constructing a potentially overflowing Time. */
inline Scale
TickScale()
{
    switch (Time::GetResolution())
    {
    case Time::Y:
        return {1, 31536000};
    case Time::D:
        return {1, 86400};
    case Time::H:
        return {1, 3600};
    case Time::MIN:
        return {1, 60};
    case Time::S:
        return {1, 1};
    case Time::MS:
        return {1000, 1};
    case Time::US:
        return {1000000, 1};
    case Time::NS:
        return {1000000000, 1};
    case Time::PS:
        return {1000000000000, 1};
    case Time::FS:
        return {1000000000000000, 1};
    default:
        NS_ABORT_MSG("Unsupported simulation time resolution");
    }
}

inline Time
Horizon()
{
    const int64_t now = Simulator::Now().GetTimeStep();
    return now < 0 ? Time(0) : Time(std::numeric_limits<int64_t>::max() - now);
}

/** Bounded schedulers reserve Time::Max() as a saturation marker. */
inline bool
CanSchedule(Time delay)
{
    return !delay.IsStrictlyNegative() && delay < Horizon();
}

template <typename... Args>
EventId
Schedule(Time delay, Args&&... args)
{
    if (!CanSchedule(delay))
    {
        return EventId();
    }
    return Simulator::Schedule(delay, std::forward<Args>(args)...);
}

template <typename... Args>
void
ScheduleWithContext(uint32_t context, Time delay, Args&&... args)
{
    if (CanSchedule(delay))
    {
        Simulator::ScheduleWithContext(context, delay, std::forward<Args>(args)...);
    }
}

inline Time
SaturatingAdd(Time a, Time b)
{
    if (a.IsStrictlyNegative() || b.IsStrictlyNegative())
    {
        return Time::Max();
    }
    const int64_t av = a.GetTimeStep();
    const int64_t bv = b.GetTimeStep();
    return bv > std::numeric_limits<int64_t>::max() - av ? Time::Max() : Time(av + bv);
}

/** Exact integer-unit conversion; matches NanoSeconds' truncation at coarse resolutions. */
inline Time
FromNanoseconds(Wide ns)
{
    const auto [num, den] = TickScale();
    const Wide maximum = std::numeric_limits<int64_t>::max();
    if (num >= 1000000000)
    {
        const uint64_t factor = num / 1000000000;
        return ns > maximum / factor ? Time::Max() : Time(static_cast<int64_t>(ns * factor));
    }
    const uint64_t divisor = (1000000000 / num) * den;
    const Wide ticks = ns / divisor;
    return ticks >= maximum ? Time::Max() : Time(static_cast<int64_t>(ticks));
}

/** Floor in nanoseconds, retaining the full native Time range. */
inline Wide
ToNanoseconds(Time time)
{
    if (time.IsStrictlyNegative())
    {
        return 0;
    }
    const auto [num, den] = TickScale();
    const Wide ticks = static_cast<uint64_t>(time.GetTimeStep());
    return num >= 1000000000 ? ticks / (num / 1000000000)
                             : ticks * ((1000000000 / num) * den);
}

/** Smallest whole-nanosecond count reaching the native saturation marker. */
inline Wide
NanosecondLimit()
{
    const auto [num, den] = TickScale();
    const Wide maximum = std::numeric_limits<int64_t>::max();
    if (num >= 1000000000)
    {
        const uint64_t factor = num / 1000000000;
        return maximum / factor + (maximum % factor != 0);
    }
    return maximum * ((1000000000 / num) * den);
}

inline Time
RoundTicks(Wide numerator, Wide denominator)
{
    const Wide maximum = std::numeric_limits<int64_t>::max();
    Wide ticks = numerator / denominator;
    if (ticks >= maximum)
    {
        return Time::Max();
    }
    const Wide remainder = numerator % denominator;
    ticks += remainder >= denominator / 2 + denominator % 2;
    return Time(static_cast<int64_t>(ticks));
}

/** Preserve ns-3's ordinary fixed-point rounding, including half-tick cases. */
inline Time
FromSeconds(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0)
    {
        return Time::Max();
    }
    if (seconds == 0.0)
    {
        return Time(0);
    }
    const auto [num, den] = TickScale();
    const double safeSeconds = ((0x1p63 - 0x1p12) * den) / num;
    if (seconds < 0x1p63 && seconds < safeSeconds)
    {
        return Seconds(seconds);
    }
    // This loose upper bound only limits intermediate width. The exact tick
    // comparison below decides representability, including coarse-unit services.
    if (seconds >= (0x1p64 * den) / num)
    {
        return Time::Max();
    }
    const uint64_t bits = std::bit_cast<uint64_t>(seconds);
    const uint64_t mantissa = (uint64_t{1} << 52) | (bits & ((uint64_t{1} << 52) - 1));
    const int shift = static_cast<int>((bits >> 52) & 0x7ff) - 1023 - 52;
    Wide numerator = Wide{mantissa} * num;
    Wide denominator = den;
    if (shift >= 0)
    {
        numerator <<= shift;
    }
    else
    {
        denominator <<= -shift;
    }
    return RoundTicks(numerator, denominator);
}

/** A guarded quotient: no overflowing division or seconds-to-Time conversion. */
inline Time
FromRate(double quantity, double rate)
{
    if (!std::isfinite(quantity) || !std::isfinite(rate) || quantity < 0.0 || rate <= 0.0)
    {
        return Time::Max();
    }
    if (quantity == 0.0)
    {
        return Time(0);
    }
    const auto [num, den] = TickScale();
    const double upperSeconds = (0x1p64 * den) / num;
    if (!(rate >= quantity / upperSeconds))
    {
        return Time::Max();
    }
    return FromSeconds(quantity / rate);
}

/** Wire serialization with a wide bit count and the DataRate rounding convention. */
inline Time
FromBitRate(Wide bits, uint64_t bitsPerSecond)
{
    if (bits == 0)
    {
        return Time(0);
    }
    if (bitsPerSecond == 0)
    {
        return Time::Max();
    }
    const auto [num, den] = TickScale();
    if (bits > std::numeric_limits<Wide>::max() / num)
    {
        return Time::Max();
    }
    const Wide numerator = bits * num;
    const Wide denominator = Wide{bitsPerSecond} * den;
    const Wide wholeSeconds = bits / bitsPerSecond;
    if (wholeSeconds < (Wide{1} << 63) && bitsPerSecond < (uint64_t{1} << 63) &&
        numerator / denominator < static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - 4096)
    {
        const auto remainder = static_cast<uint64_t>(bits % bitsPerSecond);
        return Seconds(int64x64_t(static_cast<int64_t>(wholeSeconds)) +
                       int64x64_t(remainder) / int64x64_t(bitsPerSecond));
    }
    return RoundTicks(numerator, denominator);
}

/** Nanosecond pacers keep sub-tick accumulation while supporting coarse native units. */
inline Wide
PacingSlot(uint32_t bytes, double bytesPerSecond)
{
    const Wide limit = NanosecondLimit();
    const double maxSeconds = static_cast<double>(limit) / 1e9;
    if (!std::isfinite(bytesPerSecond) || !(bytesPerSecond > 0.0) ||
        !(bytesPerSecond >= static_cast<double>(bytes) / maxSeconds))
    {
        return limit;
    }
    const double seconds = static_cast<double>(bytes) / bytesPerSecond;
    if (!(seconds < maxSeconds))
    {
        return limit;
    }
    return std::min(limit, std::max(Wide{1}, static_cast<Wide>(seconds * 1e9)));
}

/** Check the floating-point rate product before evaluating it. */
inline bool
TryBytesPerSecond(double gbps, double& result)
{
    constexpr uint64_t factor = 125000000;
    const double bound = std::numeric_limits<double>::max() / factor;
    if (!std::isfinite(gbps) || gbps < 0.0 || gbps > bound)
    {
        return false;
    }
    if (gbps == bound)
    {
        const uint64_t bits = std::bit_cast<uint64_t>(gbps);
        const uint64_t mantissa = (uint64_t{1} << 52) | (bits & ((uint64_t{1} << 52) - 1));
        const int exponent = static_cast<int>((bits >> 52) & 0x7ff) - 1023;
        const Wide maximum = Wide{(uint64_t{1} << 53) - 1} << (1023 - exponent);
        if (Wide{mantissa} * factor > maximum)
        {
            return false;
        }
    }
    result = gbps * factor;
    return true;
}

} // namespace ns3::TimeBounds

#endif // NS3_TIME_BOUNDS_H
