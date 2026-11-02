//
// Copyright (c) 2006 Georgia Tech Research Corporation
//
// SPDX-License-Identifier: GPL-2.0-only
//
// Author: Rajib Bhattacharjea<raj.b@gatech.edu>
//

#include "data-rate.h"

#include "ns3/fatal-error.h"
#include "ns3/log.h"
#include "ns3/nstime.h"

#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("DataRate");

ATTRIBUTE_HELPER_CPP(DataRate);

/* static */
bool
DataRate::DoParse(const std::string s, uint64_t* v)
{
    NS_LOG_FUNCTION(s << v);
    std::string::size_type n = s.find_first_not_of("0123456789. ");
    if (n != std::string::npos)
    { // Found non-numeric
        std::istringstream iss;
        iss.str(s.substr(0, n));
        double r;
        iss >> r;
        std::string trailer = s.substr(n, std::string::npos);
        if (trailer == "bps" || trailer == "b/s")
        {
            // bit/s
            *v = (uint64_t)r;
        }
        else if (trailer == "Bps" || trailer == "B/s")
        {
            // byte/s
            *v = (uint64_t)(r * 8);
        }
        else if (trailer == "kbps" || trailer == "kb/s" || trailer == "Kbps" || trailer == "Kb/s")
        {
            // kilobits/s
            *v = (uint64_t)(r * 1000);
        }
        else if (trailer == "kBps" || trailer == "kB/s" || trailer == "KBps" || trailer == "KB/s")
        {
            // KiloByte/s
            *v = (uint64_t)(r * 8000);
        }
        else if (trailer == "Kib/s")
        {
            // kibibit/s
            *v = (uint64_t)(r * 1024);
        }
        else if (trailer == "KiB/s")
        {
            // kibibyte/s
            *v = (uint64_t)(r * 8192);
        }
        else if (trailer == "Mbps" || trailer == "Mb/s")
        {
            // MegaBits/s
            *v = (uint64_t)(r * 1000000);
        }
        else if (trailer == "MBps" || trailer == "MB/s")
        {
            // MegaBytes/s
            *v = (uint64_t)(r * 8000000);
        }
        else if (trailer == "Mib/s")
        {
            // MebiBits/s
            *v = (uint64_t)(r * 1048576);
        }
        else if (trailer == "MiB/s")
        {
            // MebiByte/s
            *v = (uint64_t)(r * 1048576 * 8);
        }
        else if (trailer == "Gbps" || trailer == "Gb/s")
        {
            // GigaBit/s
            *v = (uint64_t)(r * 1000000000);
        }
        else if (trailer == "GBps" || trailer == "GB/s")
        {
            // GigaByte/s
            *v = (uint64_t)(r * 8 * 1000000000);
        }
        else if (trailer == "Gib/s")
        {
            // GibiBits/s
            *v = (uint64_t)(r * 1048576 * 1024);
        }
        else if (trailer == "GiB/s")
        {
            // GibiByte/s
            *v = (uint64_t)(r * 1048576 * 1024 * 8);
        }
        else
        {
            return false;
        }
        return true;
    }
    std::istringstream iss;
    iss.str(s);
    iss >> *v;
    return true;
}

DataRate::DataRate()
    : m_bps(0)
{
    NS_LOG_FUNCTION(this);
}

DataRate::DataRate(uint64_t bps)
    : m_bps(bps)
{
    NS_LOG_FUNCTION(this << bps);
    // Signed-construction guard (codex round 50, finding 4): every
    // fixed-point computation in CalculateBitsTxTime constructs
    // int64x64_t from m_bps, and a rate >= 2^63 converts negative (2^62
    // bits at 2^63 bps returned -500 ms instead of +500 ms). Rates that
    // large (an exabit per second) are beyond any modeled link; reject
    // at construction so the failure is at the configuration site.
    NS_ABORT_MSG_IF(bps >= (1ull << 63),
                    "DataRate of " << bps << " bps exceeds the supported "
                    "range (>= 2^63 bps)");
}

DataRate
DataRate::operator+(DataRate rhs) const
{
    // Pre-addition guard (codex round 51, finding 5): the non-assigning
    // operator constructs the sum without passing through the uint64_t
    // constructor's >= 2^63 abort. Check before adding; use 2^63 - 1 as
    // the operand bound so a sum landing exactly at 2^63 - 1 is still
    // representable (codex round 52: the bound is exclusive — rejecting
    // >= 2^63 - 1 spuriously aborts the representable maximum sum).
    NS_ABORT_MSG_IF(rhs.m_bps > ((1ull << 63) - 1) - m_bps,
                    "DataRate sum of " << m_bps << " + " << rhs.m_bps
                    << " bps exceeds the supported range (>= 2^63 bps)");
    return DataRate(m_bps + rhs.m_bps);
}

