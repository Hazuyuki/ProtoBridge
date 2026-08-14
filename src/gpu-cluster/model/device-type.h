/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Device Type enumeration for heterogeneous fabric endpoints
 */

#ifndef DEVICE_TYPE_H
#define DEVICE_TYPE_H

#include <cstdint>
#include <string>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Device type enumeration for fabric endpoints
 */
enum class DeviceType : uint8_t
{
    GPU = 0,    ///< GPU compute endpoint (NVIDIA, AMD)
    CPU = 1,    ///< CPU endpoint
    TPU = 2,    ///< TPU accelerator endpoint (Google)
    MEMORY = 3, ///< Memory endpoint (e.g., HBM, HMC)
    NPU = 4,    ///< NPU accelerator endpoint (Huawei Ascend)
    GAUDI = 5,    ///< Gaudi accelerator endpoint (Intel)
    UB_NPU = 6   ///< UB NPU endpoint (Huawei Ascend-UB)
};

inline const char* DeviceTypeToString(DeviceType type)
{
    switch (type)
    {
        case DeviceType::GPU:    return "GPU";
        case DeviceType::CPU:    return "CPU";
        case DeviceType::TPU:    return "TPU";
        case DeviceType::MEMORY: return "Memory";
        case DeviceType::NPU:    return "NPU";
        case DeviceType::GAUDI:  return "Gaudi";
        case DeviceType::UB_NPU: return "UB_NPU";
        default:                 return "Unknown";
    }
}

inline DeviceType DeviceTypeFromString(const std::string& str)
{
    if (str == "GPU" || str == "gpu")    return DeviceType::GPU;
    if (str == "CPU" || str == "cpu")    return DeviceType::CPU;
    if (str == "TPU" || str == "tpu")    return DeviceType::TPU;
    if (str == "Memory" || str == "mem") return DeviceType::MEMORY;
    if (str == "NPU" || str == "npu")    return DeviceType::NPU;
    if (str == "Gaudi" || str == "gaudi") return DeviceType::GAUDI;
    if (str == "UB_NPU" || str == "ub_npu") return DeviceType::UB_NPU;
    return DeviceType::GPU;
};

} // namespace ns3

#endif /* DEVICE_TYPE_H */
