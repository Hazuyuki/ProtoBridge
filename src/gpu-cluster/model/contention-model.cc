/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 */

#include "contention-model.h"
#include "fabric-header.h"

#include "ns3/log.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ContentionModel");

NS_OBJECT_ENSURE_REGISTERED(ContentionModel);

TypeId
ContentionModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::ContentionModel")
        .SetParent<Object>()
        .SetGroupName("GpuCluster")
        .AddConstructor<ContentionModel>()
        .AddAttribute("Bandwidth",
                      "Total available bandwidth in bytes per second",
                      UintegerValue(0),
                      MakeUintegerAccessor(&ContentionModel::m_bandwidth),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("CollectiveWeight",
                      "Bandwidth weight for collective traffic",
                      DoubleValue(0.7),
                      MakeDoubleAccessor(&ContentionModel::SetCollectiveWeight,
                                         &ContentionModel::GetCollectiveWeight),
                      MakeDoubleChecker<double>(0.0, 1.0))
        .AddAttribute("MemoryWeight",
                      "Bandwidth weight for memory traffic",
                      DoubleValue(0.2),
                      MakeDoubleAccessor(&ContentionModel::SetMemoryWeight,
                                         &ContentionModel::GetMemoryWeight),
                      MakeDoubleChecker<double>(0.0, 1.0))
        .AddAttribute("P2pWeight",
                      "Bandwidth weight for P2P traffic",
                      DoubleValue(0.1),
                      MakeDoubleAccessor(&ContentionModel::SetP2pWeight,
                                         &ContentionModel::GetP2pWeight),
                      MakeDoubleChecker<double>(0.0, 1.0))
        .AddTraceSource("BandwidthShare",
                        "Bandwidth share allocation event",
                        MakeTraceSourceAccessor(&ContentionModel::m_bandwidthShareTrace),
                        "ns3::ContentionModel::BandwidthShareTracedCallback");
    return tid;
}

ContentionModel::ContentionModel()
    : m_bandwidth(0)
{
    m_weights[0] = 0.7;  // Collective
    m_weights[1] = 0.2;  // Memory
    m_weights[2] = 0.1;  // P2P

    for (int i = 0; i < static_cast<int>(TrafficClass::NUM_CLASSES); i++)
    {
        m_backlog[i] = 0;
    }
}

ContentionModel::~ContentionModel()
{
}

void
ContentionModel::DoDispose()
{
    Object::DoDispose();
}

void
ContentionModel::SetCollectiveWeight(double w) { m_weights[0] = w; }
void
ContentionModel::SetMemoryWeight(double w) { m_weights[1] = w; }
void
ContentionModel::SetP2pWeight(double w) { m_weights[2] = w; }
double
ContentionModel::GetCollectiveWeight() const { return m_weights[0]; }
double
ContentionModel::GetMemoryWeight() const { return m_weights[1]; }
double
ContentionModel::GetP2pWeight() const { return m_weights[2]; }

void
ContentionModel::SetWeight(TrafficClass trafficClass, double weight)
{
    NS_LOG_FUNCTION(this << static_cast<int>(trafficClass) << weight);
    int idx = static_cast<int>(trafficClass);
    if (idx >= 0 && idx < static_cast<int>(TrafficClass::NUM_CLASSES))
    {
        m_weights[idx] = weight;
    }
}

double
ContentionModel::GetWeight(TrafficClass trafficClass) const
{
    int idx = static_cast<int>(trafficClass);
    if (idx >= 0 && idx < static_cast<int>(TrafficClass::NUM_CLASSES))
    {
        return m_weights[idx];
    }
    return 0.0;
}

void
ContentionModel::SetBandwidth(uint64_t bytesPerSecond)
{
    m_bandwidth = bytesPerSecond;
}

uint64_t
ContentionModel::GetBandwidth() const
{
    return m_bandwidth;
}

