/*
 * SPDX-License-Identifier: GPL-2.0-only
 * collective-injector.h
 *
 * Abstract base class for collective operation injectors.
 * Each collective algorithm (RingAllReduce, TreeAllReduce, etc.)
 * derives from this and implements the pipelined packet-level logic.
 */

#ifndef COLLECTIVE_INJECTOR_H
#define COLLECTIVE_INJECTOR_H

#include "fabric-endpoint.h"
#include "ns3/core-module.h"

#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace ns3
{

/**
 * @brief Collective operation type (used by all collective injectors).
 *
 * Formerly defined in workload-injector.h; relocated here so that the
 * end-to-end LLM-serving layer can be removed without touching the 16
 * collective injectors that override GetCollectiveType().
 */
enum class CollectiveType : uint8_t
{
    ALLREDUCE = 0,
    ALLGATHER = 1,
    ALLTOALL = 2,
    REDUCESCATTER = 3,
    BROADCAST = 4,
    REDUCE = 5,
    P2P = 6
};

class CollectiveInjector : public Object
{
public:
    static TypeId GetTypeId();

    CollectiveInjector();
    virtual ~CollectiveInjector();

    virtual void Initialize(uint16_t numGpus, uint64_t dataSize,
                            const std::vector<Ptr<FabricEndpoint>>& endpoints) = 0;

    virtual void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) = 0;

    virtual void Start() = 0;

    virtual void SetStartupDelay(Time delay) = 0;

    virtual CollectiveType GetCollectiveType() const = 0;

    virtual bool IsCompleted() const = 0;

    // Per-step trace instrumentation: when set, every step completion is
    // logged as a CSV row: gpu,step,collectiveStartNs,completeNs,sinceStartNs
    // (sinceStartNs = completeNs - collectiveStartNs, cumulative since collective
    // start, NOT per-step delay; per-step delay = diff of consecutive sinceStartNs)
    static void SetStepTraceFile(const std::string& path);
    static void LogStepComplete(uint16_t gpu, uint32_t step,
                                uint64_t startNs, uint64_t completeNs);

private:
    static std::string g_stepTraceFile;
    static std::ofstream g_stepTraceStream;
};

} // namespace ns3

#endif // COLLECTIVE_INJECTOR_H