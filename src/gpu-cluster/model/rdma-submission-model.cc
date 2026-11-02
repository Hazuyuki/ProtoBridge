/* SPDX-License-Identifier: GPL-2.0-only */
#include "rdma-submission-model.h"

#include "ns3/abort.h"
#include "ns3/command-line.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ns3
{
NS_OBJECT_ENSURE_REGISTERED(RdmaSubmissionModel);
NS_OBJECT_ENSURE_REGISTERED(RdmaPacketTag);

namespace
{
using UintFields = std::vector<std::pair<std::string, uint32_t*>>;
using TimeFields = std::vector<std::pair<std::string, Time*>>;
using RateFields = std::vector<std::pair<std::string, double*>>;

UintFields
UintOptions(RdmaConfig& c)
{
    return {{"rdmaGpuProducers", &c.producers},
            {"rdmaCpuWorkers", &c.cpuWorkers},
            {"rdmaQpsPerPeer", &c.qpsPerPeer},
            {"rdmaQueueDepth", &c.queueDepth},
            {"rdmaDoorbellBatch", &c.doorbellBatch},
            {"rdmaCqModeration", &c.cqModeration},
            {"rdmaMtu", &c.mtuBytes},
            {"rdmaWqeBytes", &c.wqeBytes},
            {"rdmaStagingChunkBytes", &c.stagingChunkBytes},
            {"rdmaStagingBufferBytes", &c.stagingBufferBytes}};
}

TimeFields
TimeOptions(RdmaConfig& c)
{
    return {{"rdmaGpuWqeBuildNs", &c.wqeBuild},
            {"rdmaCpuWqeBuildNs", &c.cpuWqeBuild},
            {"rdmaGpuPublishNs", &c.publish},
            {"rdmaCpuPublishNs", &c.cpuPublish},
            {"rdmaGpuDoorbellNs", &c.doorbell},
            {"rdmaCpuDoorbellNs", &c.cpuDoorbell},
            {"rdmaGpuPollNs", &c.gpuPoll},
            {"rdmaCpuPollNs", &c.cpuPoll},
            {"rdmaCpuHandoffNs", &c.cpuHandoff},
            {"rdmaBatchTimeoutNs", &c.batchTimeout},
            {"rdmaWqeFetchNs", &c.wqeFetch},
            {"rdmaDmaReadNs", &c.dmaReadLatency},
            {"rdmaDmaWriteNs", &c.dmaWriteLatency},
            {"rdmaCqWriteNs", &c.cqWrite},
            {"rdmaD2hNs", &c.d2hLatency},
            {"rdmaH2dNs", &c.h2dLatency}};
}

RateFields
RateOptions(RdmaConfig& c)
{
    return {{"rdmaPcieReadGbps", &c.pcieReadGbps},
            {"rdmaPcieWriteGbps", &c.pcieWriteGbps},
            {"rdmaHbmReadGbps", &c.hbmReadGbps},
            {"rdmaHbmWriteGbps", &c.hbmWriteGbps},
            {"rdmaHostReadGbps", &c.hostReadGbps},
            {"rdmaHostWriteGbps", &c.hostWriteGbps},
            {"rdmaD2hGbps", &c.d2hGbps},
            {"rdmaH2dGbps", &c.h2dGbps}};
}

const std::unordered_map<std::string, std::string> ALIASES{
    {"ibgdaProducers", "rdmaGpuProducers"},
    {"ibgdaQpsPerPeer", "rdmaQpsPerPeer"},
    {"ibgdaQueueDepth", "rdmaQueueDepth"},
    {"ibgdaDoorbellBatch", "rdmaDoorbellBatch"},
    {"ibgdaCqModeration", "rdmaCqModeration"},
    {"ibgdaMtu", "rdmaMtu"},
    {"ibgdaWqeBytes", "rdmaWqeBytes"},
    {"ibgdaMapQpByProducer", "rdmaMapQpByProducer"},
    {"ibgdaWqeBuildNs", "rdmaGpuWqeBuildNs"},
    {"ibgdaPublishNs", "rdmaGpuPublishNs"},
    {"ibgdaDoorbellNs", "rdmaGpuDoorbellNs"},
    {"ibgdaGpuPollNs", "rdmaGpuPollNs"},
    {"ibgdaBatchTimeoutNs", "rdmaBatchTimeoutNs"},
    {"ibgdaWqeFetchNs", "rdmaWqeFetchNs"},
    {"ibgdaDmaReadNs", "rdmaDmaReadNs"},
    {"ibgdaDmaWriteNs", "rdmaDmaWriteNs"},
    {"ibgdaCqWriteNs", "rdmaCqWriteNs"},
    {"ibgdaPcieReadGbps", "rdmaPcieReadGbps"},
    {"ibgdaPcieWriteGbps", "rdmaPcieWriteGbps"},
    {"ibgdaHbmReadGbps", "rdmaHbmReadGbps"},
    {"ibgdaHbmWriteGbps", "rdmaHbmWriteGbps"}};

bool
ParseNanoseconds(Time* dest, const std::string& value)
{
    try
    {
        size_t pos = 0;
        long long number = std::stoll(value, &pos);
        if (pos != value.size() || number < 0)
        {
            return false;
        }
        *dest = NanoSeconds(number);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}
} // namespace

bool
IsDetailedRdmaMode(const std::string& mode)
{
    return mode == "detailed" || mode == "ibgda";
}

std::string
CanonicalRdmaOption(const std::string& name)
{
    auto found = ALIASES.find(name);
    return found == ALIASES.end() ? name : found->second;
}

void
AddRdmaCommandLine(CommandLine& cmd, std::string& mode, RdmaConfig& config)
{
    cmd.AddValue("rdmaSubmission", "RDMA timing: startup, detailed, or legacy ibgda alias", mode);
    cmd.AddValue("rdmaInitiator", "Request submitter: cpu or gpu", config.initiator);
    cmd.AddValue("rdmaDataPath",
                 "Payload memory path: host, staged, or gpudirect",
                 config.dataPath);
    for (auto [name, dest] : UintOptions(config))
    {
        cmd.AddValue(name, "RDMA count/size; see doc/RDMA.md", *dest);
        for (const auto& [alias, target] : ALIASES)
        {
            if (target == name)
            {
                cmd.AddValue(alias, "Compatibility alias for " + target, *dest);
            }
        }
    }
    for (auto [name, dest] : TimeOptions(config))
    {
        cmd.AddValue(name,
                     "RDMA integer ns (0=no added latency)",
                     MakeBoundCallback(&ParseNanoseconds, dest));
        for (const auto& [alias, target] : ALIASES)
        {
            if (target == name)
            {
                cmd.AddValue(alias,
                             "Compatibility alias for " + target,
                             MakeBoundCallback(&ParseNanoseconds, dest));
            }
        }
    }
    for (auto [name, dest] : RateOptions(config))
    {
        cmd.AddValue(name, "RDMA effective bandwidth in Gb/s (0=unconstrained)", *dest);
        for (const auto& [alias, target] : ALIASES)
        {
            if (target == name)
            {
                cmd.AddValue(alias, "Compatibility alias for " + target, *dest);
            }
        }
    }
    cmd.AddValue("rdmaMapQpByProducer",
                 "1=map by worker/producer; 0=round-robin per request",
                 config.mapQpByProducer);
    cmd.AddValue("ibgdaMapQpByProducer",
                 "Compatibility alias for rdmaMapQpByProducer",
                 config.mapQpByProducer);
}

void
ApplyRdmaProfile(const std::unordered_map<std::string, std::string>& input,
                 std::string& mode,
                 RdmaConfig& config)
{
    auto values = input;
    for (const auto& [alias, target] : ALIASES)
    {
        auto found = input.find(alias);
        if (found == input.end())
        {
            continue;
        }
        auto canonical = input.find(target);
        NS_ABORT_MSG_IF(canonical != input.end() && canonical->second != found->second,
                        "Conflicting RDMA options: " << alias << " and " << target);
        values[target] = found->second;
        values.erase(alias);
    }
    std::vector<std::string> known{"rdmaSubmission",
                                   "rdmaInitiator",
                                   "rdmaDataPath",
                                   "rdmaMapQpByProducer"};
    for (auto [name, dest] : UintOptions(config))
    {
        known.push_back(name);
    }
    for (auto [name, dest] : TimeOptions(config))
    {
        known.push_back(name);
    }
    for (auto [name, dest] : RateOptions(config))
    {
        known.push_back(name);
    }
    for (const auto& [name, value] : values)
    {
        NS_ABORT_MSG_IF((name.rfind("ibgda", 0) == 0 || name.rfind("rdma", 0) == 0) &&
                            std::find(known.begin(), known.end(), name) == known.end(),
                        "Unknown RDMA profile option: " << name);
    }
    auto assign = [&values](const char* key, std::string& value) {
        auto it = values.find(key);
        if (it != values.end())
        {
            value = it->second;
        }
    };
    assign("rdmaSubmission", mode);
    assign("rdmaInitiator", config.initiator);
    assign("rdmaDataPath", config.dataPath);
    for (auto [name, dest] : UintOptions(config))
    {
        auto it = values.find(name);
        if (it == values.end())
        {
            continue;
        }
        try
        {
            size_t pos = 0;
            auto number = std::stoull(it->second, &pos);
            NS_ABORT_MSG_IF(pos != it->second.size() || number > UINT32_MAX,
                            "Invalid RDMA integer: " << name);
            *dest = static_cast<uint32_t>(number);
        }
        catch (const std::exception&)
        {
            NS_ABORT_MSG("Invalid RDMA integer: " << name);
        }
    }
    for (auto [name, dest] : TimeOptions(config))
    {
        auto it = values.find(name);
        NS_ABORT_MSG_IF(it != values.end() && !ParseNanoseconds(dest, it->second),
                        "Invalid RDMA nanoseconds: " << name);
    }
    for (auto [name, dest] : RateOptions(config))
    {
        auto it = values.find(name);
        if (it == values.end())
        {
            continue;
        }
        try
        {
            size_t pos = 0;
            *dest = std::stod(it->second, &pos);
            NS_ABORT_MSG_IF(pos != it->second.size(), "Invalid RDMA rate: " << name);
        }
        catch (const std::exception&)
        {
            NS_ABORT_MSG("Invalid RDMA rate: " << name);
        }
    }
    auto found = values.find("rdmaMapQpByProducer");
    if (found != values.end())
    {
        NS_ABORT_MSG_IF(found->second != "0" && found->second != "1",
                        "rdmaMapQpByProducer must be 0 or 1");
        config.mapQpByProducer = found->second == "1";
    }
    NS_ABORT_MSG_IF(mode != "startup" && !IsDetailedRdmaMode(mode),
                    "rdmaSubmission must be startup, detailed or ibgda");
    NS_ABORT_MSG_IF(mode == "ibgda" &&
                        (config.initiator != "gpu" || config.dataPath != "gpudirect"),
                    "The ibgda alias requires gpu/gpudirect; use detailed for independent choices");
    if (IsDetailedRdmaMode(mode))
    {
        config.Validate();
    }
}

void
RdmaConfig::Validate() const
{
    NS_ABORT_MSG_IF(initiator != "cpu" && initiator != "gpu", "rdmaInitiator must be cpu or gpu");
    NS_ABORT_MSG_IF(dataPath != "host" && dataPath != "staged" && dataPath != "gpudirect",
                    "rdmaDataPath must be host, staged or gpudirect");
    NS_ABORT_MSG_IF(cpuWorkers == 0 || cpuWorkers > 65536, "RDMA CPU workers must be in [1,65536]");
    NS_ABORT_MSG_IF(stagingChunkBytes == 0 || stagingBufferBytes < stagingChunkBytes,
                    "RDMA staging buffer must hold at least one nonzero staging chunk");
    NS_ABORT_MSG_IF(producers == 0 || qpsPerPeer == 0 || queueDepth == 0,
                    "RDMA producers, QPs and queue depth must be positive");
    NS_ABORT_MSG_IF(producers > 65536 || qpsPerPeer > 65536 || queueDepth > 1048576,
                    "RDMA counts exceed supported producer/QP/queue bounds");
    NS_ABORT_MSG_IF(doorbellBatch == 0 || doorbellBatch > queueDepth || cqModeration == 0 ||
                        cqModeration > queueDepth,
                    "RDMA doorbell batch and CQ moderation must be in [1, queueDepth]");
    NS_ABORT_MSG_IF(mtuBytes == 0 || mtuBytes > (1u << 20) || wqeBytes == 0,
                    "RDMA needs MTU in [1,1048576] B and positive WQE bytes");
    for (Time t : {wqeBuild,
                   publish,
                   doorbell,
                   batchTimeout,
                   wqeFetch,
                   dmaReadLatency,
                   dmaWriteLatency,
                   cqWrite,
                   gpuPoll,
                   cpuWqeBuild,
                   cpuPublish,
                   cpuDoorbell,
                   cpuPoll,
                   cpuHandoff,
                   d2hLatency,
                   h2dLatency})
    {
        NS_ABORT_MSG_IF(t.IsStrictlyNegative(), "RDMA timings must be nonnegative");
    }
    NS_ABORT_MSG_IF(doorbellBatch > 1 && batchTimeout.IsZero(),
                    "RDMA batching needs a positive timeout to flush partial batches");
    for (double bw : {pcieReadGbps,
                      pcieWriteGbps,
                      hbmReadGbps,
                      hbmWriteGbps,
                      hostReadGbps,
                      hostWriteGbps,
                      d2hGbps,
                      h2dGbps})
    {
        NS_ABORT_MSG_IF(!std::isfinite(bw) || bw < 0,
                        "RDMA bandwidth must be finite and nonnegative (0=unconstrained)");
    }
}

TypeId
RdmaPacketTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RdmaPacketTag")
                            .AddDeprecatedName("ns3::IbgdaPacketTag")
                            .SetParent<Tag>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RdmaPacketTag>();
    return tid;
}

TypeId
RdmaPacketTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
RdmaPacketTag::GetSerializedSize() const
{
    return 23;
}

void
RdmaPacketTag::Serialize(TagBuffer b) const
{
    b.WriteU64(workId);
    b.WriteU64(workBytes);
    b.WriteU32(qp);
    b.WriteU8(sourceReady);
    b.WriteU8(destinationReady);
    b.WriteU8(ackIssued);
}

void
RdmaPacketTag::Deserialize(TagBuffer b)
{
    workId = b.ReadU64();
    workBytes = b.ReadU64();
    qp = b.ReadU32();
    sourceReady = b.ReadU8();
    destinationReady = b.ReadU8();
    ackIssued = b.ReadU8();
}

void
RdmaPacketTag::Print(std::ostream& os) const
{
    os << "work=" << workId;
}

TypeId
RdmaSubmissionModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RdmaSubmissionModel")
                            .AddDeprecatedName("ns3::IbgdaSubmissionModel")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RdmaSubmissionModel>();
    return tid;
}

