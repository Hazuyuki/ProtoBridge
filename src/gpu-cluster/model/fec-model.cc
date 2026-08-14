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
    // RS symbol = 10 bits (GF(2^10)), matching Python surrogate
    // P(symbol error) = 1 - (1 - BER)^10
    return 1.0 - std::pow(1.0 - ber, 10.0);
}

double
FecModel::ComputePostFecBer(double preFecBer) const
{
    NS_LOG_FUNCTION(this << preFecBer);

    if (preFecBer <= 0.0)
    {
        return 0.0;
    }

    double pSym = ComputeSymbolErrorProbability(preFecBer);

    // P(uncorrectable) = 1 - sum_{i=0}^{T} C(N,i) * p^i * (1-p)^(N-i)
    // For numerical stability with very small p, use log-space computation
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

    if (preFecBer <= 0.0)
    {
        return FecResult::NO_ERROR;
    }

    double pSym = ComputeSymbolErrorProbability(preFecBer);

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

uint32_t
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
    return static_cast<uint32_t>((encodedBits + 7) / 8);
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
