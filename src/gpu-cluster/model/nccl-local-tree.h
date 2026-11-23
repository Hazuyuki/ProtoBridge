// SPDX-License-Identifier: GPL-2.0-only
#ifndef NCCL_LOCAL_TREE_H
#define NCCL_LOCAL_TREE_H

#include "collective-injector.h"

#include <array>

namespace ns3
{

/// Single-host NCCL SIMPLE Tree AllReduce on replicated local rank chains.
class NcclLocalTree : public CollectiveInjector
{
  public:
    static TypeId GetTypeId();
    // NCCL 2.27.7 device.h / init.cc and the duplicated single-graph fallback.
    static constexpr uint32_t MaxChannels = 64;
    static constexpr uint64_t DefaultSimpleBuffer = 4ULL << 20;

    void Configure(uint32_t channelLimit,
                   uint64_t simpleBuffer,
                   Time hopOverhead,
                   uint64_t overheadNsPerByte);
    void Initialize(uint16_t ranks,
                    uint64_t bytes,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;

    void SetCompletionCallback(std::function<void(uint64_t)> cb) override
    {
        m_complete = cb;
    }

    void SetStartupDelay(Time delay) override;
    void Start() override;

    bool IsCompleted() const override
    {
        return m_done;
    }

    CollectiveType GetCollectiveType() const override
    {
        return CollectiveType::ALLREDUCE;
    }

    std::optional<uint8_t> GetWireProtocolId() const override
    {
        return m_protocol;
    }

    std::optional<uint64_t> GetStartupDelayNs() const override
    {
        return m_startupNs;
    }

    uint32_t GetChannelLimit() const
    {
        return m_channelLimit;
    }

    uint64_t GetSimpleBuffer() const
    {
        return m_simpleBuffer;
    }

    const std::vector<uint64_t>& GetChannelBytes() const
    {
        return m_channelBytes;
    }

    uint64_t GetChunkBytes() const
    {
        // NCCL enqueue.cc aligns a SIMPLE FIFO step down to whole protocol grains.
        return m_simpleBuffer / Steps / 512 * 512;
    }

    const std::vector<uint64_t>& GetDeliveredBytes() const
    {
        return m_delivered;
    }

    const std::vector<uint16_t>& GetRankOrder() const
    {
        return m_order;
    }

  private:
    static constexpr size_t Steps = 8; // NCCL_STEPS FIFO slots per connection.

    struct Stage
    {
        uint64_t issued = 0;
        uint64_t completed = 0;
        std::array<uint64_t, Steps> received{};
    };

    // Equal channel ranges on the same local chain have the same readiness
    // under equal link sharing. Distinct tails progress as separate flows.
    struct ChannelGroup
    {
        uint64_t bytes;
        uint64_t chunk;
        uint64_t iterations;
        uint32_t channels;
        std::vector<Stage> stages;
        uint64_t BytesAt(uint64_t iteration) const;
    };

    void DoDispose() override;
    void BuildChannelGroups(uint64_t bytes);
    void Launch();
    void Pump(size_t group, size_t hop);
    void Send(uint16_t source,
              uint16_t dest,
              uint16_t flow,
              uint64_t channelBytes,
              uint32_t channels);
    void Receive(uint16_t dest, Ptr<Packet> packet, FabricHeader header);
    void Finish();

    uint32_t m_channelLimit = 2;
    uint64_t m_simpleBuffer = DefaultSimpleBuffer;
    uint64_t m_hopNs = 0;
    uint64_t m_byteNs = 0;
    uint8_t m_protocol = 0;
    uint64_t m_startupNs = 0;
    uint64_t m_startNs = 0;
    std::optional<uint64_t> m_startupOverride;
    std::vector<uint32_t> m_groupsDone; // per directed hop
    std::vector<uint64_t> m_delivered;
    std::vector<std::optional<uint64_t>> m_nextFreeNs; // shared software service per hop
    bool m_done = false;
    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::vector<uint16_t> m_order; // root to leaf
    std::vector<uint16_t> m_position;
    std::vector<uint16_t> m_path; // leaf -> root -> leaf
    std::vector<uint64_t> m_channelBytes;
    std::vector<ChannelGroup> m_groups;
    std::function<void(uint64_t)> m_complete;
};

} // namespace ns3
#endif
