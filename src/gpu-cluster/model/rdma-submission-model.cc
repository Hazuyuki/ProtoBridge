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
using U64Fields = std::vector<std::pair<std::string, uint64_t*>>;
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
            {"rdmaStagingBufferBytes", &c.stagingBufferBytes},
            {"rdmaMaxReadAtomic", &c.maxReadAtomic},
            {"rdmaResponderDepth", &c.responderDepth},
            {"rdmaReceiveQueueDepth", &c.receiveQueueDepth},
            {"rdmaRnrRetryLimit", &c.rnrRetryLimit},
            {"rdmaImmediate", &c.immediate}};
}

U64Fields
U64Options(RdmaConfig& c)
{
    return {{"rdmaOperand", &c.operand},
            {"rdmaCompare", &c.compare},
            {"rdmaInitialValue", &c.initialValue},
            {"rdmaSignalValue", &c.signalValue},
            {"rdmaLocalAddress", &c.localAddress},
            {"rdmaRemoteAddress", &c.remoteAddress},
            {"rdmaSignalAddress", &c.signalAddress}};
}

TimeFields
TimeOptions(RdmaConfig& c)
{
    return {
        {"rdmaGpuWqeBuildNs", &c.wqeBuild},     {"rdmaCpuWqeBuildNs", &c.cpuWqeBuild},
        {"rdmaGpuPublishNs", &c.publish},       {"rdmaCpuPublishNs", &c.cpuPublish},
        {"rdmaGpuDoorbellNs", &c.doorbell},     {"rdmaCpuDoorbellNs", &c.cpuDoorbell},
        {"rdmaGpuPollNs", &c.gpuPoll},          {"rdmaCpuPollNs", &c.cpuPoll},
        {"rdmaCpuHandoffNs", &c.cpuHandoff},    {"rdmaBatchTimeoutNs", &c.batchTimeout},
        {"rdmaWqeFetchNs", &c.wqeFetch},        {"rdmaDmaReadNs", &c.dmaReadLatency},
        {"rdmaDmaWriteNs", &c.dmaWriteLatency}, {"rdmaCqWriteNs", &c.cqWrite},
        {"rdmaD2hNs", &c.d2hLatency},           {"rdmaH2dNs", &c.h2dLatency},
        {"rdmaReadServiceNs", &c.readService},  {"rdmaAtomicServiceNs", &c.atomicService},
        {"rdmaRnrRetryNs", &c.rnrRetry},        {"rdmaReceivePostNs", &c.receivePost},
        {"rdmaSignalPollNs", &c.signalPoll},    {"rdmaReceivePostDelayNs", &c.receivePostDelay}};
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
    cmd.AddValue("rdmaOperation", "Opcode for collective=rdma; see doc/RDMA.md", config.operation);
    cmd.AddValue("rdmaSignalOp", "Signal update: set or add", config.signalOperation);
    for (auto [name, dest] : U64Options(config))
    {
        cmd.AddValue(name, "RDMA operand/address", *dest);
    }
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
                                   "rdmaMapQpByProducer",
                                   "rdmaOperation",
                                   "rdmaSignalOp"};
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
    for (auto [name, dest] : U64Options(config))
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
    assign("rdmaOperation", config.operation);
    assign("rdmaSignalOp", config.signalOperation);
    for (auto [name, dest] : U64Options(config))
    {
        auto it = values.find(name);
        if (it == values.end())
        {
            continue;
        }
        try
        {
            size_t pos = 0;
            NS_ABORT_MSG_IF(it->second.empty() || it->second[0] == '-',
                            "Invalid RDMA integer: " << name);
            *dest = std::stoull(it->second, &pos);
            NS_ABORT_MSG_IF(pos != it->second.size(), "Invalid RDMA integer: " << name);
        }
        catch (const std::exception&)
        {
            NS_ABORT_MSG("Invalid RDMA integer: " << name);
        }
    }
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
    ParseRdmaOpcode(operation);
    NS_ABORT_MSG_IF(signalOperation != "set" && signalOperation != "add",
                    "rdmaSignalOp must be set or add");
    NS_ABORT_MSG_IF(maxReadAtomic > 65536 || responderDepth == 0 || responderDepth > 65536 ||
                        receiveQueueDepth == 0 || receiveQueueDepth > 1048576 ||
                        rnrRetryLimit > 65536,
                    "RDMA read/atomic/receive limits exceed supported bounds");
    NS_ABORT_MSG_IF(rnrRetry.IsZero(), "rdmaRnrRetryNs must be positive");
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
    for (Time t :
         {wqeBuild,        publish,     doorbell,   batchTimeout,    wqeFetch,    dmaReadLatency,
          dmaWriteLatency, cqWrite,     gpuPoll,    cpuWqeBuild,     cpuPublish,  cpuDoorbell,
          cpuPoll,         cpuHandoff,  d2hLatency, h2dLatency,      readService, atomicService,
          rnrRetry,        receivePost, signalPoll, receivePostDelay})
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

