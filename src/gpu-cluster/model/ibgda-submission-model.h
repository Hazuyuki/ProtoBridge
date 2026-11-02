/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IBGDA_SUBMISSION_MODEL_H
#define IBGDA_SUBMISSION_MODEL_H

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/tag.h"

#include <deque>
#include <functional>
#include <map>
#include <vector>
#include <string>
#include <unordered_map>

namespace ns3
{
class CommandLine;

/** GPU-initiated RC RDMA timing. Times default to zero, not a fitted NIC profile.
 * QP depth counts logical work requests; the endpoint packet window is separate.
 * producer = an abstract GPU warp/CTA supplied by an upper-level GPU simulator.
 */
struct IbgdaConfig
{
    uint32_t producers{1};
    uint32_t qpsPerPeer{1};
    uint32_t queueDepth{128};
    uint32_t doorbellBatch{1};
    uint32_t cqModeration{1};
    uint32_t mtuBytes{4096};
    uint32_t wqeBytes{64};
    bool mapQpByProducer{true};
    Time wqeBuild{NanoSeconds(0)};
    Time publish{NanoSeconds(0)};
    Time doorbell{NanoSeconds(0)};
    Time batchTimeout{NanoSeconds(100)};
    Time wqeFetch{NanoSeconds(0)};
    Time dmaReadLatency{NanoSeconds(0)};
    Time dmaWriteLatency{NanoSeconds(0)};
    Time cqWrite{NanoSeconds(0)};
    Time gpuPoll{NanoSeconds(0)};
    double pcieReadGbps{0};  ///< 0 = no bandwidth constraint, still pays latency
    double pcieWriteGbps{0};
    double hbmReadGbps{0};
    double hbmWriteGbps{0};
    void Validate() const;
};

struct IbgdaWorkTiming
{
    uint64_t id{0};
    uint64_t bytes{0};
    uint16_t peer{0};
    uint32_t producer{0};
    uint32_t qp{0};
    Time ready, reserved, published, doorbell, execute, firstWire, acknowledged, observed;
    bool reachedWire{false};
};

struct IbgdaStats
{
    uint64_t submitted{0}, completed{0}, failed{0}, doorbells{0}, cqes{0};
    uint64_t peakOutstanding{0}, peakQpOutstanding{0};
    uint64_t startupSamples{0}, startupTotalNs{0}, startupMaxNs{0};
};

/// Shared CLI/profile vocabulary; profile values override matching CLI values.
void AddIbgdaCommandLine(CommandLine& cmd, std::string& mode, IbgdaConfig& config);
void ApplyIbgdaProfile(const std::unordered_map<std::string,std::string>& values,
                       std::string& mode, IbgdaConfig& config);

/** Local simulation metadata, not an additional transport header or wire cost. */
class IbgdaPacketTag : public Tag
{
  public:
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer buffer) const override;
    void Deserialize(TagBuffer buffer) override;
    void Print(std::ostream& os) const override;
    uint64_t workId{0};
    uint32_t qp{0};
    bool sourceReady{false};
    bool destinationReady{false};
};

class IbgdaSubmissionModel : public Object
{
  public:
    static TypeId GetTypeId();
    using Execute = std::function<void(uint64_t)>;
    using Completion = std::function<void(bool)>;
    using Observer = std::function<void(const IbgdaWorkTiming&)>;
    void Configure(const IbgdaConfig& config);
    const IbgdaConfig& GetConfig() const;
    const IbgdaStats& GetStats() const;
    uint64_t GetOutstanding() const;
    uint32_t GetQpForWork(uint64_t workId) const;
    uint64_t Submit(uint16_t peer, uint64_t bytes, Execute execute,
                    uint32_t producer = UINT32_MAX, Completion complete = {});
    /// Flush all published tails and await the requests issued before this call.
    void Quiet(Completion complete);
    /// Enforce subsequent same-peer requests after prior remote completions.
    /// This is a conservative RC fence (ordering, with a stronger drain).
    void Fence(uint16_t peer);
    void NotifyPacketAck(uint64_t workId, uint64_t bytes);
    void NotifyFirstWire(uint64_t workId);
    void NotifyFailure(uint64_t workId);
    Time ReserveSourceRead(uint64_t bytes);
    Time ReserveDestinationWrite(uint64_t bytes);
    void SetObserver(Observer observer);

  protected:
    void DoDispose() override;

  private:
    using QpKey = std::pair<uint16_t, uint32_t>;
    struct Work
    {
        IbgdaWorkTiming time;
        Execute execute;
        Completion complete;
        uint64_t acknowledgedBytes{0};
        uint64_t fenceThrough{0};
        bool acked{false};
        bool failed{false};
    };
    struct Qp
    {
        std::deque<uint64_t> waiting, published, outstanding;
        uint32_t occupied{0};
        Time publishFree, fetchFree;
        EventId batchEvent;
    };
    struct QuietWait { uint64_t watermark; Completion complete; };
    void Pump(const QpKey& key);
    void Publish(const QpKey& key, uint64_t id);
    void Ring(const QpKey& key);
    void ExecuteWork(uint64_t id);
    void Progress(const QpKey& key);
    void Observe(const QpKey& key, uint64_t through);
    void CheckQuiet();
    void Schedule(Time delay, std::function<void()> fn);
    Time Reserve(uint64_t bytes, double gbps, Time& free, Time earliest);
    bool BlockedByFence(const Work& work) const;
    IbgdaConfig m_config;
    IbgdaStats m_stats;
    uint64_t m_nextId{1};
    bool m_disposed{false};
    std::vector<Time> m_producerFree;
    std::map<uint64_t, Work> m_work;
    std::map<QpKey, Qp> m_qps;
    std::map<uint16_t, uint64_t> m_fences;
    std::map<uint16_t, uint64_t> m_nextQp;
    std::vector<QuietWait> m_quiet;
    std::vector<EventId> m_events;
    Observer m_observer;
    Time m_doorbellFree, m_pcieReadFree, m_pcieWriteFree, m_hbmReadFree, m_hbmWriteFree;
};

} // namespace ns3
#endif
