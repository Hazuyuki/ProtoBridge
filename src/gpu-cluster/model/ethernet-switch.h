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
 * packets bypass, matching the BER bypass). PFC priority flow control
 * remains deferred (R-SoH-2); DCQCN rate backoff is implemented in
 * DcqcnRateMachine and wired into FabricEndpoint (only the multi-flow incast
 * e2e proof, R-SoH-3, is deferred).
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

    // NvSwitch congestion hook. For RoCE data packets this marks the SoHeader
    // ECN bit (lossless) when voqDepth crosses the ECN threshold; for plain
    // Ethernet data packets it tail-drops past the VoqDropThreshold and emits a
    // NACK back to the source (lossy, EmitNackBack). Control/retry packets
    // bypass (matching the BER bypass). Returns false to drop (caller counts a
    // VOQ drop), true to admit.
    //
    // Exposed publicly (overriding the protected NvSwitch base) so the
    // lossy-drop + NACK decision is unit-testable without driving the full
    // event-driven datapath -- mirroring MarkEcnIfCongested above.
    bool OnPacketEnqueue(Ptr<Packet> packet, uint32_t outPort,
                         uint32_t inPort, size_t voqDepth) override;

    /// Data packets whose SoHeader ECN bit this switch marked (observability).
    uint64_t GetEcnMarkedPackets() const { return m_ecnMarkedPackets; }

  private:
    uint32_t m_ecnThreshold;     ///< VOQ depth (packets) at which ECN is marked
    uint32_t m_voqDropThreshold; ///< VOQ depth (packets) at which a plain-Ethernet
                                 ///< data packet is tail-dropped with a NACK
                                 ///< (lossy); must be < NvSwitch::VoqDepth
    uint64_t m_ecnMarkedPackets{0}; ///< count of ECN-marked data packets
};

} // namespace ns3

#endif // ETHERNET_SWITCH_H