void
RdmaSubmissionModel::Configure(const RdmaConfig& config)
{
    NS_ABORT_MSG_IF(m_stats.submitted != 0, "Configure RDMA before submitting work");
    config.Validate();
    m_config = config;
    m_producerFree.assign(config.ProducerCount(), Time(0));
}

void
RdmaSubmissionModel::SetHostMemoryResources(std::shared_ptr<RdmaHostMemoryResources> resources)
{
    NS_ABORT_MSG_IF(!resources || m_stats.submitted != 0,
                    "Set host memory resources before RDMA work");
    m_hostMemory = std::move(resources);
}

const RdmaConfig&
RdmaSubmissionModel::GetConfig() const
{
    return m_config;
}

const RdmaStats&
RdmaSubmissionModel::GetStats() const
{
    return m_stats;
}

uint64_t
RdmaSubmissionModel::GetOutstanding() const
{
    return m_work.size();
}

uint32_t
RdmaSubmissionModel::GetQpForWork(uint64_t id) const
{
    return m_work.at(id).time.qp;
}

uint64_t
RdmaSubmissionModel::GetBytesForWork(uint64_t id) const
{
    return m_work.at(id).time.bytes;
}

void
RdmaSubmissionModel::SetObserver(Observer observer)
{
    m_observer = std::move(observer);
}