DataRate&
DataRate::operator+=(DataRate rhs)
{
    // Same signed-range guard as the constructors (codex round 51,
    // finding 5): compound assignment mutates m_bps in place and would
    // otherwise bypass the constructor aborts. Use 2^63 - 1 as the
    // operand bound so a sum landing exactly at 2^63 - 1 is still
    // representable (codex round 52: exclusive bound, same as above).
    NS_ABORT_MSG_IF(rhs.m_bps > ((1ull << 63) - 1) - m_bps,
                    "DataRate sum of " << m_bps << " + " << rhs.m_bps
                    << " bps exceeds the supported range (>= 2^63 bps)");
    m_bps += rhs.m_bps;
    return *this;
}

DataRate
DataRate::operator-(DataRate rhs) const
{
    NS_ASSERT_MSG(m_bps >= rhs.m_bps, "Data Rate cannot be negative.");
    return DataRate(m_bps - rhs.m_bps);
}

DataRate&
DataRate::operator-=(DataRate rhs)
{
    NS_ASSERT_MSG(m_bps >= rhs.m_bps, "Data Rate cannot be negative.");
    m_bps -= rhs.m_bps;
    return *this;
}

DataRate
DataRate::operator*(double rhs) const
{
    // Pre-scale guard (codex round 51, finding 5): the double product can
    // exceed uint64 (converts to 0 or garbage) or cross 2^63 before the
    // constructor's own check sees it — both produce a rate the fixed-point
    // time arithmetic cannot represent. Validate before the narrowing
    // conversion.
    //
    // Range in long double, result in double (codex rounds 53/54): the
    // bound 2^63 - 1 is exactly representable only in long double (80-bit
    // x87); in double it rounds to 2^63, so any guard written against a
    // double literal spuriously rejects the identity product MAX * 1.0.
    // On platforms where long double == double (MSVC, Apple ARM64) the
    // range guard degrades to an inclusive 2^63 bound and MAX * 1.0
    // still aborts there — documented limitation. The value itself is
    // converted from the DOUBLE product so ordinary in-range scaling
    // keeps its historical double-rounding quantization (round 54,
    // finding 5: converting the long-double product changed
    // DataRate(10) * 0.3 from 3 to 2).
    const long double range = static_cast<long double>(m_bps) * static_cast<long double>(rhs);
    NS_ABORT_MSG_IF(!(range >= 0.0L) || range > 9223372036854775807.0L,
                    "DataRate product of " << m_bps << " bps * " << rhs
                    << " exceeds the supported range (>= 2^63 bps)");
    // Boundary clamp (codex rounds 54/55): the guard above proves the
    // true product is at most 2^63 - 1, but the DOUBLE product rounds
    // 2^63 - 1 up to 2^63 (53-bit mantissa) and the constructor aborts
    // on the rounded value. A double product that reached 2^63 is a
    // rounding artifact — clamp to the representable maximum instead.
    // On platforms where long double == double (MSVC, Apple ARM64) this
    // clamp is unreachable for genuine overflow: the guard's maximum
    // literal rounds to 2^63 there, so range > 2^63 - 1 still aborts
    // before the double product is formed (round 55: verified with a
    // -mlong-double-64 build, where (1<<62) * 2.0 aborts as intended).
    const double prod = m_bps * rhs;
    if (prod >= 9223372036854775808.0) // == 2^63 as a double literal
    {
        return DataRate((1ull << 63) - 1);
    }
    return DataRate(static_cast<uint64_t>(prod));
}

DataRate&
DataRate::operator*=(double rhs)
{
    // Same pre-scale guard as operator*(double): in-place mutation
    // bypasses the constructor aborts. Long-double range / double
    // result for the same reasons (rounds 53/54).
    const long double range = static_cast<long double>(m_bps) * static_cast<long double>(rhs);
    NS_ABORT_MSG_IF(!(range >= 0.0L) || range > 9223372036854775807.0L,
                    "DataRate product of " << m_bps << " bps * " << rhs
                    << " exceeds the supported range (>= 2^63 bps)");
    // Same boundary clamp as operator*(double): a double product of
    // 2^63 - 1 rounds up to 2^63, which the constructor would abort on.
    // Unreachable for genuine overflow on long double == double
    // platforms for the same reason as above (round 55).
    const double prod = m_bps * rhs;
    if (prod >= 9223372036854775808.0) // == 2^63 as a double literal
    {
        m_bps = (1ull << 63) - 1;
        return *this;
    }
    m_bps = static_cast<uint64_t>(prod);
    return *this;
}

