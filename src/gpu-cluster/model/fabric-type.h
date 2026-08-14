/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Fabric type enumeration for hybrid scale-up/scale-out architecture
 */

#ifndef FABRIC_TYPE_H
#define FABRIC_TYPE_H

#include <cstdint>
#include <string>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Fabric type enumeration for multi-fabric GPU clusters
 *
 * This enum identifies the type of network fabric:
 * - NVLINK: Scale-up fabric within a node (high bandwidth, low latency)
 * - ETHERNET: Scale-out fabric between nodes (standard bandwidth, higher latency)
 * - HYBRID: Indicates support for both fabric types (used for gateway endpoints)
 */
enum class FabricType : uint8_t
{
    NVLINK = 0,     ///< NVLink fabric (NVIDIA scale-up): ~300 GB/s, ~500ns
    ETHERNET = 1,   ///< Ethernet fabric (scale-out): ~100 Gbps, ~10µs
    HYBRID = 2,     ///< Hybrid fabric (both NVLink and Ethernet, for gateways)
    ICI = 3,        ///< ICI fabric (Google TPU): ~100-200 GB/s/dim, ~100ns
    HCCS = 4,       ///< HCCS fabric (Huawei Ascend): ~49 GB/s/link, ~200ns
    XGMI = 5,       ///< xGMI/Infinity Fabric (AMD MI300X): ~36 GB/s/link, ~150ns
    ROCE = 6,       ///< RoCE fabric (Intel Gaudi): ~25 Gbps/NIC, ~800ns
    UB = 7,         ///< UB fabric (Huawei SuperPod): ~56 GB/s/link, ~100ns
    METAXLINK = 8   ///< MetaXLink scale-up fabric
};

inline const char* FabricTypeToString(FabricType type)
{
    switch (type)
    {
        case FabricType::NVLINK:   return "NVLink";
        case FabricType::ETHERNET: return "Ethernet";
        case FabricType::HYBRID:   return "Hybrid";
        case FabricType::ICI:      return "ICI";
        case FabricType::HCCS:     return "HCCS";
        case FabricType::XGMI:     return "xGMI";
        case FabricType::ROCE:     return "RoCE";
        case FabricType::UB:       return "UB";
        case FabricType::METAXLINK:return "MetaXLink";
        default:                   return "Unknown";
    }
}

inline FabricType FabricTypeFromString(const std::string& str)
{
    if (str == "NVLink" || str == "nvlink")   return FabricType::NVLINK;
    if (str == "Ethernet" || str == "eth")    return FabricType::ETHERNET;
    if (str == "Hybrid" || str == "hybrid")   return FabricType::HYBRID;
    if (str == "ICI" || str == "ici")         return FabricType::ICI;
    if (str == "HCCS" || str == "hccs")       return FabricType::HCCS;
    if (str == "xGMI" || str == "xgmi")       return FabricType::XGMI;
    if (str == "RoCE" || str == "roce")       return FabricType::ROCE;
    if (str == "UB" || str == "ub")         return FabricType::UB;
    if (str == "MetaXLink" || str == "metaxlink") return FabricType::METAXLINK;
    return FabricType::NVLINK;
}

} // namespace ns3

#endif /* FABRIC_TYPE_H */