void
RdmaSubmissionModel::Schedule(Time delay, std::function<void()> fn)
{
    m_events.erase(std::remove_if(m_events.begin(),
                                  m_events.end(),
                                  [](const EventId& e) { return e.IsExpired(); }),
                   m_events.end());
    m_events.push_back(Simulator::Schedule(delay, [this, fn = std::move(fn)] {
        if (!m_disposed)
        {
            fn();
        }
    }));
}

Time
RdmaSubmissionModel::Reserve(uint64_t bytes, double gbps, Time& free, Time earliest)
{
    Time start = std::max(earliest, free);
    Time service = gbps > 0 ? Seconds(static_cast<double>(bytes) * 8 / (gbps * 1e9)) : Time(0);
    free = start + service;
    return free;
}

Time
RdmaSubmissionModel::ReserveSourceRead(uint64_t bytes)
{
    const Time now = Simulator::Now();
    Time memory;
    if (m_config.dataPath == "gpudirect")
    {
        m_stats.hbmReadBytes += bytes;
        memory = Reserve(bytes, m_config.hbmReadGbps, m_hbmReadFree, now);
    }
    else
    {
        m_stats.hostReadBytes += bytes;
        memory = Reserve(bytes, m_config.hostReadGbps, m_hostMemory->readFree, now);
    }
    Time pcie = Reserve(bytes, m_config.pcieReadGbps, m_pcieReadFree, now);
    return std::max(memory, pcie) + m_config.dmaReadLatency - now;
}

