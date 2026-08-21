/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ethernet-switch.h
 *
 * Ethernet (RoCE v2) switch for the scale-out (inter-node) fabric. A thin
 * NvSwitch derivative: reuses the L2 MAC-learning + VOQ + crossbar-arbiter +
 * cut-through datapath but carries no NVLink in-network compute (SHARP
 * all-reduce / NVLS all-gather are scale-up concepts with no Ethernet
 * equivalent; they default off on NvSwitch). RoCEv2 lossless behavior
 * (PFC priority flow control + ECN/DCQCN rate backoff) is layered on by the
 * congestion-control subsystem in a later work unit.
 */
#ifndef ETHERNET_SWITCH_H
#define ETHERNET_SWITCH_H

#include "nvswitch.h"

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

    // NvSwitch defaults SHARP/NVLS off; the So fabric has no in-network compute.
    EthernetSwitch() = default;
    ~EthernetSwitch() override = default;

    std::string GetVendorName() const override;
};

} // namespace ns3

#endif // ETHERNET_SWITCH_H
