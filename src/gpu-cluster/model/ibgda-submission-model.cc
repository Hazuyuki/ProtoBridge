/* SPDX-License-Identifier: GPL-2.0-only */
#include "ibgda-submission-model.h"
#include "ns3/abort.h"
#include "ns3/simulator.h"
#include "ns3/command-line.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace ns3
{
NS_OBJECT_ENSURE_REGISTERED(IbgdaSubmissionModel);
NS_OBJECT_ENSURE_REGISTERED(IbgdaPacketTag);

namespace
{
using UintFields = std::vector<std::pair<std::string,uint32_t*>>;
using TimeFields = std::vector<std::pair<std::string,Time*>>;
using RateFields = std::vector<std::pair<std::string,double*>>;
UintFields UintOptions(IbgdaConfig& c)
{
    return {{"ibgdaProducers",&c.producers},{"ibgdaQpsPerPeer",&c.qpsPerPeer},
        {"ibgdaQueueDepth",&c.queueDepth},{"ibgdaDoorbellBatch",&c.doorbellBatch},
        {"ibgdaCqModeration",&c.cqModeration},{"ibgdaMtu",&c.mtuBytes},
        {"ibgdaWqeBytes",&c.wqeBytes}};
}
TimeFields TimeOptions(IbgdaConfig& c)
{
    return {{"ibgdaWqeBuildNs",&c.wqeBuild},{"ibgdaPublishNs",&c.publish},
        {"ibgdaDoorbellNs",&c.doorbell},{"ibgdaBatchTimeoutNs",&c.batchTimeout},
        {"ibgdaWqeFetchNs",&c.wqeFetch},{"ibgdaDmaReadNs",&c.dmaReadLatency},
        {"ibgdaDmaWriteNs",&c.dmaWriteLatency},{"ibgdaCqWriteNs",&c.cqWrite},
        {"ibgdaGpuPollNs",&c.gpuPoll}};
}
RateFields RateOptions(IbgdaConfig& c)
{
    return {{"ibgdaPcieReadGbps",&c.pcieReadGbps},{"ibgdaPcieWriteGbps",&c.pcieWriteGbps},
        {"ibgdaHbmReadGbps",&c.hbmReadGbps},{"ibgdaHbmWriteGbps",&c.hbmWriteGbps}};
}
bool ParseNanoseconds(Time* dest, const std::string& value)
{
    try
    {
        size_t pos=0; long long number=std::stoll(value,&pos);
        if (pos != value.size() || number < 0) return false;
        *dest=NanoSeconds(number);
        return true;
    }
    catch (const std::exception&) { return false; }
}
}
void AddIbgdaCommandLine(CommandLine& cmd, std::string& mode, IbgdaConfig& config)
{
    cmd.AddValue("rdmaSubmission","RDMA submission: startup (legacy default) or ibgda",mode);
    for (auto [name,dest] : UintOptions(config)) cmd.AddValue(name,"IBGDA count/size; see doc/IBGDA.md",*dest);
    for (auto [name,dest] : TimeOptions(config))
        cmd.AddValue(name,"IBGDA time in integer ns (0=no added latency)",MakeBoundCallback(&ParseNanoseconds,dest));
    for (auto [name,dest] : RateOptions(config))
        cmd.AddValue(name,"IBGDA bandwidth in Gb/s (0=unconstrained)",*dest);
    cmd.AddValue("ibgdaMapQpByProducer","1=producer-to-QP mapping; 0=round-robin work requests",config.mapQpByProducer);
}
void ApplyIbgdaProfile(const std::unordered_map<std::string,std::string>& values,
                       std::string& mode, IbgdaConfig& config)
{
    std::vector<std::string> known{"ibgdaMapQpByProducer"};
    for (auto [name,dest] : UintOptions(config)) known.push_back(name);
    for (auto [name,dest] : TimeOptions(config)) known.push_back(name);
    for (auto [name,dest] : RateOptions(config)) known.push_back(name);
    for (const auto& [name,value] : values)
        NS_ABORT_MSG_IF(name.rfind("ibgda",0) == 0 &&
            std::find(known.begin(),known.end(),name) == known.end(),
            "Unknown IBGDA profile option: " << name);
    auto found=values.find("rdmaSubmission");
    if (found != values.end()) mode=found->second;
    for (auto [name,dest] : UintOptions(config))
    {
        auto it=values.find(name);
        if (it == values.end()) continue;
        try
        {
            size_t pos=0; auto number=std::stoull(it->second,&pos);
            NS_ABORT_MSG_IF(pos != it->second.size() || number > UINT32_MAX,
                            "Invalid IBGDA integer: " << name);
            *dest=static_cast<uint32_t>(number);
        }
        catch (const std::exception&) { NS_ABORT_MSG("Invalid IBGDA integer: " << name); }
    }
    for (auto [name,dest] : TimeOptions(config))
    {
        auto it=values.find(name);
        NS_ABORT_MSG_IF(it != values.end() && !ParseNanoseconds(dest,it->second),
                        "Invalid IBGDA nanoseconds: " << name);
    }
    for (auto [name,dest] : RateOptions(config))
    {
        auto it=values.find(name);
        if (it == values.end()) continue;
        try
        {
            size_t pos=0; *dest=std::stod(it->second,&pos);
            NS_ABORT_MSG_IF(pos != it->second.size(),"Invalid IBGDA rate: " << name);
        }
        catch (const std::exception&) { NS_ABORT_MSG("Invalid IBGDA rate: " << name); }
    }
    found=values.find("ibgdaMapQpByProducer");
    if (found != values.end())
    {
        NS_ABORT_MSG_IF(found->second != "0" && found->second != "1",
                        "ibgdaMapQpByProducer must be 0 or 1");
        config.mapQpByProducer=found->second == "1";
    }
    NS_ABORT_MSG_IF(mode != "startup" && mode != "ibgda",
                    "rdmaSubmission must be startup or ibgda");
    if (mode == "ibgda") config.Validate();
}

void IbgdaConfig::Validate() const
{
    NS_ABORT_MSG_IF(producers == 0 || qpsPerPeer == 0 || queueDepth == 0,
                    "IBGDA producers, QPs and queue depth must be positive");
    NS_ABORT_MSG_IF(producers > 65536 || qpsPerPeer > 65536 || queueDepth > 1048576,
                    "IBGDA counts exceed supported producer/QP/queue bounds");
    NS_ABORT_MSG_IF(doorbellBatch == 0 || doorbellBatch > queueDepth ||
                    cqModeration == 0 || cqModeration > queueDepth,
                    "IBGDA doorbell batch and CQ moderation must be in [1, queueDepth]");
    NS_ABORT_MSG_IF(mtuBytes == 0 || mtuBytes > (1u << 20) || wqeBytes == 0,
                    "IBGDA needs MTU in [1,1048576] B and positive WQE bytes");
    for (Time t : {wqeBuild,publish,doorbell,batchTimeout,wqeFetch,
                   dmaReadLatency,dmaWriteLatency,cqWrite,gpuPoll})
    {
        NS_ABORT_MSG_IF(t.IsStrictlyNegative(), "IBGDA timings must be nonnegative");
    }
    NS_ABORT_MSG_IF(doorbellBatch > 1 && batchTimeout.IsZero(),
                    "IBGDA batching needs a positive timeout to flush partial batches");
    for (double bw : {pcieReadGbps,pcieWriteGbps,hbmReadGbps,hbmWriteGbps})
    {
        NS_ABORT_MSG_IF(!std::isfinite(bw) || bw < 0,
                        "IBGDA bandwidth must be finite and nonnegative (0=unconstrained)");
    }
}

TypeId IbgdaPacketTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::IbgdaPacketTag").SetParent<Tag>()
        .SetGroupName("GpuCluster").AddConstructor<IbgdaPacketTag>();
    return tid;
}
TypeId IbgdaPacketTag::GetInstanceTypeId() const { return GetTypeId(); }
uint32_t IbgdaPacketTag::GetSerializedSize() const { return 14; }
void IbgdaPacketTag::Serialize(TagBuffer b) const
{ b.WriteU64(workId); b.WriteU32(qp); b.WriteU8(sourceReady); b.WriteU8(destinationReady); }
void IbgdaPacketTag::Deserialize(TagBuffer b)
{ workId=b.ReadU64(); qp=b.ReadU32(); sourceReady=b.ReadU8(); destinationReady=b.ReadU8(); }
void IbgdaPacketTag::Print(std::ostream& os) const { os << "work=" << workId; }