Time
RdmaSubmissionModel::ReserveDestinationWrite(uint64_t bytes)
{
    const Time now = Simulator::Now();
    Time memory;
    if (m_config.dataPath == "gpudirect")
    {
        m_stats.hbmWriteBytes += bytes;
        memory = Reserve(bytes, m_config.hbmWriteGbps, m_hbmWriteFree, now);
    }
    else
    {
        m_stats.hostWriteBytes += bytes;
        memory = Reserve(bytes, m_config.hostWriteGbps, m_hostMemory->writeFree, now);
    }
    Time pcie = Reserve(bytes, m_config.pcieWriteGbps, m_pcieWriteFree, now);
    return std::max(memory, pcie) + m_config.dmaWriteLatency - now;
}

Time
RdmaSubmissionModel::ReserveSourceD2h(uint64_t bytes)
{
    const Time now = Simulator::Now();
    m_stats.hbmReadBytes += bytes;
    m_stats.hostWriteBytes += bytes;
    m_stats.sourceD2hBytes += bytes;
    Time hbm = Reserve(bytes, m_config.hbmReadGbps, m_hbmReadFree, now);
    Time host = Reserve(bytes, m_config.hostWriteGbps, m_hostMemory->writeFree, now);
    Time copy = Reserve(bytes, m_config.d2hGbps, m_d2hFree, now);
    return std::max({hbm, host, copy}) + m_config.d2hLatency - now;
}

