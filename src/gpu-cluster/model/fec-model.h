/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Forward Error Correction Model: parameterized FEC(N,K,T)
 */

#ifndef FEC_MODEL_H
#define FEC_MODEL_H

#include "ns3/object.h"
#include "ns3/ptr.h"
#include "ns3/nstime.h"
#include "ns3/traced-callback.h"
#include "ns3/random-variable-stream.h"

#include <cstdint>

namespace ns3
{

class Packet;

/**
 * @ingroup gpu-cluster
 * @brief FEC correction result
 */
enum class FecResult : uint8_t
{
    NO_ERROR = 0,       ///< No errors detected
    CORRECTABLE = 1,    ///< Errors detected and corrected
    UNCORRECTABLE = 2   ///< Too many errors, cannot correct
};

/**
 * @ingroup gpu-cluster
 * @brief Forward Error Correction Model
 *
 * Parameterized FEC(N,K,T) model:
 * - N = total codeword length (data + parity symbols)
 * - K = data symbols per codeword
 * - T = error correction capability (can correct up to T symbol errors)
 * - Code rate = K/N
 * - Bandwidth overhead = (N-K)/N
 *
 * The model computes:
 * - Post-FEC uncorrectable probability from pre-FEC BER
 * - Encode latency (at sender)
 * - Decode latency (at receiver)
 * - Bandwidth overhead as effective rate reduction
 *
 * Uncorrectable codewords trigger NACK for link-level retry.
 */
class FecModel : public Object
{
  public:
    static TypeId GetTypeId();

    FecModel();
    ~FecModel() override;

    FecModel(const FecModel&) = delete;
    FecModel& operator=(const FecModel&) = delete;

    /**
     * @brief Configure FEC parameters
     * @param n Total codeword length
     * @param k Data symbols per codeword
     * @param t Error correction capability
     */
    void SetFecParams(uint32_t n, uint32_t k, uint32_t t);

    uint32_t GetN() const;
    uint32_t GetK() const;
    uint32_t GetT() const;

    /**
     * @brief Set encode latency per codeword
     */
    void SetEncodeLatency(Time latency);
    Time GetEncodeLatency() const;

    /**
     * @brief Set decode latency per codeword
     */
    void SetDecodeLatency(Time latency);
    Time GetDecodeLatency() const;

    /**
     * @brief Get code rate K/N
     */
    double GetCodeRate() const;

    /**
     * @brief Get bandwidth overhead fraction (N-K)/N
     */
    double GetBandwidthOverhead() const;

    /**
     * @brief Get effective data rate multiplier (K/N)
     * When FEC is enabled, effective throughput is reduced by this factor.
     */
    double GetEffectiveRateMultiplier() const;

    /**
     * @brief Compute post-FEC uncorrectable probability
     * @param preFecBer Pre-FEC bit error rate
     * @return Probability that a codeword is uncorrectable after FEC
     *
     * Uses binomial model: P(uncorrectable) = 1 - sum_{i=0}^{T} C(N,i) * p^i * (1-p)^(N-i)
     * where p is the symbol error probability derived from preFecBer.
     */
    double ComputePostFecBer(double preFecBer) const;

    /**
     * @brief Process a received codeword through FEC decoding
     * @param preFecBer Pre-FEC BER for this link
     * @return FecResult indicating whether the codeword was error-free, correctable, or uncorrectable
     *
     * Uses random sampling: draws a uniform random value and compares against
     * the probability distribution of 0 errors, correctable errors, and uncorrectable errors.
     */
    FecResult DecodeCodeword(double preFecBer) const;

    /**
     * @brief Decode an entire packet (multiple codewords) through FEC
     * @param preFecBer Pre-FEC BER for this link
     * @param payloadSize Payload size in bytes (to determine number of codewords)
     * @return FecResult: UNCORRECTABLE if any codeword is uncorrectable,
     *         CORRECTABLE if at least one correctable and none uncorrectable,
     *         NO_ERROR if all codewords pass without errors
     *
     * For multi-codeword packets, P(any uncorrectable) = 1 - (1 - P_uncorrectable)^numCodewords
     */
    FecResult DecodePacket(double preFecBer, uint32_t payloadSize) const;

    /**
     * @brief Apply FEC encoding to a packet (returns encoded size)
     * @param payloadSize Original payload size in bytes
     * @return Encoded size in bytes (payload * N/K, rounded up to codeword boundary)
     */
    uint32_t GetEncodedSize(uint32_t payloadSize) const;

    /**
     * @brief Get original payload size from encoded size
     * @param encodedSize Encoded size in bytes
     * @return Original payload size in bytes
     */
    uint32_t GetPayloadSize(uint32_t encodedSize) const;

    /**
     * @brief Enable or disable FEC
     */
    void SetEnabled(bool enabled);
    bool IsEnabled() const;

    uint64_t GetNoErrorCount() const;
    uint64_t GetCorrectableCount() const;
    uint64_t GetUncorrectableCount() const;

    /**
     * @brief Compute symbol error probability from BER
     * @param ber Bit error rate
     * @return Probability that a symbol (10-bit RS symbol) has at least one bit error
     */
    double ComputeSymbolErrorProbability(double ber) const;

    typedef TracedCallback<uint32_t, FecResult> FecDecodeTracedCallback; ///< Codeword index, result

  private:
    void DoDispose() override;

    uint32_t m_n;              ///< Total codeword length (symbols)
    uint32_t m_k;              ///< Data symbols per codeword
    uint32_t m_t;              ///< Error correction capability (symbols)
    Time m_encodeLatency;      ///< Encode latency per codeword
    Time m_decodeLatency;      ///< Decode latency per codeword
    bool m_enabled;            ///< Whether FEC is enabled
    Ptr<UniformRandomVariable> m_rng; ///< Random variable for FEC sampling
    mutable uint64_t m_noErrorCount;
    mutable uint64_t m_correctableCount;
    mutable uint64_t m_uncorrectableCount;

    TracedCallback<uint32_t, FecResult> m_fecDecodeTrace; ///< Decode result trace
};

} // namespace ns3

#endif /* FEC_MODEL_H */
