/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Contention Model: weighted fair queuing between traffic classes
 */

#ifndef CONTENTION_MODEL_H
#define CONTENTION_MODEL_H

#include "ns3/object.h"
#include "ns3/nstime.h"
#include "ns3/traced-callback.h"
#include "ns3/event-id.h"

#include <queue>
#include <unordered_map>
#include <cstdint>

namespace ns3
{

class Packet;

/**
 * @ingroup gpu-cluster
 * @brief Traffic class for contention scheduling
 */
enum class TrafficClass : uint8_t
{
    COLLECTIVE = 0,   ///< Collective communication (AllReduce, AllGather, etc.)
    MEMORY = 1,       ///< Memory access traffic (KV cache reads/writes)
    P2P = 2,          ///< Point-to-point traffic
    NUM_CLASSES = 3   ///< Number of traffic classes
};

/**
 * @ingroup gpu-cluster
 * @brief Contention Model
 *
 * Implements weighted fair queuing (WFQ) between traffic classes at
 * NIC injection and switch output ports. Each class has a configurable
 * weight that determines its share of available bandwidth.
 *
 * When multiple classes have pending traffic, the model schedules
 * transmissions according to their weighted fair share, preventing
 * any single class from monopolizing the link.
 */
class ContentionModel : public Object
{
  public:
    static TypeId GetTypeId();

    ContentionModel();
    ~ContentionModel() override;

    /**
     * @brief Set the weight for a traffic class
     * @param trafficClass Traffic class
     * @param weight Weight (0.0 to 1.0, all weights should sum to 1.0)
     */
    void SetCollectiveWeight(double w);
    void SetMemoryWeight(double w);
    void SetP2pWeight(double w);
    double GetCollectiveWeight() const;
    double GetMemoryWeight() const;
    double GetP2pWeight() const;

    void SetWeight(TrafficClass trafficClass, double weight);

    /**
     * @brief Get the weight for a traffic class
     */
    double GetWeight(TrafficClass trafficClass) const;

    /**
     * @brief Set total available bandwidth in bytes per second
     */
    void SetBandwidth(uint64_t bytesPerSecond);
    uint64_t GetBandwidth() const;

    /**
     * @brief Classify a packet into a traffic class based on its type
     * @param packetType FabricPacketType value
     * @return Traffic class
     */
    static TrafficClass ClassifyPacket(uint8_t packetType);

    /**
     * @brief Compute the service time for a packet of given size
     * Accounts for WFQ bandwidth sharing across active classes.
     * @param size Packet size in bytes
     * @param trafficClass Traffic class of the packet
     * @return Service time (transmission delay)
     *
     * If only one class is active, it gets full bandwidth.
     * If multiple classes are active, each gets its weighted share.
     */
    Time ComputeServiceTime(uint32_t size, TrafficClass trafficClass) const;

    /**
     * @brief Increment backlog counter for a traffic class
     * A class is active when its backlog > 0.
     */
    void IncrementBacklog(TrafficClass trafficClass);

    /**
     * @brief Decrement backlog counter for a traffic class
     * A class becomes idle when its backlog drops to 0.
     */
    void DecrementBacklog(TrafficClass trafficClass);

    /**
     * @brief Get backlog count for a traffic class
     */
    uint32_t GetBacklog(TrafficClass trafficClass) const;

    /**
     * @brief Check if a traffic class is currently active
     */
    bool IsClassActive(TrafficClass trafficClass) const;

    /**
     * @brief Get the number of active traffic classes
     */
    uint32_t GetNumActiveClasses() const;

    /**
     * @brief Get the serialization time for a packet of given size at full bandwidth
     * Used to determine when a packet finishes transmission for backlog accounting.
     * @param size Packet size in bytes
     * @return Serialization time at full link bandwidth
     */
    Time GetSerializationTime(uint32_t size) const;

    /**
     * @brief Get the effective bandwidth for a given traffic class
     * If the class is the only active one, it gets full bandwidth.
     * If multiple classes are active, each gets weight * total bandwidth.
     */
    uint64_t GetEffectiveBandwidth(TrafficClass trafficClass) const;

    // Traced callbacks
    typedef TracedCallback<uint8_t, uint32_t, uint64_t> BandwidthShareTracedCallback;

  private:
    void DoDispose() override;

    double m_weights[static_cast<int>(TrafficClass::NUM_CLASSES)]; ///< Per-class weights
    uint64_t m_bandwidth;         ///< Total bandwidth in bytes per second
    uint32_t m_backlog[static_cast<int>(TrafficClass::NUM_CLASSES)]; ///< Per-class backlog counters

    TracedCallback<uint8_t, uint32_t, uint64_t> m_bandwidthShareTrace; ///< class, numActive, effectiveBW
};

} // namespace ns3

#endif /* CONTENTION_MODEL_H */
