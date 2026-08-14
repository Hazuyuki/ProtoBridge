/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Point-to-Point Traffic Generator
 */

#ifndef P2P_TRAFFIC_H
#define P2P_TRAFFIC_H

#include "traffic-pattern.h"

#include "ns3/event-id.h"
#include "ns3/nstime.h"

#include <cstdint>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Point-to-point traffic generator
 *
 * Generates P2P traffic from a source endpoint to a destination rank.
 * Supports configurable packet size, interval, count, and burst mode.
 */
class P2pTraffic : public TrafficPattern
{
  public:
    static TypeId GetTypeId();

    P2pTraffic();
    ~P2pTraffic() override;

    void SetDestRank(uint16_t rank);
    uint16_t GetDestRank() const;

    void SetPacketSize(uint32_t size);
    uint32_t GetPacketSize() const;

    void SetInterval(Time interval);
    Time GetInterval() const;

    void SetMaxPackets(uint32_t count);
    uint32_t GetMaxPackets() const;

    void SetFlowId(uint16_t flowId);
    uint16_t GetFlowId() const;

    void SetVcId(uint8_t vcId);
    uint8_t GetVcId() const;

    void SetBurstSize(uint32_t burstSize);
    uint32_t GetBurstSize() const;

    uint32_t GetPacketsSent() const;
    uint64_t GetBytesSent() const;

  protected:
    void DoStart() override;
    void DoStop() override;

  private:
    void SendBurst();
    void ScheduleNext();

    uint16_t m_destRank;
    uint32_t m_packetSize;
    Time m_interval;
    uint32_t m_maxPackets;
    uint16_t m_flowId;
    uint8_t m_vcId;
    uint32_t m_burstSize;

    uint32_t m_packetsSent;
    uint64_t m_bytesSent;
    EventId m_sendEvent;
};

} // namespace ns3

#endif /* P2P_TRAFFIC_H */