TypeId IbgdaSubmissionModel::GetTypeId()
{
    static TypeId tid = TypeId("ns3::IbgdaSubmissionModel").SetParent<Object>()
        .SetGroupName("GpuCluster").AddConstructor<IbgdaSubmissionModel>();
    return tid;
}
void IbgdaSubmissionModel::Configure(const IbgdaConfig& config)
{
    NS_ABORT_MSG_IF(m_stats.submitted != 0, "Configure IBGDA before submitting work");
    config.Validate();
    m_config=config;
    m_producerFree.assign(config.producers, Time(0));
}
const IbgdaConfig& IbgdaSubmissionModel::GetConfig() const { return m_config; }
const IbgdaStats& IbgdaSubmissionModel::GetStats() const { return m_stats; }
uint64_t IbgdaSubmissionModel::GetOutstanding() const { return m_work.size(); }
uint32_t IbgdaSubmissionModel::GetQpForWork(uint64_t id) const { return m_work.at(id).time.qp; }
void IbgdaSubmissionModel::SetObserver(Observer observer) { m_observer=std::move(observer); }

void IbgdaSubmissionModel::Schedule(Time delay, std::function<void()> fn)
{
    m_events.erase(std::remove_if(m_events.begin(),m_events.end(),
        [](const EventId& e){return e.IsExpired();}),m_events.end());
    m_events.push_back(Simulator::Schedule(delay, [this,fn=std::move(fn)] {
        if (!m_disposed) fn();
    }));
}
Time IbgdaSubmissionModel::Reserve(uint64_t bytes, double gbps, Time& free, Time earliest)
{
    Time start=std::max(earliest,free);
    Time service=gbps > 0 ? Seconds(static_cast<double>(bytes)*8/(gbps*1e9)) : Time(0);
    free=start+service;
    return free;
}
Time IbgdaSubmissionModel::ReserveSourceRead(uint64_t bytes)
{
    Time hbm=Reserve(bytes,m_config.hbmReadGbps,m_hbmReadFree,Simulator::Now());
    // HBM and PCIe service are independent pipeline constraints on this chunk.
    Time pcie=Reserve(bytes,m_config.pcieReadGbps,m_pcieReadFree,Simulator::Now());
    return std::max(hbm,pcie)+m_config.dmaReadLatency-Simulator::Now();
}
Time IbgdaSubmissionModel::ReserveDestinationWrite(uint64_t bytes)
{
    Time hbm=Reserve(bytes,m_config.hbmWriteGbps,m_hbmWriteFree,Simulator::Now());
    Time pcie=Reserve(bytes,m_config.pcieWriteGbps,m_pcieWriteFree,Simulator::Now());
    return std::max(hbm,pcie)+m_config.dmaWriteLatency-Simulator::Now();
}