Time
RdmaSubmissionModel::ReserveDestinationH2d(uint64_t bytes)
{
    const Time now = Simulator::Now();
    m_stats.hostReadBytes += bytes;
    m_stats.hbmWriteBytes += bytes;
    m_stats.destinationH2dBytes += bytes;
    Time host = Reserve(bytes, m_config.hostReadGbps, m_hostMemory->readFree, now);
    Time hbm = Reserve(bytes, m_config.hbmWriteGbps, m_hbmWriteFree, now);
    Time copy = Reserve(bytes, m_config.h2dGbps, m_h2dFree, now);
    return std::max({host, hbm, copy}) + m_config.h2dLatency - now;
}

void
RdmaSubmissionModel::NotifyDataReady()
{
    m_stats.lastDataReadyNs = Simulator::Now().GetNanoSeconds();
}

void
RdmaSubmissionModel::AcquireReceiveStaging(uint64_t bytes, std::function<void()> ready)
{
    NS_ABORT_MSG_IF(bytes == 0 || bytes > m_config.stagingBufferBytes,
                    "Incoming RDMA WR exceeds receive staging buffer; use smaller staging chunks");
    m_receiveStagingWaiting.emplace_back(bytes, std::move(ready));
    PumpReceiveStaging();
}

void
RdmaSubmissionModel::PumpReceiveStaging()
{
    while (!m_receiveStagingWaiting.empty())
    {
        auto& pending = m_receiveStagingWaiting.front();
        if (pending.first > m_config.stagingBufferBytes - m_receiveStagingUsed)
        {
            break;
        }
        auto [bytes, callback] = std::move(pending);
        m_receiveStagingWaiting.pop_front();
        m_receiveStagingUsed += bytes;
        m_stats.destinationStagingPeakBytes =
            std::max(m_stats.destinationStagingPeakBytes, m_receiveStagingUsed);
        Schedule(Time(0), std::move(callback));
    }
}

void
RdmaSubmissionModel::ReleaseReceiveStaging(uint64_t bytes)
{
    NS_ABORT_MSG_IF(bytes > m_receiveStagingUsed, "Receive staging buffer accounting underflow");
    m_receiveStagingUsed -= bytes;
    PumpReceiveStaging();
}

void
RdmaSubmissionModel::PumpSourceStaging()
{
    while (!m_sourceStagingWaiting.empty())
    {
        uint64_t id = m_sourceStagingWaiting.front();
        auto& w = m_work.at(id);
        if (w.time.bytes > m_config.stagingBufferBytes - m_sourceStagingUsed)
        {
            break;
        }
        m_sourceStagingWaiting.pop_front();
        w.stagingAllocated = true;
        w.time.stagingAcquired = Simulator::Now();
        m_sourceStagingUsed += w.time.bytes;
        m_stats.sourceStagingPeakBytes =
            std::max(m_stats.sourceStagingPeakBytes, m_sourceStagingUsed);
        Time delay = ReserveSourceD2h(w.time.bytes);
        Schedule(delay, [this, id] {
            auto& ready = m_work.at(id);
            ready.time.sourceStaged = Simulator::Now();
            ready.time.submitted = Simulator::Now();
            QpKey key{ready.time.peer, ready.time.qp};
            m_qps[key].waiting.push_back(id);
            Pump(key);
        });
    }
}