RdmaWorkTiming
RdmaSubmissionModel::GetWorkTiming(uint64_t id) const
{
    return m_work.at(id).time;
}

bool
RdmaSubmissionModel::RequestSucceeded(uint64_t id) const
{
    return *m_work.at(id).requestSucceeded;
}

void
RdmaSubmissionModel::SetObserver(Observer observer)
{
    m_observer = std::move(observer);
}

// Round 68 (codex finding 3): a large RELATIVE delay wraps inside
// DefaultSimulatorImpl::Schedule's `delay + TimeStep(now)` — Time::Max()
// at now >= 1 exceeds INT64_MAX. Clamp every relative delay to the safe
// horizon Time::Max() - now; the event then lands at Time::Max(), the
// latest timestamp the simulator can represent (a delay beyond it is the
// same modeling outcome as saturation: effectively never).
static Time
ClampRelativeDelay(Time delay)
{
    const Time horizon = Time::Max() - Simulator::Now();
    return delay > horizon ? horizon : delay;
}

void
RdmaSubmissionModel::Schedule(Time delay, std::function<void()> fn)
{
    m_events.erase(std::remove_if(m_events.begin(),
                                  m_events.end(),
                                  [](const EventId& e) { return e.IsExpired(); }),
                   m_events.end());
    // Round 69 (codex finding 1): clamp inside the common wrapper so no
    // call site can pass a saturated delay that wraps in Schedule's
    // `delay + TimeStep(now)` (a saturated Time::Max() delay at now >= 1
    // exceeds INT64_MAX). The clamped event lands at Time::Max().
    m_events.push_back(Simulator::Schedule(ClampRelativeDelay(delay), [this, fn = std::move(fn)] {
        if (!m_disposed)
        {
            fn();
        }
    }));
}

// Saturated Time addition (round 67, codex finding 4): the Reserve()
// ledger can pin `free` at Time::Max() (a degenerate configured rate with
// a multi-byte WQE: wqeBytes=64, pcieReadGbps=2^-54 saturates the service
// seconds pre-guard), and adding a positive configured DMA latency to it
// overflows the signed Time storage. Saturate against the tick storage and
// construct the saturated result via Time::Max() — never Time(int64) or
// Seconds() on a large value (see the SaturatedAdd comment in
// fabric-endpoint.cc for the full reasoning; the same constraints apply).
static Time
SaturatedAddTime(Time a, Time b)
{
    if (a <= Time(0) || b <= Time(0))
    {
        return (a <= Time(0)) ? b : a;
    }
    const int64_t aTicks = a.GetTimeStep();
    const int64_t bTicks = b.GetTimeStep();
    if (bTicks > std::numeric_limits<int64_t>::max() - aTicks)
    {
        return Time::Max();
    }
    return Time(aTicks + bTicks);
}

// (the Schedule-wrapper clamp lives above with the wrapper itself).

