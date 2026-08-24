/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ethernet-switch.h
 *
 * Ethernet (RoCE v2) switch for the scale-out (inter-node) fabric. A thin
 * NvSwitch derivative: reuses the L2 MAC-learning + VOQ + crossbar-arbiter +
 * cut-through datapath but carries no NVLink in-network compute (SHARP
 * all-reduce / NVLS all-gather are scale-up concepts with no Ethernet
 * equivalent; they default off on NvSwitch). RoCEv2 ECN congestion marking is
 * layered on here: on enqueue, when a VOQ crosses the ECN threshold, the
 * switch marks the inner SoHeader ECN bit of RoCE data packets (control
 * packets bypass, matching the BER bypass). PFC priority flow control +
 * DCQCN rate backoff remain separable future work (see CHANGES.md WU-SoH).
 */
#ifndef ETHERNET_SWITCH_H
#define ETHERNET_SWITCH_H

#include "nvswitch.h"

#include <cstddef>
#include <cstdint>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Ethernet (RoCEv2) switch for the scale-out fabric.
 *
 * Distinct TypeId ("ns3::EthernetSwitch") so the topology helpers can select
 * the So switch via NvSwitchHelper::SetSwitchType and the simulator can
 * distinguish an Su NVSwitch from an So Ethernet switch.
 */
class EthernetSwitch : public NvSwitch
{
  public:
    static TypeId GetTypeId();

    EthernetSwitch();
    ~EthernetSwitch() override = default;

    std::string GetVendorName() const override;

    /**
     * Mark the SoHeader ECN bit on a RoCE data packet when voqDepth >= the ECN
     * threshold. Control packets and non-RoCE packets are left untouched (they
     * carry no inner SoHeader). Returns true iff the packet was marked.
     *
     * Exposed publicly so the ECN-marking logic is unit-testable without
     * driving the full event-driven datapath.
     */
    bool MarkEcnIfCongested(Ptr<Packet> packet, size_t voqDepth) const;

  protected:
    // NvSwitch congestion hook: mark the SoHeader ECN bit on enqueue when the
    // VOQ depth crosses the ECN threshold.
    void OnPacketEnqueue(Ptr<Packet> packet, uint32_t outPort,
                         size_t voqDepth) override;

  private:
    uint32_t m_ecnThreshold; ///< VOQ depth (packets) at which ECN is marked
};

} // namespace ns3

#endif // ETHERNET_SWITCH_H
