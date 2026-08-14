/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Link Degradation Model for simulating link errors and degradation
 */

#ifndef LINK_DEGRADATION_H
#define LINK_DEGRADATION_H

#include "ns3/object.h"
#include "ns3/ptr.h"
#include "ns3/callback.h"
#include "ns3/event-id.h"
#include "ns3/traced-callback.h"
#include "ns3/nstime.h"
#include "ns3/random-variable-stream.h"

#include <functional>
#include <deque>
#include <string>

namespace ns3
{

class Packet;

/**
 * @ingroup gpu-cluster
 * @brief Per-channel link metadata for tier-aware BER/FEC selection.
 *
 * Set during topology build so LinkDegradationModel can report which physical
 * tier (intra-node / intra-rack / inter-rack) and medium (electrical / optical)
 * a channel belongs to, rather than carrying a single global BER.
 */
struct LinkMetadata
{
    std::string linkClass;       ///< "intra_node", "intra_rack", "inter_rack"
    std::string medium;          ///< "electrical", "optical"
    double distanceMeters = 0.0; ///< Physical link distance (m)
    std::string protocol;        ///< "NVLink", "RoCEv2", "ICI", "XCCL", ...
    double bandwidthGbps = 0.0;  ///< Per-link bandwidth (Gbps)
    double ber = 0.0;            ///< Bit error rate for this channel
    std::string fecProfile;      ///< "none", "RS(544,514,15)", ...
};

/**
 * @ingroup gpu-cluster
 * @brief Error injection mode
 */
enum class ErrorMode : uint8_t
{
    INDEPENDENT = 0,    ///< Each packet error is independent (default)
    BURST = 1           ///< Errors occur in bursts at codeword granularity
};

/**
 * @ingroup gpu-cluster
 * @brief Degradation trigger type
 */
enum class DegradationTrigger : uint8_t
{
    NONE = 0,           ///< No degradation
    BIT_ERROR = 1,      ///< Bit error rate increase
    LINK_FLAP = 2,      ///< Link going up/down
    CONGESTION = 3,     ///< Congestion-induced degradation
    THERMAL = 4,        ///< Thermal throttling
    EXTERNAL = 5        ///< External signal (e.g., stress test)
};

/**
 * @ingroup gpu-cluster
 * @brief Link state
 */
enum class LinkState : uint8_t
{
    UP = 0,             ///< Link is up and healthy
    DEGRADED = 1,       ///< Link is degraded
    DOWN = 2            ///< Link is down
};

/**
 * @ingroup gpu-cluster
 * @brief Link Degradation Model
 *
 * This class models link degradation and error conditions:
 * - Dynamic bit error rate (BER) adjustment
 * - Link flapping (up/down transitions)
 * - Congestion-induced packet loss
 * - Thermal throttling effects
 * - External signal triggers
 * - Burst/correlated error injection at codeword granularity
 *
 * The model can be attached to a channel to inject errors
 * based on configured degradation scenarios.
 */
class LinkDegradationModel : public Object
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    LinkDegradationModel();
    ~LinkDegradationModel() override;

    // Delete copy constructor and assignment operator
    LinkDegradationModel(const LinkDegradationModel&) = delete;
    LinkDegradationModel& operator=(const LinkDegradationModel&) = delete;

    /**
     * @brief Set the random number stream for error generation
     * @param stream The random number stream
     */
    void SetRandomStream(Ptr<RandomVariableStream> stream);

    /**
     * @brief Set the bit error rate (BER)
     * @param ber Bit error rate (e.g., 1e-9)
     */
    void SetBer(double ber);

    /**
     * @brief Get the current bit error rate
     * @return Current BER
     */
    double GetBer() const;

    /**
     * @brief Set per-channel link metadata (tier, medium, BER, FEC profile)
     *
     * The metadata's ber field is used as the channel's BER; SetBer() is
     * equivalent to setting ber alone. Stored for tracing and FEC selection.
     */
    void SetLinkMetadata(const LinkMetadata& meta);

    /**
     * @brief Get per-channel link metadata
     */
    const LinkMetadata& GetLinkMetadata() const;

    /**
     * @brief Set the packet loss rate (independent of BER)
     * @param rate Packet loss rate (0.0 to 1.0)
     */
    void SetPacketLossRate(double rate);

    /**
     * @brief Get the packet loss rate
     * @return Current packet loss rate
     */
    double GetPacketLossRate() const;

    /**
     * @brief Get a uniform random value in [0,1) for external sampling
     */
    double GetRandomValue();

    /**
     * @brief Set the current link state
     * @param state Link state
     */
    void SetLinkState(LinkState state);

    /**
     * @brief Get the current link state
     * @return Current link state
     */
    LinkState GetLinkState() const;

    /**
     * @brief Check if the link is operational
     * @return True if link is up (not down)
     */
    bool IsLinkUp() const;

    /**
     * @brief Set the current degradation trigger
     * @param trigger Degradation trigger
     */
    void SetTrigger(DegradationTrigger trigger);

    /**
     * @brief Get the current degradation trigger
     * @return Current trigger
     */
    DegradationTrigger GetTrigger() const;

    /**
     * @brief Apply degradation based on trigger
     * @param trigger The degradation trigger
     * @param severity Severity level (0.0 to 1.0)
     */
    void ApplyDegradation(DegradationTrigger trigger, double severity);