uint64_t IbgdaSubmissionModel::Submit(uint16_t peer, uint64_t bytes, Execute execute,
                                      uint32_t producer, Completion complete)
{
    NS_ABORT_MSG_IF(m_disposed || !execute || bytes == 0,
                    "IBGDA needs a live model, nonzero bytes and an execute callback");
    if (m_producerFree.empty()) Configure(m_config);
    NS_ABORT_MSG_IF(m_nextId == std::numeric_limits<uint64_t>::max(), "IBGDA work ID exhausted");
    const uint64_t id=m_nextId++;
    if (producer == UINT32_MAX) producer=(id-1)%m_config.producers;
    NS_ABORT_MSG_IF(producer >= m_config.producers, "IBGDA producer ID out of range");
    uint32_t qp=(m_config.mapQpByProducer ? producer : m_nextQp[peer]++)%m_config.qpsPerPeer;
    Work w;
    w.time.id=id; w.time.bytes=bytes; w.time.peer=peer; w.time.producer=producer;
    w.time.qp=qp; w.time.ready=Simulator::Now();
    auto fence=m_fences.find(peer);
    if (fence != m_fences.end()) w.fenceThrough=fence->second;
    w.execute=std::move(execute); w.complete=std::move(complete);
    m_work.emplace(id,std::move(w));
    ++m_stats.submitted;
    QpKey key{peer,qp};
    m_qps[key].waiting.push_back(id);
    Pump(key);
    return id;
}
bool IbgdaSubmissionModel::BlockedByFence(const Work& work) const
{
    if (work.fenceThrough == 0) return false;
    for (const auto& [id,w] : m_work)
    {
        if (id > work.fenceThrough) break;
        if (w.time.peer == work.time.peer && !w.acked) return true;
    }
    return false;
}
void IbgdaSubmissionModel::Pump(const QpKey& key)
{
    auto& qp=m_qps.at(key);
    while (!qp.waiting.empty() && qp.occupied < m_config.queueDepth)
    {
        uint64_t id=qp.waiting.front();
        Work& w=m_work.at(id);
        if (BlockedByFence(w)) break;
        qp.waiting.pop_front();
        ++qp.occupied;
        qp.outstanding.push_back(id);
        w.time.reserved=Simulator::Now();
        Time buildStart=std::max(Simulator::Now(),m_producerFree[w.time.producer]);
        Time buildEnd=buildStart+m_config.wqeBuild;
        m_producerFree[w.time.producer]=buildEnd;
        // A NIC sees only the contiguous published SQ prefix, even when GPU
        // producers build different WQEs in parallel.
        Time publishEnd=std::max(buildEnd,qp.publishFree)+m_config.publish;
        qp.publishFree=publishEnd;
        Schedule(publishEnd-Simulator::Now(),[this,key,id]{Publish(key,id);});
        m_stats.peakQpOutstanding=std::max<uint64_t>(m_stats.peakQpOutstanding,qp.occupied);
        uint64_t occupied=0;
        for (const auto& [k,q] : m_qps) occupied+=q.occupied;
        m_stats.peakOutstanding=std::max(m_stats.peakOutstanding,occupied);
    }
}
void IbgdaSubmissionModel::Publish(const QpKey& key, uint64_t id)
{
    auto& qp=m_qps.at(key);
    m_work.at(id).time.published=Simulator::Now();
    qp.published.push_back(id);
    if (qp.published.size() >= m_config.doorbellBatch)
    {
        Ring(key);
    }
    else if (!qp.batchEvent.IsPending())
    {
        qp.batchEvent=Simulator::Schedule(m_config.batchTimeout,[this,key]{
            if (!m_disposed) Ring(key);
        });
    }
}
void IbgdaSubmissionModel::Ring(const QpKey& key)
{
    auto& qp=m_qps.at(key);
    if (qp.batchEvent.IsPending()) Simulator::Cancel(qp.batchEvent);
    if (qp.published.empty()) return;
    ++m_stats.doorbells;
    Time ringEnd=std::max(Simulator::Now(),m_doorbellFree)+m_config.doorbell;
    m_doorbellFree=ringEnd;
    while (!qp.published.empty())
    {
        uint64_t id=qp.published.front(); qp.published.pop_front();
        auto& w=m_work.at(id);
        w.time.doorbell=ringEnd;
        Time fetched=Reserve(m_config.wqeBytes,m_config.pcieReadGbps,m_pcieReadFree,
                             std::max(ringEnd,qp.fetchFree))+m_config.wqeFetch;
        qp.fetchFree=fetched;
        Schedule(fetched-Simulator::Now(),[this,id]{ExecuteWork(id);});
    }
}
void IbgdaSubmissionModel::ExecuteWork(uint64_t id)
{
    auto it=m_work.find(id);
    if (it == m_work.end()) return;
    it->second.time.execute=Simulator::Now();
    auto execute=std::move(it->second.execute);
    execute(id);
}
void IbgdaSubmissionModel::NotifyFirstWire(uint64_t id)
{
    auto it=m_work.find(id);
    if (it == m_work.end() || it->second.time.reachedWire) return;
    auto& t=it->second.time;
    t.firstWire=Simulator::Now(); t.reachedWire=true;
    uint64_t startup=(t.firstWire-t.ready).GetNanoSeconds();
    ++m_stats.startupSamples; m_stats.startupTotalNs+=startup;
    m_stats.startupMaxNs=std::max(m_stats.startupMaxNs,startup);
}
void IbgdaSubmissionModel::NotifyPacketAck(uint64_t id, uint64_t bytes)
{
    auto it=m_work.find(id);
    if (it == m_work.end() || it->second.acked) return;
    Work& w=it->second;
    NS_ABORT_MSG_IF(bytes > w.time.bytes-w.acknowledgedBytes, "IBGDA ACK exceeds WR byte ledger");
    w.acknowledgedBytes+=bytes;
    if (w.acknowledgedBytes != w.time.bytes) return;
    w.acked=true; w.time.acknowledged=Simulator::Now();
    QpKey key{w.time.peer,w.time.qp};
    Progress(key);
    for (auto& [k,q] : m_qps) Pump(k);
}
void IbgdaSubmissionModel::Progress(const QpKey& key)
{
    auto& qp=m_qps.at(key);
    uint64_t through=0; uint32_t ready=0;
    for (uint64_t id : qp.outstanding)
    {
        if (!m_work.at(id).acked) break;
        through=id; ++ready;
    }
    // The last completed WR is signaled even for a short tail. Reclaiming a
    // full queue must never depend on another WR being submitted first.
    if (ready == 0) return;
    if (ready < m_config.cqModeration && ready != qp.occupied) return;
    ++m_stats.cqes;
    // Remove this prefix from the NIC's completion ledger now; queue slots
    // stay occupied until the GPU has observed the CQ write.
    for (uint32_t i=0;i<ready;++i) qp.outstanding.pop_front();
    Schedule(m_config.cqWrite+m_config.gpuPoll,[this,key,through]{Observe(key,through);});
}
void IbgdaSubmissionModel::Observe(const QpKey& key, uint64_t through)
{
    auto& qp=m_qps.at(key);
    std::vector<Completion> callbacks;
    std::vector<IbgdaWorkTiming> timings;
    for (auto it=m_work.begin();it!=m_work.end();)
    {
        auto& w=it->second;
        if (it->first > through) break;
        if (w.time.peer != key.first || w.time.qp != key.second || !w.acked)
        { ++it; continue; }
        w.time.observed=Simulator::Now();
        timings.push_back(w.time);
        if (w.complete) callbacks.push_back(std::move(w.complete));
        --qp.occupied; ++m_stats.completed;
        it=m_work.erase(it);
    }
    Pump(key);
    Progress(key);
    // Observers may dispose a model. Do not keep references into its mutable
    // work map or invoke pending completion callbacks after disposal.
    auto observer=m_observer;
    if (observer)
        for (const auto& timing : timings)
        {
            observer(timing);
            if (m_disposed) return;
        }
    for (auto& cb : callbacks)
    {
        cb(true);
        if (m_disposed) return;
    }
    CheckQuiet();
}
void IbgdaSubmissionModel::Quiet(Completion complete)
{
    NS_ABORT_MSG_IF(!complete, "IBGDA quiet needs a callback");
    m_quiet.push_back({m_nextId-1,std::move(complete)});
    for (auto& [key,qp] : m_qps) Ring(key);
    CheckQuiet();
}
void IbgdaSubmissionModel::Fence(uint16_t peer)
{
    m_fences[peer]=m_nextId-1;
    for (auto& [key,qp] : m_qps) if (key.first == peer) Ring(key);
}
void IbgdaSubmissionModel::CheckQuiet()
{
    std::vector<Completion> ready;
    for (auto it=m_quiet.begin();it!=m_quiet.end();)
    {
        if (m_work.empty() || m_work.begin()->first > it->watermark)
        { ready.push_back(std::move(it->complete)); it=m_quiet.erase(it); }
        else ++it;
    }
    for (auto& cb : ready) cb(m_stats.failed == 0);
}
void IbgdaSubmissionModel::NotifyFailure(uint64_t id)
{
    auto it=m_work.find(id);
    if (it != m_work.end() && !it->second.failed)
    { it->second.failed=true; ++m_stats.failed; }
    // Failed WRs stay outstanding: neither quiet nor a successful completion
    // is manufactured for a permanently lost transfer.
}
void IbgdaSubmissionModel::DoDispose()
{
    m_disposed=true;
    for (auto& [key,qp] : m_qps)
        if (qp.batchEvent.IsPending()) Simulator::Cancel(qp.batchEvent);
    for (auto& event : m_events) if (event.IsPending()) Simulator::Cancel(event);
    m_events.clear(); m_work.clear(); m_qps.clear(); m_quiet.clear(); m_observer={};
    Object::DoDispose();
}
} // namespace ns3
