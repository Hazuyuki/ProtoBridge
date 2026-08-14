/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 */

#include "link-degradation.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/double.h"
#include "ns3/uinteger.h"
#include "ns3/enum.h"

#include <algorithm>

#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("LinkDegradationModel");

NS_OBJECT_ENSURE_REGISTERED(LinkDegradationModel);

TypeId
LinkDegradationModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::LinkDegradationModel")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<LinkDegradationModel>()
                            .AddAttribute("Ber",
                                          "Bit error rate",
                                          DoubleValue(1e-12),  // Default very low BER
                                          MakeDoubleAccessor(&LinkDegradationModel::SetBer,
                                                             &LinkDegradationModel::GetBer),
                                          MakeDoubleChecker<double>(0.0, 1.0))
                            .AddAttribute("PacketLossRate",
                                          "Packet loss rate",
                                          DoubleValue(0.0),
                                          MakeDoubleAccessor(&LinkDegradationModel::SetPacketLossRate,
                                                             &LinkDegradationModel::GetPacketLossRate),
                                          MakeDoubleChecker<double>(0.0, 1.0))
                            .AddTraceSource("BerChange",
                                            "Bit error rate changed",
                                            MakeTraceSourceAccessor(&LinkDegradationModel::m_berChangeTrace),
                                            "ns3::LinkDegradationModel::BerChangeTracedCallback")
                            .AddTraceSource("StateChange",
                                            "Link state changed",
                                            MakeTraceSourceAccessor(&LinkDegradationModel::m_stateChangeTrace),
                                            "ns3::LinkDegradationModel::StateChangeTracedCallback")
                            .AddTraceSource("PacketDrop",
                                            "Packet was dropped",
                                            MakeTraceSourceAccessor(&LinkDegradationModel::m_packetDropTrace),
                                            "ns3::LinkDegradationModel::PacketDropTracedCallback")
                            .AddAttribute("ErrorMode",
                                          "Error injection mode",
                                          EnumValue<ErrorMode>(ErrorMode::INDEPENDENT),
                                          MakeEnumAccessor<ErrorMode>(&LinkDegradationModel::SetErrorMode,
                                                           &LinkDegradationModel::GetErrorMode),
                                          MakeEnumChecker<ErrorMode>(
                                              ErrorMode::INDEPENDENT, "ns3::ErrorMode::INDEPENDENT",
                                              ErrorMode::BURST, "ns3::ErrorMode::BURST"))
                            .AddAttribute("CodewordSize",
                                          "Codeword size in bytes for burst error granularity",
                                          UintegerValue(256),
                                          MakeUintegerAccessor(&LinkDegradationModel::m_codewordSize),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("BurstLength",
                                          "Mean burst length in codewords",
                                          UintegerValue(4),
                                          MakeUintegerAccessor(&LinkDegradationModel::m_burstLength),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("BurstArrivalRate",
                                          "Probability of burst arrival per codeword",
                                          DoubleValue(1e-6),
                                          MakeDoubleAccessor(&LinkDegradationModel::m_burstArrivalRate),
                                          MakeDoubleChecker<double>(0.0, 1.0))
                            .AddTraceSource("Burst",
                                            "Burst error event",
                                            MakeTraceSourceAccessor(&LinkDegradationModel::m_burstTrace),
                                            "ns3::LinkDegradationModel::BurstTracedCallback");
    return tid;
}

LinkDegradationModel::LinkDegradationModel()
    : m_ber(1e-12),
      m_baseBer(1e-12),
      m_packetLossRate(0.0),
      m_basePacketLossRate(0.0),
      m_linkState(LinkState::UP),
      m_currentTrigger(DegradationTrigger::NONE),
      m_severity(0.0),
      m_scheduledTrigger(DegradationTrigger::NONE),
      m_scheduledSeverity(0.0),
      m_totalPackets(0),
      m_droppedPackets(0),
      m_errorPackets(0),
      m_errorMode(ErrorMode::INDEPENDENT),
      m_codewordSize(256),
      m_burstLength(4),
      m_burstArrivalRate(1e-6),
      m_inBurst(false),
      m_burstRemaining(0),
      m_totalBursts(0)
{
    NS_LOG_FUNCTION(this);
    m_fallbackRandom = CreateObject<UniformRandomVariable>();
}

LinkDegradationModel::~LinkDegradationModel()
{
    NS_LOG_FUNCTION(this);
}

void
LinkDegradationModel::DoDispose()
{
    NS_LOG_FUNCTION(this);
    CancelScheduledDegradation();
    m_random = nullptr;
    Object::DoDispose();
}

void
LinkDegradationModel::SetRandomStream(Ptr<RandomVariableStream> stream)
{
    NS_LOG_FUNCTION(this << stream);
    m_random = stream;
}

void
LinkDegradationModel::SetBer(double ber)
{
    NS_LOG_FUNCTION(this << ber);

    // Validate BER: must be finite, non-negative, and less than 1.0
    if (!std::isfinite(ber) || ber < 0.0 || ber >= 1.0)
    {
        NS_FATAL_ERROR("Invalid BER value " << ber
                       << ": must be finite, >= 0.0, and < 1.0");
    }

    double oldBer = m_ber;
    m_ber = ber;
    m_baseBer = ber;
    m_linkMetadata.ber = ber;
    m_berChangeTrace(oldBer, m_ber);
}

double
LinkDegradationModel::GetBer() const
{
    return m_ber;
}

void
LinkDegradationModel::SetLinkMetadata(const LinkMetadata& meta)
{
    NS_LOG_FUNCTION(this);
    m_linkMetadata = meta;
    // Sync BER with metadata's ber field if it is set (non-zero); otherwise
    // leave the existing BER untouched so a metadata-only call is a no-op.
    if (meta.ber > 0.0)
    {
        SetBer(meta.ber);
    }
}

const LinkMetadata&
LinkDegradationModel::GetLinkMetadata() const
{
    return m_linkMetadata;
}

void
LinkDegradationModel::SetPacketLossRate(double rate)
{
    NS_LOG_FUNCTION(this << rate);

    // Validate: must be finite and in [0.0, 1.0)
    if (!std::isfinite(rate) || rate < 0.0 || rate >= 1.0)
    {
        NS_FATAL_ERROR("Invalid packet loss rate " << rate
                       << ": must be finite, >= 0.0, and < 1.0");
    }

    m_packetLossRate = rate;
    m_basePacketLossRate = rate;
}

double
LinkDegradationModel::GetPacketLossRate() const
{
    return m_packetLossRate;
}

void
LinkDegradationModel::SetLinkState(LinkState state)
{
    NS_LOG_FUNCTION(this << static_cast<int>(state));
    LinkState oldState = m_linkState;
    m_linkState = state;
    m_stateChangeTrace(oldState, m_linkState);

    if (!m_linkStateCallback.IsNull())
    {
        m_linkStateCallback(oldState, m_linkState);
    }
}

LinkState
LinkDegradationModel::GetLinkState() const
{
    return m_linkState;
}

bool
LinkDegradationModel::IsLinkUp() const
{
    return m_linkState != LinkState::DOWN;
}

void
LinkDegradationModel::SetLinkStateCallback(LinkStateCallback cb)
{
    NS_LOG_FUNCTION(this);
    m_linkStateCallback = cb;
}

void
LinkDegradationModel::SetPacketDropCallback(PacketDropCallback cb)
{
    NS_LOG_FUNCTION(this);
    m_packetDropCallback = cb;
}

void
LinkDegradationModel::SetErrorMode(ErrorMode mode)
{
    NS_LOG_FUNCTION(this << static_cast<int>(mode));
    m_errorMode = mode;
}

ErrorMode
LinkDegradationModel::GetErrorMode() const
{
    return m_errorMode;
}

void
LinkDegradationModel::SetCodewordSize(uint32_t sizeBytes)
{
    NS_LOG_FUNCTION(this << sizeBytes);
    m_codewordSize = sizeBytes;
}

uint32_t
LinkDegradationModel::GetCodewordSize() const
{
    return m_codewordSize;
}

void
LinkDegradationModel::SetBurstLength(uint32_t codewords)
{
    NS_LOG_FUNCTION(this << codewords);
    m_burstLength = codewords;
}

uint32_t
LinkDegradationModel::GetBurstLength() const
{
    return m_burstLength;
}

void
LinkDegradationModel::SetBurstArrivalRate(double rate)
{
    NS_LOG_FUNCTION(this << rate);
    m_burstArrivalRate = rate;
}

double
LinkDegradationModel::GetBurstArrivalRate() const
{
    return m_burstArrivalRate;
}

bool
LinkDegradationModel::IsInBurst() const
{
    return m_inBurst;
}

void
LinkDegradationModel::SetTrigger(DegradationTrigger trigger)
{
    NS_LOG_FUNCTION(this << static_cast<int>(trigger));
    m_currentTrigger = trigger;
}

DegradationTrigger
LinkDegradationModel::GetTrigger() const
{
    return m_currentTrigger;
}

void
LinkDegradationModel::ApplyDegradation(DegradationTrigger trigger, double severity)
{
    NS_LOG_FUNCTION(this << static_cast<int>(trigger) << severity);

    m_currentTrigger = trigger;
    m_severity = severity;

    switch (trigger)
    {
        case DegradationTrigger::BIT_ERROR:
            // Increase BER based on severity
            // Severity 1.0 = BER * 1000
            m_ber = m_baseBer * (1.0 + severity * 999.0);
            m_berChangeTrace(m_baseBer, m_ber);
            SetLinkState(LinkState::DEGRADED);
            break;

        case DegradationTrigger::LINK_FLAP:
            // Link goes down temporarily
            if (severity > 0.5)
            {
                SetLinkState(LinkState::DOWN);
            }
            else
            {
                SetLinkState(LinkState::DEGRADED);
            }
            break;

        case DegradationTrigger::CONGESTION:
            // Increase packet loss rate
            m_packetLossRate = m_basePacketLossRate + (1.0 - m_basePacketLossRate) * severity;
            SetLinkState(LinkState::DEGRADED);
            break;

        case DegradationTrigger::THERMAL:
            // Thermal throttling - both BER increase and bandwidth reduction
            {
                double oldBer = m_ber;
                m_ber = m_baseBer * (1.0 + severity * 99.0);
                m_berChangeTrace(oldBer, m_ber);
            }
            m_packetLossRate = m_basePacketLossRate + severity * 0.1;
            SetLinkState(LinkState::DEGRADED);
            break;

        case DegradationTrigger::EXTERNAL:
            // External trigger - general degradation
            {
                double oldBer = m_ber;
                m_ber = m_baseBer * (1.0 + severity * 999.0);
                m_berChangeTrace(oldBer, m_ber);
            }
            m_packetLossRate = m_basePacketLossRate + severity * 0.5;
            SetLinkState(LinkState::DEGRADED);
            break;

        default:
            ClearDegradation();
            break;
    }
}

void
LinkDegradationModel::ClearDegradation()
{
    NS_LOG_FUNCTION(this);

    // Restore to base values
    double oldBer = m_ber;
    m_ber = m_baseBer;
    if (oldBer != m_ber)
    {
        m_berChangeTrace(oldBer, m_ber);
    }

    m_packetLossRate = m_basePacketLossRate;
    m_currentTrigger = DegradationTrigger::NONE;
    m_severity = 0.0;
    SetLinkState(LinkState::UP);
}

double
LinkDegradationModel::GetRandomValue()
{
    if (m_random)
    {
        return m_random->GetValue();
    }
    if (!m_fallbackRandom)
    {
        m_fallbackRandom = CreateObject<UniformRandomVariable>();
    }
    return m_fallbackRandom->GetValue();
}

bool
LinkDegradationModel::ProcessPacket(Ptr<Packet> packet)
{
    NS_LOG_FUNCTION(this << packet);

    m_totalPackets++;

    // Check if link is down
    if (m_linkState == LinkState::DOWN)
    {
        m_droppedPackets++;
        m_packetDropTrace(packet->GetSize(), m_currentTrigger);
        if (!m_packetDropCallback.IsNull())
        {
            m_packetDropCallback(packet, m_currentTrigger);
        }
        return false;
    }

    // Check packet loss rate
    if (m_packetLossRate > 0)
    {
        double lossSample = GetRandomValue();
        if (lossSample < m_packetLossRate)
        {
            NS_LOG_DEBUG("Packet dropped by loss rate: " << m_packetLossRate);
            m_droppedPackets++;
            m_packetDropTrace(packet->GetSize(), m_currentTrigger);
            if (!m_packetDropCallback.IsNull())
            {
                m_packetDropCallback(packet, m_currentTrigger);
            }
            return false;
        }
    }

    // Check bit errors
    if (m_ber > 0)
    {
        if (m_errorMode == ErrorMode::BURST)
        {
            return ProcessPacketBurstMode(packet);
        }

        double errorProb = CalculatePacketErrorProbability(packet->GetSize());
        double errorSample = GetRandomValue();
        if (errorSample < errorProb)
        {
            NS_LOG_DEBUG("Packet dropped by BER: " << m_ber << " errorProb: " << errorProb);
            m_errorPackets++;
            m_droppedPackets++;
            m_packetDropTrace(packet->GetSize(), m_currentTrigger);
            if (!m_packetDropCallback.IsNull())
            {
                m_packetDropCallback(packet, m_currentTrigger);
            }
            return false;
        }
    }

    return true;
}

void
LinkDegradationModel::ScheduleDegradation(Time delay, DegradationTrigger trigger,
                                           double severity, Time duration)
{
    NS_LOG_FUNCTION(this << delay << static_cast<int>(trigger) << severity << duration);

    CancelScheduledDegradation();

    // Store parameters for the degradation event
    m_scheduledTrigger = trigger;
    m_scheduledSeverity = severity;

    // Schedule degradation
    m_degradationEvent = Simulator::Schedule(delay,
                                              &LinkDegradationModel::OnDegradationEvent,
                                              this);

    // Schedule recovery if duration is specified
    if (duration > Time(0))
    {
        m_recoveryEvent = Simulator::Schedule(delay + duration,
                                              &LinkDegradationModel::OnRecoveryEvent,
                                              this);
    }
}

void
LinkDegradationModel::CancelScheduledDegradation()
{
    NS_LOG_FUNCTION(this);

    if (m_degradationEvent.IsPending())
    {
        m_degradationEvent.Cancel();
    }
    if (m_recoveryEvent.IsPending())
    {
        m_recoveryEvent.Cancel();
    }
}

double
LinkDegradationModel::CalculatePacketErrorProbability(uint32_t packetSize) const
{
    // P(error) = 1 - (1 - BER)^(bits)
    // For small BER, approximately P(error) ≈ BER * bits
    uint64_t bits = static_cast<uint64_t>(packetSize) * 8;
    double probNoError = std::pow(1.0 - m_ber, bits);
    return 1.0 - probNoError;
}

void
LinkDegradationModel::OnDegradationEvent()
{
    NS_LOG_FUNCTION(this);
    ApplyDegradation(m_scheduledTrigger, m_scheduledSeverity);
}

void
LinkDegradationModel::OnRecoveryEvent()
{
    NS_LOG_FUNCTION(this);
    ClearDegradation();
}

bool
LinkDegradationModel::ProcessPacketBurstMode(Ptr<Packet> packet)
{
    NS_LOG_FUNCTION(this << packet);

    uint32_t packetSize = packet->GetSize();
    uint32_t numCodewords = (packetSize + m_codewordSize - 1) / m_codewordSize;

    for (uint32_t i = 0; i < numCodewords; i++)
    {
        if (m_inBurst)
        {
            // Currently in a burst: this codeword always fails
            m_burstRemaining--;
            if (m_burstRemaining == 0)
            {
                m_inBurst = false;
            }

            NS_LOG_DEBUG("Codeword " << i << " dropped in burst (remaining=" << m_burstRemaining << ")");
            m_errorPackets++;
            m_droppedPackets++;
            m_packetDropTrace(packet->GetSize(), m_currentTrigger);
            if (!m_packetDropCallback.IsNull())
            {
                m_packetDropCallback(packet, m_currentTrigger);
            }
            return false;
        }
        else
        {
            // Not in burst: check if a new burst starts
            // Also apply independent BER for codewords outside bursts
            double codewordBits = static_cast<double>(m_codewordSize) * 8.0;
            double codewordErrorProb = 1.0 - std::pow(1.0 - m_ber, codewordBits);

            if (ShouldStartBurst())
            {
                // Start a new burst
                m_inBurst = true;
                // Geometric distribution for burst length with mean m_burstLength
                double u = GetRandomValue();
                if (u < 1.0 / (1.0 + static_cast<double>(m_burstLength)))
                {
                    m_burstRemaining = 1;
                }
                else
                {
                    m_burstRemaining = static_cast<uint32_t>(
                        std::log(1.0 - u) / std::log(static_cast<double>(m_burstLength) /
                                                      (1.0 + static_cast<double>(m_burstLength)))
                    ) + 1;
                    if (m_burstRemaining < 1) m_burstRemaining = 1;
                    if (m_burstRemaining > 1000) m_burstRemaining = 1000;
                }
                m_totalBursts++;
                m_burstTrace(m_burstRemaining, m_burstRemaining);

                NS_LOG_DEBUG("Burst started at codeword " << i << " length=" << m_burstRemaining);

                // This codeword is part of the burst
                m_burstRemaining--;
                if (m_burstRemaining == 0)
                {
                    m_inBurst = false;
                }

                m_errorPackets++;
                m_droppedPackets++;
                m_packetDropTrace(packet->GetSize(), m_currentTrigger);
                if (!m_packetDropCallback.IsNull())
                {
                    m_packetDropCallback(packet, m_currentTrigger);
                }
                return false;
            }
            else if (GetRandomValue() < codewordErrorProb)
            {
                // Independent BER error on this codeword (outside burst)
                NS_LOG_DEBUG("Codeword " << i << " dropped by independent BER");
                m_errorPackets++;
                m_droppedPackets++;
                m_packetDropTrace(packet->GetSize(), m_currentTrigger);
                if (!m_packetDropCallback.IsNull())
                {
                    m_packetDropCallback(packet, m_currentTrigger);
                }
                return false;
            }
        }
    }

    return true;
}

bool
LinkDegradationModel::ShouldStartBurst()
{
    if (m_burstArrivalRate <= 0.0)
    {
        return false;
    }
    return GetRandomValue() < m_burstArrivalRate;
}

} // namespace ns3