uint64_t
RdmaSubmissionModel::Submit(uint16_t peer,
                            uint64_t bytes,
                            Execute execute,
                            uint32_t producer,
                            Completion complete)
{
    NS_ABORT_MSG_IF(m_disposed || !execute || bytes == 0,
                    "RDMA needs a live model, nonzero bytes and an execute callback");
    if (m_producerFree.empty())
    {
        Configure(m_config);
    }
    if (producer != UINT32_MAX)
    {
        NS_ABORT_MSG_IF(producer >= m_config.ProducerCount(), "RDMA producer ID out of range");
    }
    const uint64_t chunks =
        m_config.dataPath == "staged" ? 1 + (bytes - 1) / m_config.stagingChunkBytes : 1;
    NS_ABORT_MSG_IF(chunks > UINT64_MAX - m_nextId, "RDMA work ID exhausted");
    const uint64_t requestId = m_nextId;
    ++m_stats.requests;
    auto remaining = std::make_shared<uint64_t>(chunks);
    auto requestReachedWire = std::make_shared<bool>(false);
    auto finish = [this, remaining, complete](bool ok) {
        if (--*remaining == 0)
        {
            ++m_stats.completedRequests;
            if (complete)
            {
                complete(ok);
            }
        }
    };
    std::vector<uint64_t> deferred;
    Time handoff = (m_config.initiator == "cpu" && m_config.dataPath != "host")
                       ? m_config.cpuHandoff
                       : Time(0);
    for (uint64_t offset = 0; offset < bytes;)
    {
        uint64_t length = m_config.dataPath == "staged"
                              ? std::min<uint64_t>(m_config.stagingChunkBytes, bytes - offset)
                              : bytes;
        uint64_t id = m_nextId++;
        uint32_t selected = producer == UINT32_MAX ? (id - 1) % m_config.ProducerCount() : producer;
        uint32_t qp =
            (m_config.mapQpByProducer ? selected : m_nextQp[peer]++) % m_config.qpsPerPeer;
        Work w;
        w.time.id = id;
        w.time.requestId = requestId;
        w.time.offset = offset;
        w.time.bytes = length;
        w.time.peer = peer;
        w.time.producer = selected;
        w.time.qp = qp;
        w.time.ready = Simulator::Now();
        w.time.submitted = Simulator::Now();
        auto fence = m_fences.find(peer);
        if (fence != m_fences.end())
        {
            w.fenceThrough = fence->second;
        }
        w.execute = execute;
        w.complete = finish;
        w.requestReachedWire = requestReachedWire;
        m_work.emplace(id, std::move(w));
        ++m_stats.submitted;
        if (!handoff.IsZero())
        {
            deferred.push_back(id);
        }
        else if (m_config.dataPath == "staged")
        {
            m_sourceStagingWaiting.push_back(id);
        }
        else
        {
            QpKey key{peer, qp};
            m_qps[key].waiting.push_back(id);
            Pump(key);
        }
        offset += length;
    }
    if (!deferred.empty())
    {
        Schedule(handoff, [this, deferred] {
            for (uint64_t id : deferred)
            {
                auto& work = m_work.at(id);
                work.time.submitted = Simulator::Now();
                if (m_config.dataPath == "staged")
                {
                    m_sourceStagingWaiting.push_back(id);
                }
                else
                {
                    QpKey key{work.time.peer, work.time.qp};
                    m_qps[key].waiting.push_back(id);
                    Pump(key);
                }
            }
            if (m_config.dataPath == "staged")
            {
                PumpSourceStaging();
            }
        });
    }
    if (m_config.dataPath == "staged")
    {
        PumpSourceStaging();
    }
    return requestId;
}

bool
RdmaSubmissionModel::BlockedByFence(const Work& work) const
{
    if (work.fenceThrough == 0)
    {
        return false;
    }
    for (const auto& [id, w] : m_work)
    {
        if (id > work.fenceThrough)
        {
            break;
        }
        if (w.time.peer == work.time.peer && !w.acked)
        {
            return true;
        }
    }
    return false;
}

void
RdmaSubmissionModel::Pump(const QpKey& key)
{
    auto& qp = m_qps.at(key);
    while (!qp.waiting.empty() && qp.occupied < m_config.queueDepth)
    {
        uint64_t id = qp.waiting.front();
        Work& w = m_work.at(id);
        if (BlockedByFence(w))
        {
            break;
        }
        qp.waiting.pop_front();
        ++qp.occupied;
        qp.outstanding.push_back(id);
        w.time.reserved = Simulator::Now();
        Time buildStart = std::max(Simulator::Now(), m_producerFree[w.time.producer]);
        Time buildEnd =
            buildStart + (m_config.initiator == "cpu" ? m_config.cpuWqeBuild : m_config.wqeBuild);
        m_producerFree[w.time.producer] = buildEnd;
        // A NIC sees only the contiguous published SQ prefix, even when different
        // producers build different WQEs in parallel.
        Time publishEnd = std::max(buildEnd, qp.publishFree) +
                          (m_config.initiator == "cpu" ? m_config.cpuPublish : m_config.publish);
        qp.publishFree = publishEnd;
        Schedule(publishEnd - Simulator::Now(), [this, key, id] { Publish(key, id); });
        m_stats.peakQpOutstanding = std::max<uint64_t>(m_stats.peakQpOutstanding, qp.occupied);
        uint64_t occupied = 0;
        for (const auto& [k, q] : m_qps)
        {
            occupied += q.occupied;
        }
        m_stats.peakOutstanding = std::max(m_stats.peakOutstanding, occupied);
    }
}