Time
RdmaSubmissionModel::Reserve(uint64_t bytes, double gbps, Time& free, Time earliest)
{
    Time start = std::max(earliest, free);
    // Round 66 (codex finding 7): an individually oversized service
    // overflows Time's signed ns storage inside Seconds() (the checked
    // multiplication asserts, and under floating-point diagnostics the
    // conversion is undefined) BEFORE the ledger saturation below can run:
    // wqeBytes=125000000 at pcieReadGbps=2^-34 already exceeds INT64_MAX
    // ns. Saturate the service at Time::Max().
    Time service(0);
    if (gbps > 0)
    {
        // Round 71 (codex findings 1-3): the round-70 resolution switch
        // still multiplied before bounding, double-rounded against ns-3's
        // int64x64 conversion, and silently absorbed Y/D/H/MIN into the FS
        // branch:
        //   - FS multiply overflow (finding 1): the bound check ran on the
        //     PRODUCT, so `sec * 1e15` overflowed to +inf (FE_OVERFLOW,
        //     SIGFPE under traps) before the guard could fire for finite
        //     sec as small as ~5.12e293 (probe: 64 B @ 1e-300 gbps, FS ->
        //     sec * 1e15 = inf). The pre-multiply bound must compare sec
        //     itself against the largest representable service.
        //   - Double-rounding mismatch (finding 2): `llround(sec * tps)`
        //     rounds the binary64 product, not the real value; ns-3's
        //     Seconds() converts through 64.64 fixed point and rounds
        //     that, so the two can disagree by one tick (probe at NS:
        //     64 B @ 204.8 Gbps -> double path gives llround(2.5) = 3,
        //     Seconds() gives 2; exact fixed-point value 2.499999...).
        //     Fixed by delegating every in-range service to Seconds().
        //   - Resolution fall-through (finding 3): the round-70 switch
        //     handled S/MS/US/NS/PS and let default take FS — but ns-3's
        //     Time::Y/D/H/MIN also fall into default, where the FS scale
        //     was wrong (probe at MIN: 7.5e9 B @ 1 Gbps, sec = 60 s ->
        //     6e16 FS ticks where Seconds(60) = 1 tick of 60 s). All ten
        //     units are now explicit, with the MIN/H/D/Y ticks-per-second
        //     derived from ns-3's Scale(): MIN 1/60, H 1/3600, D 1/86400,
        //     Y 1/31536000.
        // Round 70's history (codex findings 2-5 of that round) is kept
        // verbatim below for the record; note that its per-resolution
        // numbers were corrected in round 71 (finding 4 of that round):
        //   - S/MS: GetNanoSeconds() wraps int64 (I * 10^k overflows), so
        //     the round-69 denominator was negative and an ordinary
        //     service became a huge NEGATIVE Time (probe: 32 B @ 400 Gbps
        //     at MS -> -5.9e24 ticks);
        //   - US: GetNanoSeconds() at US wraps to -1000 (native probe of
        //     Time::Max().GetNanoSeconds(), not INT64_MAX as round 70
        //     claimed), so the round-69 tps projection was meaningless;
        //   - NS/PS/FS: the integer division inside GetNanoSeconds()
        //     truncates the tick count first, so the scale factor was
        //     wrong (native probe at PS: 1000 B @ 8 Gbps with the
        //     round-69 formula -> 1000000 ticks, i.e. 1 us of service
        //     where the true value is 1 ns... the formula mixed tick
        //     scales; Seconds(1e-6 s) at PS = 10^6 ticks);
        //   - the round-69 guard `ticksNeeded > (double)INT64_MAX` used
        //     strict > while binary64((double)INT64_MAX) == 2^63 exactly,
        //     so a service whose product rounds to exactly 2^63 slipped
        //     through (probe: 64 B at gbps ~= 5.55e-14 -> sec*1e12 == 2^63
        //     exactly at PS; at NS the same input gives 9.223372036854776e+18
        //     < 2^63);
        //   - the unguarded `sec * 2^63` multiply overflows to +inf for
        //     finite sec as small as ~5.12e293 (probe: 64 B @ 1e-300).
        // Wall-clock horizons of Time::Max() for reference (native
        // probes): PS = 9223372.036854776 s, FS = 9223.3720368547765 s.
        //
        // Current conversion: derive ticks-per-second from the resolution
        // (1 tick = 10^(9-k) ns at 10^-k s resolution; MIN/H/D/Y from
        // ns-3's Scale() coefficients), compare sec against the largest
        // in-range service BEFORE any multiply (no overflowable
        // multiply), saturate at the inclusive 2^63 bound, and delegate
        // every in-range service to Seconds() so the tick count matches
        // ns-3's int64x64 rounding exactly.
        // Round 72 (codex finding 1): finite gbps can still overflow the
        // multiply below (gbps = 1e300 is accepted by the isfinite check but
        // gbps * 1e9 = inf, raising FE_OVERFLOW -> SIGFPE under fenv traps
        // before any guard sees it). Dividing by 1e9 first makes the
        // expression overflow-free for every finite gbps: bitRatio <=
        // 0x1p+64, 1e9 is exact, so bitRatio / 1e9 <= 0x1p+38.
        // Round 73 (codex finding 1): the reassociated division still
        // overflows binary64 when the ROUNDED intermediate bitRatio pushes
        // the quotient past DBL_MAX even though the exact quotient is
        // representable (bytes = 500000002, gbps = 0x1.000000112e0bfp-1022:
        // exact sec = DBL_MAX*(1+2.1e-18) is representable, but bitRatio's
        // ~2^-29 relative rounding makes the binary64 quotient exceed
        // DBL_MAX*(1+2^-53) -> FE_OVERFLOW -> SIGFPE under fenv traps
        // before the bound check runs). Pre-divide the saturation
        // threshold into gbps units and skip the divide entirely when the
        // rate is at or below it (gbps <= 0 is validated above, so the
        // branch covers zero, subnormal, and small-enough-that-the-exact-
        // sec-exceeds-every-representable-bound rates); only an in-range
        // gbps reaches `bitRatio / gbps`, which then cannot overflow.
        const double bitRatio = static_cast<double>(bytes) * 8 / 1e9;
        double ticksPerSecond;
        switch (Time::GetResolution())
        {
        case Time::S:
            ticksPerSecond = 1;
            break;
        case Time::MS:
            ticksPerSecond = 1e3;
            break;
        case Time::US:
            ticksPerSecond = 1e6;
            break;
        case Time::NS:
            ticksPerSecond = 1e9;
            break;
        case Time::PS:
            ticksPerSecond = 1e12;
            break;
        case Time::FS:
            ticksPerSecond = 1e15;
            break;
        case Time::MIN:
            ticksPerSecond = 1.0 / 60;
            break;
        case Time::H:
            ticksPerSecond = 1.0 / 3600;
            break;
        case Time::D:
            ticksPerSecond = 1.0 / 86400;
            break;
        case Time::Y:
            ticksPerSecond = 1.0 / 31536000;
            break;
        default: // Time::LAST and any future unit added by ns-3
            ticksPerSecond = 1;
            break;
        }
        // Inclusive pre-multiply bound: the largest finite sec whose
        // Seconds() conversion stays below 2^63 ticks. The margin of one
        // ULP at the bound absorbs double-rounding of the division that
        // produced sec, so Seconds() can never reach the checked 2^63
        // multiply inside ns-3. Everything at or beyond the bound is a
        // degenerate service and pins the ledger at Time::Max().
        const double saturateGbps =
            bitRatio / ((0x1p63 - 0x1p11) / ticksPerSecond);
        if (!(gbps > saturateGbps))
        {
            // gbps <= 0 (validated above), subnormal, or small enough
            // that the exact sec exceeds every representable bound:
            // saturate without performing the overflowing divide.
            service = Time::Max();
        }
        else
        {
            const double sec = bitRatio / gbps;
            // Round 72 (codex finding 1): at coarse resolutions (MIN/H/D/Y)
            // Seconds() can return a NEGATIVE Time for a large finite sec.
            // ns-3's int64x64(double) builds hi = (int64)sec by C++
            // float-to-int conversion; that conversion is out of range for
            // sec >= 2^63, so hi truncates to INT64_MIN, and
            // lo = (uint64)((sec - hi) * 2^64) is then built from that
            // garbage hi. Multiplying by Scale(MIN) = 6e16
            // (INT64X64_USE_128 build) computes lo * 6e16 in signed
            // __int128: negative x positive wraps to a large positive
            // 128-bit value whose low 64 bits are a NEGATIVE tick count,
            // so the checked 2^63 multiply passes and Round() emits
            // negative ticks (UBSan probe: MIN 64 B @
            // gbps = 5.551115123125783e-26 -> service = -153722867280912930
            // ticks, then the ledger subtraction
            // INT64_MAX - serviceTicks overflows signed int64). The clamp
            // below pins a negative service at Time::Max() as well: a
            // degenerate rate should pin the ledger at the far-future
            // bound, not wrap it past Time(0). The bound already compares
            // sec itself, so the clamp only fires for the coarse-unit
            // Seconds() window 2^63 s <= sec < bound (at MIN that window
            // spans [2^63, 60*(2^63-2^11)) s; at S/MS/US/NS/PS/FS the
            // window is empty because Seconds() asserts before 2^63).
            service = (sec >= (0x1p63 - 0x1p11) / ticksPerSecond)
                          ? Time::Max()
                          : Seconds(sec);
            if (service.IsStrictlyNegative())
            {
                // Zero is valid when a small memory access rounds below one tick.
                service = Time::Max();
            }
        }
    }
    // Round 65 (codex finding 8): saturate the free ledger instead of
    // wrapping. The saturating bound is a no-op for every realistic service
    // (start + service wrapping needs ~292 years of queued PCIe service at
    // pcieReadGbps=2^-32), but a degenerate gbps pins the ledger at the
    // far-future bound below instead of wrapping it past Time(0).
    // wqeBytes=125000000 @ pcieReadGbps=2^-32 is the probe.
    // Round 68 (codex finding 1): the ledger must be saturated in RAW TICKS,
    // not nanoseconds. The round-65/66 ns-based ledger was correct only at
    // the NS default resolution: GetNanoSeconds() divides by 10^3 per
    // coarser step, so at PS a 1 us service (10^6 ticks) collapsed to
    // 10^3 ns and free was reconstructed 10^3x too small
    // (ReserveSourceRead(1000) @ pcieReadGbps=8 returned 1000 ticks
    // instead of 10^6 — a negative Publish delay). The tick form is exact
    // at every resolution; GetNanoSeconds() itself overflows at MS and
    // coarser (INT64_MAX ticks x 10^6), which the tick form never touches.
    const int64_t startTicks = start.GetTimeStep();
    const int64_t serviceTicks = service.GetTimeStep();
    free = Time(std::numeric_limits<int64_t>::max() - serviceTicks < startTicks
                    ? std::numeric_limits<int64_t>::max()
                    : startTicks + serviceTicks);
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
    // Round 67 (codex finding 4): saturate the composed latency — `free` may
    // be pinned at Time::Max() (see Reserve() above), and the configured
    // dmaReadLatency addition would overflow the signed Time storage.
    return SaturatedAddTime(std::max(memory, pcie), m_config.dmaReadLatency) - now;
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
    // Round 67 (codex finding 4): saturate — see ReserveSourceRead above.
    return SaturatedAddTime(std::max(memory, pcie), m_config.dmaWriteLatency) - now;
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
    // Round 67 (codex finding 4): saturate — see ReserveSourceRead above.
    return SaturatedAddTime(std::max({hbm, host, copy}), m_config.d2hLatency) - now;
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
    // Round 67 (codex finding 4): saturate — see ReserveSourceRead above.
    return SaturatedAddTime(std::max({host, hbm, copy}), m_config.h2dLatency) - now;
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
RdmaSubmissionModel::StageSourcePayload(uint64_t bytes, std::function<void()> ready)
{
    NS_ABORT_MSG_IF(bytes == 0 || bytes > m_config.stagingBufferBytes,
                    "Responder staging buffer is too small");
    m_sourceStagingWaiting.push_back({0, bytes, std::move(ready)});
    PumpSourceStaging();
}

void
RdmaSubmissionModel::ReleaseSourceStaging(uint64_t bytes)
{
    NS_ABORT_MSG_IF(bytes > m_sourceStagingUsed, "Source staging buffer accounting underflow");
    m_sourceStagingUsed -= bytes;
    PumpSourceStaging();
}

void
RdmaSubmissionModel::PumpSourceStaging()
{
    while (!m_sourceStagingWaiting.empty())
    {
        auto pending = m_sourceStagingWaiting.front();
        if (pending.bytes > m_config.stagingBufferBytes - m_sourceStagingUsed)
        {
            break;
        }
        m_sourceStagingWaiting.pop_front();
        m_sourceStagingUsed += pending.bytes;
        m_stats.sourceStagingPeakBytes =
            std::max(m_stats.sourceStagingPeakBytes, m_sourceStagingUsed);
        if (pending.id)
        {
            auto& work = m_work.at(pending.id);
            work.stagingAllocated = true;
            work.time.stagingAcquired = Simulator::Now();
        }
        Schedule(ReserveSourceD2h(pending.bytes), [this, pending] {
            if (pending.id)
            {
                auto& work = m_work.at(pending.id);
                work.time.sourceStaged = Simulator::Now();
                work.time.submitted = Simulator::Now();
                QpKey key{work.time.peer, work.time.qp};
                auto& waiting = m_qps[key].waiting;
                waiting.insert(std::upper_bound(waiting.begin(), waiting.end(), pending.id),
                               pending.id);
                Pump(key);
            }
            else
            {
                pending.ready();
            }
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
    return SubmitOperation(RdmaOpcode::WRITE,
                           peer,
                           bytes,
                           std::move(execute),
                           producer,
                           std::move(complete));
}

uint64_t
RdmaSubmissionModel::SubmitOperation(RdmaOpcode opcode,
                                     uint16_t peer,
                                     uint64_t bytes,
                                     Execute execute,
                                     uint32_t producer,
                                     Completion complete)
{
    NS_ABORT_MSG_IF(m_disposed || !execute || bytes == 0,
                    "RDMA needs live model, nonzero bytes and execution");
    NS_ABORT_MSG_IF((RdmaUsesReadAtomicCredit(opcode) || opcode == RdmaOpcode::PUT_SIGNAL) &&
                        m_config.maxReadAtomic == 0,
                    "RDMA Read/atomic operations require rdmaMaxReadAtomic > 0");
    if (m_producerFree.empty())
    {
        Configure(m_config);
    }
    NS_ABORT_MSG_IF(producer != UINT32_MAX && producer >= m_config.ProducerCount(),
                    "RDMA producer ID out of range");
    bool split = m_config.dataPath == "staged" &&
                 (RdmaHasSourcePayload(opcode) || opcode == RdmaOpcode::READ);
    uint64_t dataChunks = split ? 1 + (bytes - 1) / m_config.stagingChunkBytes : 1;
    // Round 64 (codex finding 10): guard the count addition BEFORE it
    // wraps — PUT_SIGNAL with bytes=UINT64_MAX and 1-byte staging chunks
    // produced dataChunks=UINT64_MAX and the +1 wrapped count to zero,
    // emitting no work items so the completion callback never fired.
    NS_ABORT_MSG_IF(dataChunks >
                        UINT64_MAX - (opcode == RdmaOpcode::PUT_SIGNAL ? 1 : 0),
                    "RDMA staged work-item count overflows uint64");
    uint64_t count = dataChunks + (opcode == RdmaOpcode::PUT_SIGNAL ? 1 : 0);
    NS_ABORT_MSG_IF(count > UINT64_MAX - m_nextId, "RDMA work ID exhausted");
    uint64_t requestId = m_nextId;
    ++m_stats.requests;
    auto remaining = std::make_shared<uint64_t>(count);
    auto succeeded = std::make_shared<bool>(true), reachedWire = std::make_shared<bool>(false);
    auto finish = [this, remaining, succeeded, complete](bool ok) {
        *succeeded = *succeeded && ok;
        if (--*remaining == 0)
        {
            if (*succeeded)
            {
                ++m_stats.completedRequests;
            }
            else
            {
                ++m_stats.failedRequests;
            }
            if (complete)
            {
                complete(*succeeded);
            }
        }
    };
    auto enqueue = [this](uint64_t id) {
        auto& w = m_work.at(id);
        w.time.submitted = Simulator::Now();
        if (m_config.dataPath == "staged" && RdmaHasSourcePayload(w.time.opcode))
        {
            m_sourceStagingWaiting.push_back({id, w.time.bytes, {}});
        }
        else
        {
            QpKey key{w.time.peer, w.time.qp};
            auto& waiting = m_qps[key].waiting;
            waiting.insert(std::upper_bound(waiting.begin(), waiting.end(), id), id);
            Pump(key);
        }
    };
    std::vector<uint64_t> ids;
    uint64_t offset = 0;
    for (uint64_t index = 0; index < count; ++index)
    {
        bool signal = index == dataChunks;
        uint64_t length =
            signal
                ? 8
                : (split ? std::min<uint64_t>(m_config.stagingChunkBytes, bytes - offset) : bytes);
        uint64_t id = m_nextId++;
        uint32_t selected = producer == UINT32_MAX ? (id - 1) % m_config.ProducerCount() : producer;
        uint32_t qp =
            (m_config.mapQpByProducer ? selected : m_nextQp[peer]++) % m_config.qpsPerPeer;
        Work work;
        work.time.id = id;
        work.time.requestId = requestId;
        work.time.offset = offset;
        work.time.bytes = length;
        work.time.peer = peer;
        work.time.producer = selected;
        work.time.qp = qp;
        work.time.opcode = signal ? RdmaOpcode::SIGNAL : opcode;
        work.time.ready = Simulator::Now();
        work.time.submitted = Simulator::Now();
        work.execute = execute;
        work.complete = finish;
        work.requestReachedWire = reachedWire;
        work.requestSucceeded = succeeded;
        auto fence = m_fences.find(peer);
        if (fence != m_fences.end())
        {
            work.fenceThrough = fence->second;
        }
        // An unsent payload DMA must not be overtaken by a later Read/atomic
        // request on its RC SQ. Waiting for visibility is conservative.
        if (RdmaUsesReadAtomicCredit(work.time.opcode))
        {
            for (const auto& [earlierId, earlier] : m_work)
            {
                if (earlier.time.peer == peer && earlier.time.qp == qp &&
                    RdmaHasSourcePayload(earlier.time.opcode))
                {
                    work.fenceThrough = std::max(work.fenceThrough, earlierId);
                }
            }
        }
        if (signal)
        {
            work.fenceThrough = id - 1; // all payload chunks must be remotely visible first
        }
        m_work.emplace(id, std::move(work));
        ++m_stats.submitted;
        ids.push_back(id);
        if (!signal)
        {
            offset += length;
        }
    }
    Time handoff = (m_config.initiator == "cpu" && m_config.dataPath != "host")
                       ? m_config.cpuHandoff
                       : Time(0);
    auto start = [this, ids, enqueue] {
        for (auto id : ids)
        {
            enqueue(id);
        }
        PumpSourceStaging();
    };
    if (handoff.IsZero())
    {
        start();
    }
    else
    {
        Schedule(handoff, std::move(start));
    }
    return requestId;
}

void
RdmaSubmissionModel::NotifyOperationComplete(uint64_t id, bool success)
{
    auto it = m_work.find(id);
    if (it == m_work.end() || it->second.acked)
    {
        return;
    }
    auto& work = it->second;
    if (!success && !work.failed)
    {
        work.failed = true;
        ++m_stats.failed;
        *work.requestSucceeded = false;
    }
    work.acknowledgedBytes = work.time.bytes;
    work.acked = true;
    work.time.acknowledged = Simulator::Now();
    Progress({work.time.peer, work.time.qp});
    for (auto& [key, qp] : m_qps)
    {
        Pump(key);
    }
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
        if (RdmaUsesReadAtomicCredit(w.time.opcode) &&
            qp.readAtomicOccupied >= m_config.maxReadAtomic)
        {
            break;
        }
        if (BlockedByFence(w))
        {
            break;
        }
        qp.waiting.pop_front();
        ++qp.occupied;
        if (RdmaUsesReadAtomicCredit(w.time.opcode))
        {
            ++qp.readAtomicOccupied;
            m_stats.peakReadAtomic =
                std::max<uint64_t>(m_stats.peakReadAtomic, qp.readAtomicOccupied);
        }
        qp.outstanding.push_back(id);
        w.time.reserved = Simulator::Now();
        Time buildStart = std::max(Simulator::Now(), m_producerFree[w.time.producer]);
        // Round 68 (codex finding 2): saturate the producer/publish/doorbell
        // ledgers — wqeBuild=INT64_MAX (publish=1) wrapped buildEnd past
        // INT64_MAX and Schedule() aborted on the negative delay.
        Time buildEnd =
            SaturatedAddTime(buildStart,
                             m_config.initiator == "cpu" ? m_config.cpuWqeBuild
                                                         : m_config.wqeBuild);
        m_producerFree[w.time.producer] = buildEnd;
        // A NIC sees only the contiguous published SQ prefix, even when different
        // producers build different WQEs in parallel.
        Time publishEnd = SaturatedAddTime(
            std::max(buildEnd, qp.publishFree),
            m_config.initiator == "cpu" ? m_config.cpuPublish : m_config.publish);
        qp.publishFree = publishEnd;
        // Round 68 (codex finding 3): publishEnd can be the saturated
        // Time::Max(); the relative form exceeds INT64_MAX and wraps
        // inside DefaultSimulatorImpl::Schedule — clamp to the safe
        // horizon (lands at Time::Max(), never executes anyway).
        Schedule(ClampRelativeDelay(publishEnd - Simulator::Now()),
                 [this, key, id] { Publish(key, id); });
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
        qp.batchEvent =
            Simulator::Schedule(ClampRelativeDelay(m_config.batchTimeout), [this, key] {
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
    // Round 68 (codex finding 2): saturate the doorbell ledger (two WQEs
    // with doorbell=2^62 ticks both executed at 2^62; the second must
    // saturate at INT64_MAX, not wrap).
    Time ringEnd = SaturatedAddTime(
        std::max(Simulator::Now(), m_doorbellFree),
        m_config.initiator == "cpu" ? m_config.cpuDoorbell : m_config.doorbell);
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
        // Round 68 (codex finding 2): saturate in ticks — GetNanoSeconds()
        // divides by 10^3 per coarser resolution step, so at PS the fetch
        // ledger collapsed 10^3x (same defect as Reserve(), see above);
        // GetNanoSeconds() also overflows at MS and coarser.
        const int64_t fetchedTicks = fetched.GetTimeStep();
        const int64_t fetchTicks = m_config.wqeFetch.GetTimeStep();
        fetched = Time(fetchTicks > std::numeric_limits<int64_t>::max() - fetchedTicks
                           ? std::numeric_limits<int64_t>::max()
                           : fetchedTicks + fetchTicks);
        qp.fetchFree = fetched;
        // Round 68 (codex finding 3): fetched can be the saturated
        // Time::Max() — clamp the relative delay (see Pump above).
        Schedule(ClampRelativeDelay(fetched - Simulator::Now()),
                 [this, id] { ExecuteWork(id); });
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
    // Round 68 (codex finding 2): saturate — cqWrite=INT64_MAX (gpuPoll=1)
    // wrapped the relative delay past INT64_MAX and Schedule() aborted.
    Schedule(SaturatedAddTime(m_config.cqWrite,
                              m_config.initiator == "cpu" ? m_config.cpuPoll
                                                          : m_config.gpuPoll),
             [this, key, through] { Observe(key, through); });
}

void
RdmaSubmissionModel::Observe(const QpKey& key, uint64_t through)
{
    auto& qp = m_qps.at(key);
    std::vector<std::pair<Completion, bool>> callbacks;
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
            callbacks.emplace_back(std::move(w.complete), !w.failed);
        }
        --qp.occupied;
        if (RdmaUsesReadAtomicCredit(w.time.opcode))
        {
            --qp.readAtomicOccupied;
        }
        if (!w.failed)
        {
            ++m_stats.completed;
        }
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
    for (auto& [cb, ok] : callbacks)
    {
        cb(ok);
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
        *it->second.requestSucceeded = false;
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
