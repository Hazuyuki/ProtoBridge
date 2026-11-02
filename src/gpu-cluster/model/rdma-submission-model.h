/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RDMA_SUBMISSION_MODEL_H
#define RDMA_SUBMISSION_MODEL_H

#include "rdma-operation.h"

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/tag.h"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ns3
{
class CommandLine;

/** CPU- or GPU-initiated RC RDMA timing with independent payload paths.
 * Times default to zero, not a fitted NIC profile. QP depth counts network work
 * requests (staging chunks for staged transfers), independently of the endpoint
 * packet window. A producer is a CPU worker or an abstract GPU warp/CTA.
 */
struct RdmaConfig
{
    std::string initiator{"gpu"};      ///< cpu or gpu; independent of payload location
    std::string dataPath{"gpudirect"}; ///< host, staged, or gpudirect
    uint32_t cpuWorkers{1};
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
    double pcieReadGbps{0}; ///< 0 = no bandwidth constraint, still pays latency
    double pcieWriteGbps{0};
    double hbmReadGbps{0};
    double hbmWriteGbps{0};
    Time cpuWqeBuild{NanoSeconds(0)}, cpuPublish{NanoSeconds(0)};
    Time cpuDoorbell{NanoSeconds(0)}, cpuPoll{NanoSeconds(0)}, cpuHandoff{NanoSeconds(0)};
    double hostReadGbps{0}, hostWriteGbps{0};
    double d2hGbps{0}, h2dGbps{0};
    Time d2hLatency{NanoSeconds(0)}, h2dLatency{NanoSeconds(0)};
    uint32_t stagingChunkBytes{65536};
    uint32_t stagingBufferBytes{131072}; ///< Separate source and destination pools
    uint32_t maxReadAtomic{16}, responderDepth{16}, receiveQueueDepth{128}, rnrRetryLimit{7};
    Time readService{NanoSeconds(0)}, atomicService{NanoSeconds(0)};
    Time rnrRetry{NanoSeconds(1000)}, receivePost{NanoSeconds(0)}, signalPoll{NanoSeconds(0)};
    // Inputs for the optional native RDMA-operations workload.
    std::string operation{"write"}, signalOperation{"set"};
    uint64_t operand{1}, compare{0}, initialValue{0}, signalValue{1};
    uint64_t localAddress{0}, remoteAddress{0}, signalAddress{1ull << 48};
    uint32_t immediate{0};
    Time receivePostDelay{NanoSeconds(0)};

    uint32_t ProducerCount() const
    {
        return initiator == "cpu" ? cpuWorkers : producers;
    }

    void Validate() const;
};

struct RdmaWorkTiming
{
    uint64_t id{0};
    uint64_t requestId{0}, offset{0};
    uint64_t bytes{0};
    uint16_t peer{0};
    uint32_t producer{0};
    uint32_t qp{0};
    RdmaOpcode opcode{RdmaOpcode::WRITE};
    Time ready, reserved, published, doorbell, execute, firstWire, acknowledged, observed;
    Time stagingAcquired, sourceStaged, submitted;
    bool reachedWire{false};
};

struct RdmaStats
{
    uint64_t requests{0}, completedRequests{0};
    uint64_t failedRequests{0}, peakReadAtomic{0};
    uint64_t submitted{0}, completed{0}, failed{0}, doorbells{0}, cqes{0};
    uint64_t peakOutstanding{0}, peakQpOutstanding{0};
    uint64_t startupSamples{0}, startupTotalNs{0}, startupMaxNs{0};
    uint64_t requestStartupSamples{0}, requestStartupTotalNs{0}, requestStartupMaxNs{0};
    uint64_t sourceD2hBytes{0}, destinationH2dBytes{0};
    uint64_t hostReadBytes{0}, hostWriteBytes{0}, hbmReadBytes{0}, hbmWriteBytes{0};
    uint64_t sourceStagingPeakBytes{0}, destinationStagingPeakBytes{0};
    uint64_t lastObservedNs{0}, lastDataReadyNs{0};
};

/// Shared CLI/profile vocabulary; profile values override matching CLI values.
void AddRdmaCommandLine(CommandLine& cmd, std::string& mode, RdmaConfig& config);
void ApplyRdmaProfile(const std::unordered_map<std::string, std::string>& values,
                      std::string& mode,
                      RdmaConfig& config);
bool IsDetailedRdmaMode(const std::string& mode);
std::string CanonicalRdmaOption(const std::string& name);

/** Local simulation metadata, not an additional transport header or wire cost. */
class RdmaPacketTag : public Tag
{
  public:
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer buffer) const override;
    void Deserialize(TagBuffer buffer) override;
    void Print(std::ostream& os) const override;
    uint64_t workId{0};
    uint64_t workBytes{0};
    uint32_t qp{0};
    bool sourceReady{false};
    bool destinationReady{false};
    bool ackIssued{false};
};

