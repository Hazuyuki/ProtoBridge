/*
 * SPDX-License-Identifier: GPL-2.0-only
 * fabric-switch.h
 *
 * Abstract base class for vendor-specific fabric switches.
 * All switch implementations (NvSwitch, AMD Infinity Fabric Switch, etc.)
 * must derive from this class and implement the core interface.
 */

#ifndef FABRIC_SWITCH_H
#define FABRIC_SWITCH_H

#include "ns3/net-device.h"
#include "ns3/mac48-address.h"

#include <cstdint>
#include <string>

namespace ns3
{

class FabricSwitch : public NetDevice
{
public:
    static TypeId GetTypeId();

    FabricSwitch();
    virtual ~FabricSwitch();

    /// Add a port (NetDevice) to the switch
    virtual uint32_t AddPort(Ptr<NetDevice> device) = 0;

    /// Get number of ports
    virtual uint32_t GetNPorts() const = 0;

    /// Get a specific port
    virtual Ptr<NetDevice> GetPort(uint32_t index) const = 0;

    /// Add a static MAC-to-port route
    virtual void AddStaticRoute(Mac48Address addr, uint32_t port) = 0;

    /// Identify the vendor this switch represents
    virtual std::string GetVendorName() const = 0;
};

} // namespace ns3

#endif // FABRIC_SWITCH_H