    /**
     * @brief Clear degradation (return to normal)
     */
    void ClearDegradation();

    /**
     * @brief Process a packet through the degradation model
     * @param packet The packet to process
     * @return True if packet should be delivered, false if dropped
     */
    bool ProcessPacket(Ptr<Packet> packet);

    /**
     * @brief Schedule a degradation event
     * @param delay Time until degradation
     * @param trigger Degradation trigger
     * @param severity Severity level
     * @param duration Duration of degradation (0 for permanent)
     */
    void ScheduleDegradation(Time delay, DegradationTrigger trigger,
                             double severity, Time duration);

    /**
     * @brief Cancel any scheduled degradation
     */
    void CancelScheduledDegradation();

    /**
     * @brief Callback type for link state change
     */
    typedef Callback<void, LinkState, LinkState> LinkStateCallback;

    /**
     * @brief Set callback for link state changes
     * @param cb Callback function
     */
    void SetLinkStateCallback(LinkStateCallback cb);

    /**
     * @brief Callback type for packet drop
     */
    typedef Callback<void, Ptr<Packet>, DegradationTrigger> PacketDropCallback;

    /**
     * @brief Set callback for packet drops
     * @param cb Callback function
     */
    void SetPacketDropCallback(PacketDropCallback cb);

    // Burst error mode
    void SetErrorMode(ErrorMode mode);
    ErrorMode GetErrorMode() const;

    void SetCodewordSize(uint32_t sizeBytes);
    uint32_t GetCodewordSize() const;

    void SetBurstLength(uint32_t codewords);
    uint32_t GetBurstLength() const;

    void SetBurstArrivalRate(double rate);
    double GetBurstArrivalRate() const;

    bool IsInBurst() const;

    // Traced callbacks
    typedef TracedCallback<double, double> BerChangeTracedCallback;        ///< Old BER, new BER
    typedef TracedCallback<LinkState, LinkState> StateChangeTracedCallback; ///< Old state, new state
    typedef TracedCallback<uint32_t, DegradationTrigger> PacketDropTracedCallback; ///< Size, trigger
    typedef TracedCallback<uint32_t, uint32_t> BurstTracedCallback; ///< Burst start: length, remaining

  private:
    void DoDispose() override;

    /**
     * @brief Calculate probability of packet error based on BER
     * @param packetSize Packet size in bytes
     * @return Probability of error
     */
    double CalculatePacketErrorProbability(uint32_t packetSize) const;

    /**
     * @brief Process a packet in burst error mode at codeword granularity
     * @param packet The packet to process
     * @return True if packet should be delivered, false if dropped
     */
    bool ProcessPacketBurstMode(Ptr<Packet> packet);

    /**
     * @brief Determine whether a new burst starts at this codeword
     * @return True if a burst should begin
     */
    bool ShouldStartBurst();

    /**
     * @brief Handle scheduled degradation event
     */
    void OnDegradationEvent();

    /**
     * @brief Handle scheduled recovery event
     */
    void OnRecoveryEvent();

    // Member variables
    Ptr<RandomVariableStream> m_random;         ///< Random number stream
    Ptr<UniformRandomVariable> m_fallbackRandom; ///< Fallback RNG when m_random is null
    double m_ber;                               ///< Bit error rate
    double m_baseBer;                           ///< Base (normal) BER
    LinkMetadata m_linkMetadata;                ///< Per-channel metadata (tier/medium/ber/fec)
    double m_packetLossRate;                    ///< Packet loss rate
    double m_basePacketLossRate;                ///< Base packet loss rate
    LinkState m_linkState;                      ///< Current link state
    DegradationTrigger m_currentTrigger;        ///< Current degradation trigger
    double m_severity;                          ///< Current degradation severity

    EventId m_degradationEvent;                 ///< Scheduled degradation event
    DegradationTrigger m_scheduledTrigger;      ///< Trigger for scheduled degradation
    double m_scheduledSeverity;                 ///< Severity for scheduled degradation
    EventId m_recoveryEvent;                    ///< Scheduled recovery event

    LinkStateCallback m_linkStateCallback;      ///< Link state change callback
    PacketDropCallback m_packetDropCallback;    ///< Packet drop callback

    // Statistics
    uint64_t m_totalPackets;                    ///< Total packets processed
    uint64_t m_droppedPackets;                  ///< Dropped packets
    uint64_t m_errorPackets;                    ///< Packets with errors

    // Burst error mode state
    ErrorMode m_errorMode;                      ///< Error injection mode
    uint32_t m_codewordSize;                    ///< Codeword size in bytes (for burst granularity)
    uint32_t m_burstLength;                     ///< Mean burst length in codewords
    double m_burstArrivalRate;                  ///< Probability of burst arrival per codeword
    bool m_inBurst;                             ///< Currently in a burst
    uint32_t m_burstRemaining;                  ///< Remaining codewords in current burst
    uint64_t m_totalBursts;                     ///< Total bursts started

    // Traced sources
    TracedCallback<double, double> m_berChangeTrace;       ///< BER change trace
    TracedCallback<LinkState, LinkState> m_stateChangeTrace; ///< State change trace
    TracedCallback<uint32_t, DegradationTrigger> m_packetDropTrace; ///< Packet drop trace
    TracedCallback<uint32_t, uint32_t> m_burstTrace; ///< Burst trace: burst length, remaining
};

} // namespace ns3

#endif /* LINK_DEGRADATION_H */