/// Share this object between NIC endpoints attached to the same host DRAM.
struct RdmaHostMemoryResources
{
    Time readFree, writeFree;
};

class RdmaSubmissionModel : public Object
{
  public:
    static TypeId GetTypeId();
    using Execute = std::function<void(uint64_t)>;
    using Completion = std::function<void(bool)>;
    using Observer = std::function<void(const RdmaWorkTiming&)>;
    void Configure(const RdmaConfig& config);
    void SetHostMemoryResources(std::shared_ptr<RdmaHostMemoryResources> resources);
    const RdmaConfig& GetConfig() const;
    const RdmaStats& GetStats() const;
    uint64_t GetOutstanding() const;
    uint32_t GetQpForWork(uint64_t workId) const;
    uint64_t GetBytesForWork(uint64_t workId) const;
    RdmaWorkTiming GetWorkTiming(uint64_t workId) const;
    bool RequestSucceeded(uint64_t workId) const;
    uint64_t Submit(uint16_t peer,
                    uint64_t bytes,
                    Execute execute,
                    uint32_t producer = UINT32_MAX,
                    Completion complete = {});
    uint64_t SubmitOperation(RdmaOpcode opcode,
                             uint16_t peer,
                             uint64_t bytes,
                             Execute execute,
                             uint32_t producer = UINT32_MAX,
                             Completion complete = {});
    /// Complete an operation after its response/data/notification condition.
    void NotifyOperationComplete(uint64_t workId, bool success = true);
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
    Time ReserveDestinationH2d(uint64_t bytes);
    void AcquireReceiveStaging(uint64_t bytes, std::function<void()> ready);
    void ReleaseReceiveStaging(uint64_t bytes);
    /// D2H plus finite source space for responder-generated read/atomic traffic.
    void StageSourcePayload(uint64_t bytes, std::function<void()> ready);
    void ReleaseSourceStaging(uint64_t bytes);
    void NotifyDataReady();
    void SetObserver(Observer observer);

  protected:
    void DoDispose() override;

  private:
    using QpKey = std::pair<uint16_t, uint32_t>;

    struct Work
    {
        RdmaWorkTiming time;
        Execute execute;
        Completion complete;
        uint64_t acknowledgedBytes{0};
        uint64_t fenceThrough{0};
        bool acked{false};
        bool failed{false};
        bool stagingAllocated{false};
        std::shared_ptr<bool> requestReachedWire;
        std::shared_ptr<bool> requestSucceeded;
    };

    struct Qp
    {
        std::deque<uint64_t> waiting, published, outstanding;
        uint32_t occupied{0};
        uint32_t readAtomicOccupied{0};
        Time publishFree, fetchFree;
        EventId batchEvent;
    };

    struct QuietWait
    {
        uint64_t watermark;
        Completion complete;
    };

    void Pump(const QpKey& key);
    void Publish(const QpKey& key, uint64_t id);
    void Ring(const QpKey& key);
    void ExecuteWork(uint64_t id);
    void Progress(const QpKey& key);
    void Observe(const QpKey& key, uint64_t through);
    void CheckQuiet();
    void PumpSourceStaging();
    void PumpReceiveStaging();
    Time ReserveSourceD2h(uint64_t bytes);
    void Schedule(Time delay, std::function<void()> fn);
    Time Reserve(uint64_t bytes, double gbps, Time& free, Time earliest);
    bool BlockedByFence(const Work& work) const;
    RdmaConfig m_config;
    RdmaStats m_stats;
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
    Time m_d2hFree, m_h2dFree;
    std::shared_ptr<RdmaHostMemoryResources> m_hostMemory{
        std::make_shared<RdmaHostMemoryResources>()};
    uint64_t m_sourceStagingUsed{0}, m_receiveStagingUsed{0};

    struct SourceStageWait
    {
        uint64_t id{0}, bytes{0};
        std::function<void()> ready;
    };

    std::deque<SourceStageWait> m_sourceStagingWaiting;
    std::deque<std::pair<uint64_t, std::function<void()>>> m_receiveStagingWaiting;
};

} // namespace ns3
#endif