void
RdmaSubmissionModel::Publish(const QpKey& key, uint64_t id)
{
    auto& qp = m_qps.at(key);
    m_work.at(id).time.published = Simulator::Now();
    qp.published.push_back(id);
    if (qp.published.size() >= m_config.doorbellBatch)
    {
        Ring(key);
    }
    else if (!qp.batchEvent.IsPending())
    {
        qp.batchEvent = Simulator::Schedule(m_config.batchTimeout, [this, key] {
            if (!m_disposed)
            {
                Ring(key);
            }
        });
    }
}

void
RdmaSubmissionModel::Ring(const QpKey& key)
{
    auto& qp = m_qps.at(key);
    if (qp.batchEvent.IsPending())
    {
        Simulator::Cancel(qp.batchEvent);
    }
    if (qp.published.empty())
    {
        return;
    }
    ++m_stats.doorbells;
    Time ringEnd = std::max(Simulator::Now(), m_doorbellFree) +
                   (m_config.initiator == "cpu" ? m_config.cpuDoorbell : m_config.doorbell);
    m_doorbellFree = ringEnd;
    while (!qp.published.empty())
    {
        uint64_t id = qp.published.front();
        qp.published.pop_front();
        auto& w = m_work.at(id);
        w.time.doorbell = ringEnd;
        Time earliest = std::max(ringEnd, qp.fetchFree);
        Time fetched = Reserve(m_config.wqeBytes, m_config.pcieReadGbps, m_pcieReadFree, earliest);
        if (m_config.initiator == "cpu")
        {
            m_stats.hostReadBytes += m_config.wqeBytes;
            fetched = std::max(fetched,
                               Reserve(m_config.wqeBytes,
                                       m_config.hostReadGbps,
                                       m_hostMemory->readFree,
                                       earliest));
        }
        else
        {
            m_stats.hbmReadBytes += m_config.wqeBytes;
            fetched =
                std::max(fetched,
                         Reserve(m_config.wqeBytes, m_config.hbmReadGbps, m_hbmReadFree, earliest));
        }
        fetched += m_config.wqeFetch;
        qp.fetchFree = fetched;
        Schedule(fetched - Simulator::Now(), [this, id] { ExecuteWork(id); });
    }
}

void
RdmaSubmissionModel::ExecuteWork(uint64_t id)
{
    auto it = m_work.find(id);
    if (it == m_work.end())
    {
        return;
    }
    it->second.time.execute = Simulator::Now();
    auto execute = std::move(it->second.execute);
    execute(id);
}

void
RdmaSubmissionModel::NotifyFirstWire(uint64_t id)
{
    auto it = m_work.find(id);
    if (it == m_work.end() || it->second.time.reachedWire)
    {
        return;
    }
    auto& t = it->second.time;
    t.firstWire = Simulator::Now();
    t.reachedWire = true;
    uint64_t startup = (t.firstWire - t.ready).GetNanoSeconds();
    if (!*it->second.requestReachedWire)
    {
        *it->second.requestReachedWire = true;
        ++m_stats.requestStartupSamples;
        m_stats.requestStartupTotalNs += startup;
        m_stats.requestStartupMaxNs = std::max(m_stats.requestStartupMaxNs, startup);
    }
    ++m_stats.startupSamples;
    m_stats.startupTotalNs += startup;
    m_stats.startupMaxNs = std::max(m_stats.startupMaxNs, startup);
}

void
RdmaSubmissionModel::NotifyPacketAck(uint64_t id, uint64_t bytes)
{
    auto it = m_work.find(id);
    if (it == m_work.end() || it->second.acked)
    {
        return;
    }
    Work& w = it->second;
    NS_ABORT_MSG_IF(bytes > w.time.bytes - w.acknowledgedBytes, "RDMA ACK exceeds WR byte ledger");
    w.acknowledgedBytes += bytes;
    if (w.acknowledgedBytes != w.time.bytes)
    {
        return;
    }
    w.acked = true;
    w.time.acknowledged = Simulator::Now();
    QpKey key{w.time.peer, w.time.qp};
    Progress(key);
    for (auto& [k, q] : m_qps)
    {
        Pump(k);
    }
}