DataRate
DataRate::operator*(uint64_t rhs) const
{
    // Pre-scale guard (codex round 51, finding 5): the uint64 product
    // wraps BEFORE the constructor's check runs — 2^62 bps * 4 wrapped to
    // zero, silently creating a divide-by-zero rate. Check the operands
    // against the 2^63 bound before multiplying.
    NS_ABORT_MSG_IF(m_bps != 0 && rhs > (((1ull << 63) - 1) / m_bps),
                    "DataRate product of " << m_bps << " bps * " << rhs
                    << " exceeds the supported range (>= 2^63 bps)");
    return DataRate(m_bps * rhs);
}

DataRate&
DataRate::operator*=(uint64_t rhs)
{
    // Same pre-scale guard as operator*(uint64_t): in-place mutation
    // bypasses the constructor aborts.
    NS_ABORT_MSG_IF(m_bps != 0 && rhs > (((1ull << 63) - 1) / m_bps),
                    "DataRate product of " << m_bps << " bps * " << rhs
                    << " exceeds the supported range (>= 2^63 bps)");
    m_bps *= rhs;
    return *this;
}

Time
DataRate::CalculateBytesTxTime(uint32_t bytes) const
{
    NS_LOG_FUNCTION(this << bytes);
    // 64-bit multiplication (codex round 48, finding 3): a packet larger
    // than 512 MiB wraps the old 32-bit bytes*8 (a 536,870,969-B padded
    // packet serialized in 2 ns instead of ~21.5 ms at 200 Gbps). Wire-
    // efficiency padding can legitimately produce such bodies.
    return CalculateBitsTxTime(static_cast<uint64_t>(bytes) * 8);
}

Time
DataRate::CalculateBitsTxTime(uint64_t bits) const
{
    NS_LOG_FUNCTION(this << bits);
    // Unsigned-safe quotient (codex round 49, finding 6): constructing
    // int64x64_t directly from a uint64 >= 2^63 converts to a NEGATIVE
    // int64 (2^63 bits at 1 Tbps returned -9.2e15 ns). Split into an
    // unsigned whole-seconds quotient and a fractional remainder, both
    // well inside int64 range, and reassemble. bits beyond ~292 Gs
    // (2^64 / 64e9... i.e. durations beyond Time's own range) abort
    // inside Seconds anyway.
    const uint64_t wholeSec = bits / m_bps;
    const uint64_t remBits = bits % m_bps;
    if (wholeSec > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    {
        NS_ABORT_MSG("CalculateBitsTxTime: " << bits << " bits at "
                     << m_bps << " bps exceeds Time's representable range");
    }
    const int64x64_t fractional = int64x64_t(remBits) / int64x64_t(m_bps);
    return Seconds(int64x64_t(wholeSec) + fractional);
}

uint64_t
DataRate::GetBitRate() const
{
    NS_LOG_FUNCTION(this);
    return m_bps;
}

DataRate::DataRate(std::string rate)
{
    NS_LOG_FUNCTION(this << rate);
    bool ok = DoParse(rate, &m_bps);
    if (!ok)
    {
        NS_FATAL_ERROR("Could not parse rate: " << rate);
    }
    // Same signed-construction guard as the uint64_t constructor (codex
    // round 50, finding 4): the string parser accepts >= 2^63 bps.
    NS_ABORT_MSG_IF(m_bps >= (1ull << 63),
                    "DataRate of " << rate << " (" << m_bps
                    << " bps) exceeds the supported range (>= 2^63 bps)");
}

/* For printing of data rate */
std::ostream&
operator<<(std::ostream& os, const DataRate& rate)
{
    os << rate.GetBitRate() << "bps";
    return os;
}

/* Initialize a data rate from an input stream */
std::istream&
operator>>(std::istream& is, DataRate& rate)
{
    std::string value;
    std::getline(is, value);
    uint64_t v;
    bool ok = DataRate::DoParse(value, &v);
    if (!ok)
    {
        is.setstate(std::ios_base::failbit);
    }
    rate = DataRate(v);
    return is;
}

double
operator*(const DataRate& lhs, const Time& rhs)
{
    return rhs.GetSeconds() * lhs.GetBitRate();
}

double
operator*(const Time& lhs, const DataRate& rhs)
{
    return lhs.GetSeconds() * rhs.GetBitRate();
}

} // namespace ns3