TrafficClass
ContentionModel::ClassifyPacket(uint8_t packetType)
{
    FabricPacketType type = static_cast<FabricPacketType>(packetType);
    if (type == FabricPacketType::MEMORY_READ ||
        type == FabricPacketType::MEMORY_WRITE ||
        type == FabricPacketType::MEMORY_RESP)
    {
        return TrafficClass::MEMORY;
    }
    if (type == FabricPacketType::P2P)
    {
        return TrafficClass::P2P;
    }
    // All collective types (ALLREDUCE, ALLGATHER, ALLTOALL, etc.) and DATA
    return TrafficClass::COLLECTIVE;
}

Time
ContentionModel::ComputeServiceTime(uint32_t size, TrafficClass trafficClass) const
{
    if (m_bandwidth == 0 || size == 0)
    {
        return Time(0);
    }

    uint32_t numActive = GetNumActiveClasses();
    if (numActive <= 1)
    {
        return Time(0);
    }

    // Incremental WFQ penalty: size/(weight*BW) - size/BW
    // The NetDevice already serializes at full BW; this schedules
    // the additional delay from WFQ bandwidth reduction only.
    double weight = GetWeight(trafficClass);
    if (weight <= 0)
    {
        return Time(0);
    }

    double fullBwTime = static_cast<double>(size) / static_cast<double>(m_bandwidth);
    double wfqBwTime = static_cast<double>(size) / (weight * static_cast<double>(m_bandwidth));
    double penalty = wfqBwTime - fullBwTime;
    return Seconds(penalty);
}

void
ContentionModel::IncrementBacklog(TrafficClass trafficClass)
{
    int idx = static_cast<int>(trafficClass);
    if (idx >= 0 && idx < static_cast<int>(TrafficClass::NUM_CLASSES))
    {
        m_backlog[idx]++;
    }
}

void
ContentionModel::DecrementBacklog(TrafficClass trafficClass)
{
    int idx = static_cast<int>(trafficClass);
    if (idx >= 0 && idx < static_cast<int>(TrafficClass::NUM_CLASSES))
    {
        if (m_backlog[idx] > 0)
        {
            m_backlog[idx]--;
        }
    }
}

uint32_t
ContentionModel::GetBacklog(TrafficClass trafficClass) const
{
    int idx = static_cast<int>(trafficClass);
    if (idx >= 0 && idx < static_cast<int>(TrafficClass::NUM_CLASSES))
    {
        return m_backlog[idx];
    }
    return 0;
}

bool
ContentionModel::IsClassActive(TrafficClass trafficClass) const
{
    int idx = static_cast<int>(trafficClass);
    if (idx >= 0 && idx < static_cast<int>(TrafficClass::NUM_CLASSES))
    {
        return m_backlog[idx] > 0;
    }
    return false;
}

uint32_t
ContentionModel::GetNumActiveClasses() const
{
    uint32_t count = 0;
    for (int i = 0; i < static_cast<int>(TrafficClass::NUM_CLASSES); i++)
    {
        if (m_backlog[i] > 0) count++;
    }
    return count;
}

uint64_t
ContentionModel::GetEffectiveBandwidth(TrafficClass trafficClass) const
{
    if (m_bandwidth == 0) return 0;

    uint32_t numActive = GetNumActiveClasses();
    if (numActive == 0) return m_bandwidth;

    // If this class is the only active one, it gets full bandwidth
    if (numActive == 1 && IsClassActive(trafficClass))
    {
        return m_bandwidth;
    }

    // Otherwise, get weighted share
    double weight = GetWeight(trafficClass);
    return static_cast<uint64_t>(weight * static_cast<double>(m_bandwidth));
}

Time
ContentionModel::GetSerializationTime(uint32_t size) const
{
    if (m_bandwidth == 0 || size == 0)
    {
        return Time(0);
    }
    return Seconds(static_cast<double>(size) / static_cast<double>(m_bandwidth));
}

} // namespace ns3
