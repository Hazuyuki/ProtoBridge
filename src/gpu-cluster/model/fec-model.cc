/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 */

#include "fec-model.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/double.h"
#include "ns3/uinteger.h"
#include "ns3/boolean.h"
#include "ns3/random-variable-stream.h"

#include <cmath>
#include <algorithm>
#include <numeric>

namespace ns3
{

namespace
{
constexpr uint32_t RS_SYMBOL_BITS = 10;
}

NS_LOG_COMPONENT_DEFINE("FecModel");

NS_OBJECT_ENSURE_REGISTERED(FecModel);

TypeId
FecModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::FecModel")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<FecModel>()
                            .AddAttribute("N",
                                          "Total codeword length (data + parity symbols)",
                                          UintegerValue(544),
                                          MakeUintegerAccessor(&FecModel::m_n),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("K",
                                          "Data symbols per codeword",
                                          UintegerValue(514),
                                          MakeUintegerAccessor(&FecModel::m_k),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("T",
                                          "Error correction capability in symbols",
                                          UintegerValue(15),
                                          MakeUintegerAccessor(&FecModel::m_t),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("EncodeLatency",
                                          "FEC encode latency per codeword",
                                          TimeValue(NanoSeconds(50)),
                                          MakeTimeAccessor(&FecModel::m_encodeLatency),
                                          MakeTimeChecker())
                            .AddAttribute("DecodeLatency",
                                          "FEC decode latency per codeword",
                                          TimeValue(NanoSeconds(80)),
                                          MakeTimeAccessor(&FecModel::m_decodeLatency),
                                          MakeTimeChecker())
                            .AddAttribute("Enabled",
                                          "Whether FEC is enabled",
                                          BooleanValue(true),
                                          MakeBooleanAccessor(&FecModel::m_enabled),
                                          MakeBooleanChecker())
                            .AddTraceSource("FecDecode",
                                            "FEC decode result",
                                            MakeTraceSourceAccessor(&FecModel::m_fecDecodeTrace),
                                            "ns3::FecModel::FecDecodeTracedCallback");
    return tid;
}

FecModel::FecModel()
    : m_n(544),
      m_k(514),
      m_t(15),
      m_encodeLatency(NanoSeconds(50)),
      m_decodeLatency(NanoSeconds(80)),
      m_enabled(true),
      m_rng(CreateObject<UniformRandomVariable>()),
      m_noErrorCount(0),
      m_correctableCount(0),
      m_uncorrectableCount(0)
{
    NS_LOG_FUNCTION(this);
}

FecModel::~FecModel()
{
    NS_LOG_FUNCTION(this);
}

void
FecModel::DoDispose()
{
    NS_LOG_FUNCTION(this);
    Object::DoDispose();
}

void
FecModel::SetFecParams(uint32_t n, uint32_t k, uint32_t t)
{
    NS_LOG_FUNCTION(this << n << k << t);
    NS_ASSERT_MSG(n > k, "N must be greater than K");
    NS_ASSERT_MSG(2 * t <= n - k, "T must satisfy 2T <= N-K for MDS codes");
    m_n = n;
    m_k = k;
    m_t = t;
}

uint32_t
FecModel::GetN() const
{
    return m_n;
}

uint32_t
FecModel::GetK() const
{
    return m_k;
}

uint32_t
FecModel::GetT() const
{
    return m_t;
}

void
FecModel::SetEncodeLatency(Time latency)
{
    NS_LOG_FUNCTION(this << latency);
    m_encodeLatency = latency;
}

Time
FecModel::GetEncodeLatency() const
{
    return m_encodeLatency;
}

void
FecModel::SetDecodeLatency(Time latency)
{
    NS_LOG_FUNCTION(this << latency);
    m_decodeLatency = latency;
}

Time
FecModel::GetDecodeLatency() const
{
    return m_decodeLatency;
}

double
FecModel::GetCodeRate() const
{
    return static_cast<double>(m_k) / static_cast<double>(m_n);
}

double
FecModel::GetBandwidthOverhead() const
{
    return static_cast<double>(m_n - m_k) / static_cast<double>(m_n);
}

double
FecModel::GetEffectiveRateMultiplier() const
{
    return GetCodeRate();
}

double
FecModel::ComputeSymbolErrorProbability(double ber) const
{
    if (ber >= 1.0)
    {
        return 1.0;
    }
    // No representable pSym at the top of the range (codex rounds 55/56):
    // for BER above ~0.975 the true 1 - (1-BER)^10 is within 2^-53 of
    // 1.0, so no IEEE-754 double can hold it — expm1/log1p rounds to
    // exactly 1.0 and the callers' log(1 - pSym) raises FE_DIVBYZERO
    // under fenv traps. Saturate at the helper boundary; the CALLERS
    // treat pSym == 1.0 as certain corruption without evaluating
    // log(1 - pSym) (round 56: the helper returning 1.0 is not enough —
    // ComputePostFecBer / DecodeCodeword must skip the log-space sum).
    if (ber >= 1.0 - 1e-13)
    {
        return 1.0;
    }
    // Near-zero endpoint (codex round 54): for BER <= ~2^-53 the true
    // pSym is below double resolution; the raw 1 - pow form rounds to
    // exactly 0 and the callers' log(pSym) raises FE_DIVBYZERO. expm1/
    // log1p stay accurate down to the subnormal range.
    // Non-finite BER (NaN, -Inf): the caller-side guards in
    // ComputePostFecBer / DecodeCodeword intercept these before the
    // log-space sums, so a NaN here degrades to the 1.0 / UNCORRECTABLE
    // early-out instead of an FE_INVALID trap.
    return -std::expm1(10.0 * std::log1p(-ber));
}

double
FecModel::ComputePostFecBer(double preFecBer) const
{
    NS_LOG_FUNCTION(this << preFecBer);

    // Round 58: the NaN guard must precede ANY ordered comparison —
    // `preFecBer <= 0.0` raises FE_INVALID for NaN under fenv traps, and
    // std::isnan is an unordered quiet predicate that does not signal.
    // (The round-57 reorder kept the `<= 0.0` check first; the direct
    // ComputePostFecBer(NaN) call with traps enabled still aborted.)
    if (std::isnan(preFecBer))
    {
        // NaN BER: no meaningful symbol error rate; degrade to the clean
        // path rather than propagate NaN into log-space sums.
        return 0.0;
    }
    if (preFecBer <= 0.0)
    {
        return 0.0;
    }

    // BER at or above 1.0: every codeword is certainly corrupted and
    // certainly uncorrectable (round 53). Return 1.0 directly instead of
    // evaluating the log-space sum below: at BER = 1.0 the symbol-error
    // probability is exactly 1, and std::log(1.0 - pSym) raises
    // FE_DIVBYZERO under fenv traps or IEEE diagnostics; a BER > 1.0
    // (reachable via LinkDegradationModel's severity scaling, which only
    // bounds the attribute input at [0,1]) would further raise
    // FE_INVALID from std::log of a negative number.
    if (preFecBer >= 1.0)
    {
        return 1.0;
    }

    double pSym = ComputeSymbolErrorProbability(preFecBer);

    // pSym at (or indistinguishable from) 1.0 — helper saturation branch
    // for BER >= ~0.975 where no double is representable closer to 1, or
    // NaN input. Skip the log-space sum: std::log(1.0 - pSym) would be
    // log(0) -> FE_DIVBYZERO (round 56; round 55's helper-side saturation
    // alone was not enough). The codeword is certainly uncorrectable, so
    // the post-FEC BER is 1.0.
    if (!(pSym < 1.0))
    {
        return 1.0;
    }

    // P(uncorrectable) = 1 - sum_{i=0}^{T} C(N,i) * p^i * (1-p)^(N-i)
    // For numerical stability with very small p, use log-space computation
    //
    // Rounded endpoints (codex round 54): expm1/log1p keep pSym off the
    // exact 0/1 boundaries for BER in (0, 1), so std::log(pSym) and
    // std::log(1 - pSym) below stay finite — both were FE_DIVBYZERO traps
    // at BER ~1e-18 / ~0.99 respectively. The >= 1.0 early-outs above and
    // here keep that property for the saturated top of the range too.
    double pUncorrectable = 1.0;
    double cumulativeCorrectable = 0.0;

    for (uint32_t i = 0; i <= m_t && i <= m_n; i++)
    {
        // C(N,i) * p^i * (1-p)^(N-i)
        double logTerm = 0.0;

        // Compute C(N,i) using log to avoid overflow
        for (uint32_t j = 0; j < i; j++)
        {
            logTerm += std::log(static_cast<double>(m_n - j));
            logTerm -= std::log(static_cast<double>(j + 1));
        }

        if (i > 0)
        {
            logTerm += static_cast<double>(i) * std::log(pSym);
        }
        if (m_n > i)
        {
            logTerm += static_cast<double>(m_n - i) * std::log(1.0 - pSym);
        }

        double term = std::exp(logTerm);
        cumulativeCorrectable += term;
    }

    pUncorrectable = 1.0 - cumulativeCorrectable;
    if (pUncorrectable < 0.0) pUncorrectable = 0.0;
    if (pUncorrectable > 1.0) pUncorrectable = 1.0;

    NS_LOG_DEBUG("Pre-FEC BER=" << preFecBer
                                << " symbol err=" << pSym
                                << " post-FEC uncorrectable=" << pUncorrectable);

    return pUncorrectable;
}

FecResult
FecModel::DecodeCodeword(double preFecBer) const
{
    NS_LOG_FUNCTION(this << preFecBer);

    if (!m_enabled)
    {
        return FecResult::NO_ERROR;
    }

    // Round 58: isnan precedes every ordered comparison — see
    // ComputePostFecBer. The round-57 reorder kept `<= 0.0` first, so the
    // direct DecodeCodeword(NaN) call with traps enabled still aborted
    // (codex probe: rc = -8 = SIGFPE).
    if (std::isnan(preFecBer))
    {
        // NaN BER: degrade to the clean path.
        return FecResult::NO_ERROR;
    }
    if (preFecBer <= 0.0)
    {
        return FecResult::NO_ERROR;
    }

    // Same BER >= 1.0 early-out as ComputePostFecBer (round 53): the
    // log-space sum below would raise FE_DIVBYZERO (BER == 1.0) or
    // FE_INVALID (BER > 1.0) and then deterministically conclude
    // UNCORRECTABLE anyway, so skip the numerically invalid evaluation.
    if (preFecBer >= 1.0)
    {
        m_fecDecodeTrace(0, FecResult::UNCORRECTABLE);
        m_uncorrectableCount++;
        return FecResult::UNCORRECTABLE;
    }

    double pSym = ComputeSymbolErrorProbability(preFecBer);

    // pSym at (or indistinguishable from) 1.0 — helper saturation branch
    // for BER >= ~0.975 where no double is representable closer to 1, or
    // NaN input. Skip the log-space sums: std::log(1.0 - pSym) would be
    // log(0) -> FE_DIVBYZERO (round 56). The codeword is certainly
    // uncorrectable.
    if (!(pSym < 1.0))
    {
        m_fecDecodeTrace(0, FecResult::UNCORRECTABLE);
        m_uncorrectableCount++;
        return FecResult::UNCORRECTABLE;
    }

    // Compute probability of exactly 0 errors and probability of correctable errors
    double pNoError = std::pow(1.0 - pSym, static_cast<double>(m_n));

    // Sum of P(exactly i errors) for i=1..T
    double pCorrectable = 0.0;
    for (uint32_t i = 1; i <= m_t && i <= m_n; i++)
    {
        double logTerm = 0.0;
        for (uint32_t j = 0; j < i; j++)
        {
            logTerm += std::log(static_cast<double>(m_n - j));
            logTerm -= std::log(static_cast<double>(j + 1));
        }
        logTerm += static_cast<double>(i) * std::log(pSym);
        logTerm += static_cast<double>(m_n - i) * std::log(1.0 - pSym);
        pCorrectable += std::exp(logTerm);
    }

    // Draw random sample to determine outcome using per-object RNG
    double sample = m_rng->GetValue();

    FecResult result;
    if (sample < pNoError)
    {
        result = FecResult::NO_ERROR;
    }
    else if (sample < pNoError + pCorrectable)
    {
        result = FecResult::CORRECTABLE;
    }
    else
    {
        result = FecResult::UNCORRECTABLE;
    }

    m_fecDecodeTrace(0, result);
    if (result == FecResult::NO_ERROR)
    {
        m_noErrorCount++;
    }
    else if (result == FecResult::CORRECTABLE)
    {
        m_correctableCount++;
    }
    else
    {
        m_uncorrectableCount++;
    }
    return result;
}

FecResult
FecModel::DecodePacket(double preFecBer, uint32_t payloadSize) const
{
    NS_LOG_FUNCTION(this << preFecBer << payloadSize);

    if (!m_enabled)
    {
        return FecResult::NO_ERROR;
    }

    // Round 57: size-independent NaN policy. DecodeCodeword (single-codeword
    // branch) and ComputePostFecBer both degrade NaN to the clean path, but
    // the multi-codeword branch below recomputes pSym and reaches different
    // comparisons, so NaN,642 -> NO_ERROR while NaN,643 -> UNCORRECTABLE.
    // Intercept at entry to match the clean-path policy for any payload.
    // Round 58: the entry guard also keeps NaN off the `<= 0.0` comparison
    // below, so DecodePacket(NaN, traps) exits cleanly while the direct
    // ComputePostFecBer / DecodeCodeword entry points needed their own
    // isnan-first reorder (codex probe: both returned 0 / NO_ERROR with
    // invalid=0).
    if (std::isnan(preFecBer))
    {
        return FecResult::NO_ERROR;
    }

    if (preFecBer <= 0.0)
    {
        return FecResult::NO_ERROR;
    }

    uint64_t payloadBits = static_cast<uint64_t>(payloadSize) * 8;
    uint64_t dataBitsPerCodeword = static_cast<uint64_t>(m_k) * RS_SYMBOL_BITS;
    uint32_t numCodewords = static_cast<uint32_t>(
        (payloadBits + dataBitsPerCodeword - 1) / dataBitsPerCodeword);

    if (numCodewords <= 1)
    {
        return DecodeCodeword(preFecBer);
    }

    // For multi-codeword packets, compute combined probability
    double pUncorrectable = ComputePostFecBer(preFecBer);
    double pAnyUncorrectable = 1.0 - std::pow(1.0 - pUncorrectable, static_cast<double>(numCodewords));

    // Compute the disjoint packet outcomes: all codewords clean, at least one
    // correctable error and no uncorrectable codeword, or any uncorrectable
    // codeword.
    double pSym = ComputeSymbolErrorProbability(preFecBer);
    double pNoErrorCodeword = std::pow(1.0 - pSym, static_cast<double>(m_n));
    double pAllClean = std::pow(pNoErrorCodeword, static_cast<double>(numCodewords));
    double pCorrectable = std::max(0.0, 1.0 - pAllClean - pAnyUncorrectable);
    double sample = m_rng->GetValue();

    FecResult result;
    if (sample < pAllClean)
    {
        result = FecResult::NO_ERROR;
        m_noErrorCount++;
    }
    else if (sample < pAllClean + pCorrectable)
    {
        result = FecResult::CORRECTABLE;
        m_correctableCount++;
    }
    else
    {
        result = FecResult::UNCORRECTABLE;
        m_uncorrectableCount++;
    }
    m_fecDecodeTrace(0, result);
    return result;
}

uint64_t
FecModel::GetEncodedSize(uint32_t payloadSize) const
{
    if (!m_enabled)
    {
        return payloadSize;
    }

    uint64_t payloadBits = static_cast<uint64_t>(payloadSize) * 8;
    uint64_t dataBitsPerCodeword = static_cast<uint64_t>(m_k) * RS_SYMBOL_BITS;
    uint64_t numCodewords = (payloadBits + dataBitsPerCodeword - 1) / dataBitsPerCodeword;
    uint64_t encodedBits = numCodewords * m_n * RS_SYMBOL_BITS;
    // uint64 return (codex round 51, finding 2): the encoded size of a
    // ~4-GB payload crosses UINT32_MAX; the old uint32 return narrowed it
    // below the payload size, and the padding site's `encoded > payload`
    // test silently skipped the parity expansion entirely.
    return (encodedBits + 7) / 8;
}

uint32_t
FecModel::GetPayloadSize(uint32_t encodedSize) const
{
    if (!m_enabled)
    {
        return encodedSize;
    }

    uint64_t encodedBits = static_cast<uint64_t>(encodedSize) * 8;
    uint64_t codewordBits = static_cast<uint64_t>(m_n) * RS_SYMBOL_BITS;
    uint64_t numCodewords = encodedBits / codewordBits;
    uint64_t payloadBits = numCodewords * m_k * RS_SYMBOL_BITS;
    return static_cast<uint32_t>(payloadBits / 8);
}

void
FecModel::SetEnabled(bool enabled)
{
    NS_LOG_FUNCTION(this << enabled);
    m_enabled = enabled;
}

bool
FecModel::IsEnabled() const
{
    return m_enabled;
}

uint64_t
FecModel::GetNoErrorCount() const
{
    return m_noErrorCount;
}

uint64_t
FecModel::GetCorrectableCount() const
{
    return m_correctableCount;
}

uint64_t
FecModel::GetUncorrectableCount() const
{
    return m_uncorrectableCount;
}

} // namespace ns3