void
RdmaSubmissionModel::Progress(const QpKey& key)
{
    auto& qp = m_qps.at(key);
    uint64_t through = 0;
    uint32_t ready = 0;
    for (uint64_t id : qp.outstanding)
    {
        if (!m_work.at(id).acked)
        {
            break;
        }
        through = id;
        ++ready;
    }
    // The last completed WR is signaled even for a short tail. Reclaiming a
    // full queue must never depend on another WR being submitted first.
    if (ready == 0)
    {
        return;
    }
    if (ready < m_config.cqModeration && ready != qp.occupied)
    {
        return;
    }
    ++m_stats.cqes;
    // Remove this prefix from the NIC's completion ledger now; queue slots
    // stay occupied until the initiator has observed the CQ write.
    for (uint32_t i = 0; i < ready; ++i)
    {
        qp.outstanding.pop_front();
    }
    Schedule(m_config.cqWrite + (m_config.initiator == "cpu" ? m_config.cpuPoll : m_config.gpuPoll),
             [this, key, through] { Observe(key, through); });
}

void
RdmaSubmissionModel::Observe(const QpKey& key, uint64_t through)
{
    auto& qp = m_qps.at(key);
    std::vector<Completion> callbacks;
    std::vector<RdmaWorkTiming> timings;
    uint64_t releasedStaging = 0;
    for (auto it = m_work.begin(); it != m_work.end();)
    {
        auto& w = it->second;
        if (it->first > through)
        {
            break;
        }
        if (w.time.peer != key.first || w.time.qp != key.second || !w.acked)
        {
            ++it;
            continue;
        }
        w.time.observed = Simulator::Now();
        m_stats.lastObservedNs = Simulator::Now().GetNanoSeconds();
        if (w.stagingAllocated)
        {
            releasedStaging += w.time.bytes;
        }
        timings.push_back(w.time);
        if (w.complete)
        {
            callbacks.push_back(std::move(w.complete));
        }
        --qp.occupied;
        ++m_stats.completed;
        it = m_work.erase(it);
    }
    NS_ABORT_MSG_IF(releasedStaging > m_sourceStagingUsed,
                    "Source staging buffer accounting underflow");
    m_sourceStagingUsed -= releasedStaging;
    if (releasedStaging)
    {
        PumpSourceStaging();
    }
    Pump(key);
    Progress(key);
    // Observers may dispose a model. Do not keep references into its mutable
    // work map or invoke pending completion callbacks after disposal.
    auto observer = m_observer;
    if (observer)
    {
        for (const auto& timing : timings)
        {
            observer(timing);
            if (m_disposed)
            {
                return;
            }
        }
    }
    for (auto& cb : callbacks)
    {
        cb(true);
        if (m_disposed)
        {
            return;
        }
    }
    CheckQuiet();
}

void
RdmaSubmissionModel::Quiet(Completion complete)
{
    NS_ABORT_MSG_IF(!complete, "RDMA quiet needs a callback");
    m_quiet.push_back({m_nextId - 1, std::move(complete)});
    for (auto& [key, qp] : m_qps)
    {
        Ring(key);
    }
    CheckQuiet();
}

void
RdmaSubmissionModel::Fence(uint16_t peer)
{
    m_fences[peer] = m_nextId - 1;
    for (auto& [key, qp] : m_qps)
    {
        if (key.first == peer)
        {
            Ring(key);
        }
    }
}

void
RdmaSubmissionModel::CheckQuiet()
{
    std::vector<Completion> ready;
    for (auto it = m_quiet.begin(); it != m_quiet.end();)
    {
        if (m_work.empty() || m_work.begin()->first > it->watermark)
        {
            ready.push_back(std::move(it->complete));
            it = m_quiet.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto& cb : ready)
    {
        cb(m_stats.failed == 0);
    }
}

void
RdmaSubmissionModel::NotifyFailure(uint64_t id)
{
    auto it = m_work.find(id);
    if (it != m_work.end() && !it->second.failed)
    {
        it->second.failed = true;
        ++m_stats.failed;
    }
    // Failed WRs stay outstanding: neither quiet nor a successful completion
    // is manufactured for a permanently lost transfer.
}

void
RdmaSubmissionModel::DoDispose()
{
    m_disposed = true;
    for (auto& [key, qp] : m_qps)
    {
        if (qp.batchEvent.IsPending())
        {
            Simulator::Cancel(qp.batchEvent);
        }
    }
    for (auto& event : m_events)
    {
        if (event.IsPending())
        {
            Simulator::Cancel(event);
        }
    }
    m_events.clear();
    m_work.clear();
    m_qps.clear();
    m_quiet.clear();
    m_observer = {};
    m_sourceStagingWaiting.clear();
    m_receiveStagingWaiting.clear();
    Object::DoDispose();
}
} // namespace ns3
