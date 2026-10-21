/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Fabric Endpoint Model Implementation
 */

#include "fabric-endpoint.h"

#include "nccl-protocol-model.h"
#include "nccl-protocol-payload-builder.h"
#include "roce-payload-builder.h"      // RocePayloadBuilder::GetTypeId (Seam B builder guard)
#include "roce-protocol-model.h" // RoceProtocol::SIMPLE
#include "so-header.h"            // SoHeader::PeekEcn

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/boolean.h"
#include "ns3/pointer.h"
#include "ns3/enum.h"

#include <algorithm>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("FabricEndpoint");

NS_OBJECT_ENSURE_REGISTERED(FabricEndpoint);

TypeId
FabricEndpoint::GetTypeId()
{
    static TypeId tid = TypeId("ns3::FabricEndpoint")
        .SetParent<Application>()
        .SetGroupName("GpuCluster")
        .AddConstructor<FabricEndpoint>()
        .AddAttribute("DeviceType",
                      "Type of this endpoint device (0=GPU, 1=CPU, 2=TPU, 3=MEMORY)",
                      UintegerValue(0),
                      MakeUintegerAccessor(&FabricEndpoint::m_deviceType),
                      MakeUintegerChecker<uint8_t>())
        .AddAttribute("FabricType",
                      "Fabric type (0=NVLink, 1=Ethernet, 2=Hybrid)",
                      UintegerValue(0),
                      MakeUintegerAccessor(&FabricEndpoint::m_fabricType),
                      MakeUintegerChecker<uint8_t>())
        .AddAttribute("Rank",
                      "Endpoint rank",
                      UintegerValue(0),
                      MakeUintegerAccessor(&FabricEndpoint::m_rank),
                      MakeUintegerChecker<uint16_t>())
        .AddAttribute("NumVirtualChannels",
                      "Number of virtual channels",
                      UintegerValue(1),
                      MakeUintegerAccessor(&FabricEndpoint::m_numVcs),
                      MakeUintegerChecker<uint8_t>())
        .AddAttribute("LaunchDelay",
                      "NCCL launch delay in nanoseconds",
                      UintegerValue(2000),
                      MakeUintegerAccessor(&FabricEndpoint::m_launchDelayNs),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("SprayingEnabled",
                      "Enable packet spraying across multiple links",
                      BooleanValue(false),
                      MakeBooleanAccessor(&FabricEndpoint::m_sprayingEnabled),
                      MakeBooleanChecker())
        .AddAttribute("SprayChunkSize",
                      "Chunk size for spraying in bytes (default 4KB)",
                      UintegerValue(4*1024),  // 4KB default
                      MakeUintegerAccessor(&FabricEndpoint::m_sprayChunkSize),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("CreditManager",
                      "Credit manager for flow control",
                      PointerValue(),
                      MakePointerAccessor(&FabricEndpoint::m_creditManager),
                      MakePointerChecker<CreditManager>())
        .AddAttribute("LinkDegradationModel",
                      "Link degradation model for this endpoint",
                      PointerValue(),
                      MakePointerAccessor(&FabricEndpoint::m_linkDegradationModel),
                      MakePointerChecker<LinkDegradationModel>())
        .AddAttribute("MemorySize",
                      "Memory size in bytes (for MEMORY device type)",
                      UintegerValue(0),
                      MakeUintegerAccessor(&FabricEndpoint::m_memorySize),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("MemoryLatency",
                      "Memory access latency (for MEMORY device type)",
                      TimeValue(NanoSeconds(100)),
                      MakeTimeAccessor(&FabricEndpoint::m_memoryLatency),
                      MakeTimeChecker())
        .AddAttribute("SyncMemLatencyNs",
                      "Sync load/store latency in nanoseconds (500 for NVLink, 200 for UB)",
                      UintegerValue(500),
                      MakeUintegerAccessor(&FabricEndpoint::GetSyncMemLatencyNs, &FabricEndpoint::SetSyncMemLatencyNs),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("AsyncMemLatencyNs",
                      "Async URMA latency in nanoseconds (1000 for NVLink, 2000 for UB)",
                      UintegerValue(1000),
                      MakeUintegerAccessor(&FabricEndpoint::GetAsyncMemLatencyNs, &FabricEndpoint::SetAsyncMemLatencyNs),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("LlrEnabled",
                      "Enable link-level retry (false for NVLink, true for UB)",
                      BooleanValue(false),
                      MakeBooleanAccessor(&FabricEndpoint::m_llrEnabled),
                      MakeBooleanChecker())
        .AddAttribute("LlrRetryLimit",
                      "Maximum retry count for LLR",
                      UintegerValue(3),
                      MakeUintegerAccessor(&FabricEndpoint::m_llrRetryLimit),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("FlowControlPolicy",
                      "Send-gate flow-control policy: credit (per-VC, implemented), "
                      "window or rate (seam only, admitted with a warning)",
                      EnumValue(FlowControlPolicy::CREDIT),
                      MakeEnumAccessor<FlowControlPolicy>(&FabricEndpoint::m_flowControlPolicy),
                      MakeEnumChecker(FlowControlPolicy::CREDIT, "credit",
                                      FlowControlPolicy::WINDOW, "window",
                                      FlowControlPolicy::RATE, "rate"))
        .AddAttribute("FecModel",
                      "Forward error correction model",
                      PointerValue(),
                      MakePointerAccessor(&FabricEndpoint::m_fecModel),
                      MakePointerChecker<FecModel>())
        .AddAttribute("LatencyStatistics",
                      "Per-flow latency statistics collector",
                      PointerValue(),
                      MakePointerAccessor(&FabricEndpoint::m_latencyStatistics),
                      MakePointerChecker<LatencyStatistics>())
        .AddAttribute("ContentionModel",
                      "WFQ contention model for bandwidth arbitration between traffic classes",
                      PointerValue(),
                      MakePointerAccessor(&FabricEndpoint::SetContentionModel,
                                          &FabricEndpoint::GetContentionModel),
                      MakePointerChecker<ContentionModel>())
        .AddAttribute("NumLanes",
                      "Number of physical lanes per logical link (1=no sub-lane spraying, 6=NVLink 6-lane)",
                      UintegerValue(1),
                      MakeUintegerAccessor(&FabricEndpoint::m_numLanes),
                      MakeUintegerChecker<uint32_t>(1, 64))
        .AddTraceSource("Tx",
                        "A packet is transmitted",
                        MakeTraceSourceAccessor(&FabricEndpoint::m_txTrace),
                        "ns3::TracedValueCallback::Uint32")
        .AddTraceSource("Rx",
                        "A packet is received",
                        MakeTraceSourceAccessor(&FabricEndpoint::m_rxTrace),
                        "ns3::TracedValueCallback::Uint32");
    return tid;
}

FabricEndpoint::FabricEndpoint()
    : m_deviceType(static_cast<uint8_t>(DeviceType::GPU)),
      m_fabricType(static_cast<uint8_t>(FabricType::NVLINK)),
      m_interNodeFabricType(static_cast<uint8_t>(FabricType::ROCE)),
      m_rank(0),
      m_nodeId(0),
      m_localRankBase(0),
      m_localRankCount(1),
      m_currentDeviceIndex(0),
      m_sprayingEnabled(false),
      m_sprayChunkSize(4*1024),  // 4KB default
      m_globalSprayOffset(0),
      m_numLanes(1),
      m_numVcs(1),
      m_launchDelayNs(2000),
      m_bulkChunkSize(8 * 1024 * 1024),
      m_bypassReorderBuffer(false),
      m_memorySize(0),
      m_memoryLatency(NanoSeconds(100)),
      m_syncMemLatencyNs(500),
      m_asyncMemLatencyNs(1000),
      m_llrEnabled(false),
      m_llrRetryLimit(UINT32_MAX),
      m_isRetransmitting(false),
      m_fecOpticalOnly(false),
      m_contentionModel(nullptr),
      m_txBytes(0),
      m_rxBytes(0),
      m_txPackets(0),
      m_rxPackets(0),
      m_cnpPacketsSent(0),
      m_cnpPacketsReceived(0)
{
    NS_LOG_FUNCTION(this);

    m_creditManager = CreateObject<CreditManager>();
    m_payloadBuilder = CreateObject<NcclProtocolPayloadBuilder>();
    m_protocolModel = CreateObject<NcclProtocolModel>();
    m_llrManager = CreateObject<LlrManager>();
    m_fecModel = nullptr;  // FEC disabled by default
    m_latencyStatistics = nullptr;

    for (uint8_t vc = 0; vc < m_numVcs; ++vc)
    {
        m_creditManager->InitializeVc(vc, 64);
    }
}

FabricEndpoint::~FabricEndpoint()
{
    NS_LOG_FUNCTION(this);
}

void
FabricEndpoint::DoDispose()
{
    NS_LOG_FUNCTION(this);
    // Cancel every pending deferred-egress event: each captures raw `this`
    // and dereferences m_devices, which is cleared below. A survivor fired
    // after disposal crashed on the cleared member (codex round 13,
    // finding 2) — SIGSEGV when a paced send's callback landed after the
    // endpoint was disposed and released.
    for (const EventId& ev : m_deferredEgressEvents)
    {
        if (!ev.IsExpired())
        {
            Simulator::Cancel(ev);
        }
    }
    m_deferredEgressEvents.clear();
    m_devices.clear();
    m_creditManager = nullptr;
    m_payloadBuilder = nullptr;
    m_protocolModel = nullptr;
    m_interNodeProtocolModel = nullptr;
    m_llrManager = nullptr;
    m_gbnNextAckSeq.clear();
    m_gbnPendingAcks.clear();
    m_reorderBuffers.clear();
    m_linkDegradationModel = nullptr;
    m_fecModel = nullptr;
    m_latencyStatistics = nullptr;
    // The DCQCN machine owns armed recovery timers that capture its raw
    // `this` (FastRecovery/AdditiveIncrease). Drop the reference so the
    // machine is destroyed here, while its timers can still be cancelled
    // by its own DoDispose — a surviving ref would fire into freed memory
    // once the endpoint is gone.
    SetDcqcnRateMachine(nullptr);
    m_sendQueue = std::queue<SendQueueEntry>();
    m_routingTable.clear();
    m_neighborMacTable.clear();
    m_memoryData.clear();
    Application::DoDispose();
}

void
FabricEndpoint::Initialize()
{
    NS_LOG_FUNCTION(this);

    // Initialize credit manager for VCs not already initialized
    for (uint8_t vc = 0; vc < m_numVcs; ++vc)
    {
        if (m_creditManager->GetAvailableCredits(vc) == 0
            && m_configuredVcs.find(vc) == m_configuredVcs.end())
        {
            m_creditManager->InitializeVc(vc, 64);
        }
    }

    m_creditManager->SetCreditAvailableCallback(
        MakeCallback(&FabricEndpoint::OnCreditAvailable, this));

    // Set up LLR permanent loss callback.
    if (m_llrManager)
    {
        m_llrManager->SetPermanentLossCallback(
            MakeCallback(&FabricEndpoint::NotifyPermanentLoss, this));
    }

    // Set up device receive callbacks
    for (auto& device : m_devices)
    {
        device->SetPromiscReceiveCallback(
            MakeCallback(&FabricEndpoint::ReceiveFromDevice, this));
    }
}

void
FabricEndpoint::StartApplication()
{
    NS_LOG_FUNCTION(this);

    // Validate: unrecoverable lossy link (BER>0 with no FEC and no LLR) is impossible
    if (m_linkDegradationModel && m_linkDegradationModel->GetBer() > 0.0 &&
        !m_llrEnabled && !LinkUsesFec())
    {
        NS_FATAL_ERROR("Unrecoverable configuration: BER=" << m_linkDegradationModel->GetBer()
                       << " requires FEC or LLR for packet recovery. "
                       << "Set FEC enabled, LLR enabled, or BER=0.");
    }

    // Only initialize VCs that weren't already initialized via SetVcCredits
    // (m_configuredVcs exemption: an exhausted-but-configured window must
    // NOT be reset to the 64-credit default here — see SetVcCredits).
    for (uint8_t vc = 0; vc < m_numVcs; ++vc)
    {
        if (m_creditManager->GetAvailableCredits(vc) == 0
            && m_configuredVcs.find(vc) == m_configuredVcs.end())
        {
            m_creditManager->InitializeVc(vc, 64);
        }
    }

    m_creditManager->SetCreditAvailableCallback(
        MakeCallback(&FabricEndpoint::OnCreditAvailable, this));

    for (auto& device : m_devices)
    {
        device->SetPromiscReceiveCallback(
            MakeCallback(&FabricEndpoint::ReceiveFromDevice, this));
    }

    // Initialize memory backing store for MEMORY device type
    if (GetDeviceType() == DeviceType::MEMORY && m_memorySize > 0)
    {
        m_memoryData.resize(m_memorySize, 0);
    }
}

void
FabricEndpoint::StopApplication()
{
    NS_LOG_FUNCTION(this);
}

void
FabricEndpoint::SetDeviceType(DeviceType type)
{
    m_deviceType = static_cast<uint8_t>(type);
}

DeviceType
FabricEndpoint::GetDeviceType() const
{
    return static_cast<DeviceType>(m_deviceType);
}

void
FabricEndpoint::SetFabricType(FabricType type)
{
    m_fabricType = static_cast<uint8_t>(type);
}

FabricType
FabricEndpoint::GetFabricType() const
{
    return static_cast<FabricType>(m_fabricType);
}

void
FabricEndpoint::SetInterNodeFabricType(FabricType type)
{
    m_interNodeFabricType = static_cast<uint8_t>(type);
}

void
FabricEndpoint::SetNodeId(uint32_t nodeId)
{
    m_nodeId = nodeId;
}

uint32_t
FabricEndpoint::GetNodeId() const
{
    return m_nodeId;
}

void
FabricEndpoint::SetLocalRankRange(uint16_t base, uint16_t count)
{
    m_localRankBase = base;
    m_localRankCount = count;
}

bool
FabricEndpoint::IsLocalRank(uint16_t rank) const
{
    return rank >= m_localRankBase && rank < m_localRankBase + m_localRankCount;
}

void
FabricEndpoint::SetRingNeighbors(uint16_t next, uint16_t prev)
{
    m_ringNext = next;
    m_ringPrev = prev;
}

uint16_t
FabricEndpoint::GetRingNext() const
{
    return m_ringNext;
}

uint16_t
FabricEndpoint::GetRingPrev() const
{
    return m_ringPrev;
}

void
FabricEndpoint::SetTreeNeighbors(uint16_t parent, uint16_t leftChild, uint16_t rightChild)
{
    m_treeParent = parent;
    m_treeLeftChild = leftChild;
    m_treeRightChild = rightChild;
}

uint16_t
FabricEndpoint::GetTreeParent() const
{
    return m_treeParent;
}

uint16_t
FabricEndpoint::GetTreeLeftChild() const
{
    return m_treeLeftChild;
}

uint16_t
FabricEndpoint::GetTreeRightChild() const
{
    return m_treeRightChild;
}

FabricType
FabricEndpoint::GetFabricTypeForDest(uint16_t destRank) const
{
    // IsLocalRank alone decides Su vs So — no m_localRankCount > 1 gate:
    // with one GPU per node the only local rank is this endpoint itself, and
    // the old count gate would wrongly push remote traffic onto the Su
    // fabric. Single-node setups set a range covering every rank, so
    // IsLocalRank is always true there and behavior is unchanged.
    if (!IsLocalRank(destRank))
    {
        return static_cast<FabricType>(m_interNodeFabricType);
    }
    return static_cast<FabricType>(m_fabricType);
}

void
FabricEndpoint::SetRank(uint16_t rank)
{
    m_rank = rank;
}

uint16_t
FabricEndpoint::GetRank() const
{
    return m_rank;
}

uint32_t
FabricEndpoint::AddNetDevice(Ptr<NetDevice> device)
{
    NS_LOG_FUNCTION(this << device);
    uint32_t idx = m_devices.size();
    m_devices.push_back(device);

    // Set endpoint MAC address from first device if not already configured
    if (m_address == Mac48Address() && idx == 0)
    {
        m_address = Mac48Address::ConvertFrom(device->GetAddress());
    }

    return idx;
}

uint32_t
FabricEndpoint::GetNNetDevices() const
{
    return m_devices.size();
}

Ptr<NetDevice>
FabricEndpoint::GetNetDevice(uint32_t index) const
{
    if (index < m_devices.size())
    {
        return m_devices[index];
    }
    return nullptr;
}

void
FabricEndpoint::SetRoutingEntry(uint16_t destRank, uint32_t deviceIndex)
{
    m_routingTable[destRank] = deviceIndex;
}

void
FabricEndpoint::ClearRoutingTable()
{
    m_routingTable.clear();
}

uint32_t
FabricEndpoint::GetRoutingDeviceIndex(uint16_t destRank) const
{
    auto it = m_routingTable.find(destRank);
    if (it != m_routingTable.end())
    {
        return it->second;
    }
    return UINT32_MAX;
}

void
FabricEndpoint::SetRoutingDevices(uint16_t destRank, const std::vector<uint32_t>& deviceIndices)
{
    m_routingDevicesTable[destRank] = deviceIndices;
    // Also set the first device as the default routing entry for backward compatibility
    if (!deviceIndices.empty())
    {
        m_routingTable[destRank] = deviceIndices[0];
    }
}

std::vector<uint32_t>
FabricEndpoint::GetRoutingDevices(uint16_t destRank) const
{
    auto it = m_routingDevicesTable.find(destRank);
    if (it != m_routingDevicesTable.end())
    {
        return it->second;
    }
    // Fall back to single device if multi-path not set
    auto singleIt = m_routingTable.find(destRank);
    if (singleIt != m_routingTable.end())
    {
        return {singleIt->second};
    }
    return {};
}

void
FabricEndpoint::SetSprayingEnabled(bool enable)
{
    m_sprayingEnabled = enable;
}

bool
FabricEndpoint::GetSprayingEnabled() const
{
    return m_sprayingEnabled;
}

void
FabricEndpoint::SetSprayChunkSize(uint32_t chunkSize)
{
    m_sprayChunkSize = chunkSize;
}

void
FabricEndpoint::SetNumLanes(uint32_t numLanes)
{
    m_numLanes = numLanes;
}

uint32_t
FabricEndpoint::GetNumLanes() const
{
    return m_numLanes;
}

void
FabricEndpoint::SetLaneGroup(uint32_t logicalDevIdx, const std::vector<uint32_t>& physicalDevIndices)
{
    m_laneGroups[logicalDevIdx] = physicalDevIndices;
}

std::vector<uint32_t>
FabricEndpoint::GetPhysicalDevicesForDest(uint16_t destRank) const
{
    // Check routing devices table first (for switched topology with multiple links)
    auto routingIt = m_routingDevicesTable.find(destRank);
    if (routingIt != m_routingDevicesTable.end() && routingIt->second.size() > 1)
    {
        return routingIt->second;
    }

    // Then check lane groups (for ring/fullmesh topology with numLanes)
    uint32_t logicalDevIdx = GetRoutingDeviceIndex(destRank);
    auto it = m_laneGroups.find(logicalDevIdx);
    if (it != m_laneGroups.end() && it->second.size() > 1)
    {
        return it->second;
    }

    return {logicalDevIdx};
}

void
FabricEndpoint::SetNeighborMac(uint16_t rank, Mac48Address addr)
{
    m_neighborMacTable[rank] = addr;
}

Mac48Address
FabricEndpoint::GetNeighborMac(uint16_t rank) const
{
    auto it = m_neighborMacTable.find(rank);
    if (it != m_neighborMacTable.end())
    {
        return it->second;
    }
    return Mac48Address::GetBroadcast();
}

uint32_t
FabricEndpoint::ResolveDeviceIndex(uint16_t destRank)
{
    // For spraying: round-robin over THIS DESTINATION's permitted devices
    // (m_routingDevicesTable[destRank]), not over every attached device.
    // GetNextDeviceIndex() rotates over all of m_devices — on a multi-node
    // endpoint that set mixes the dest's links with unrelated scale-out
    // ports, and a sprayed packet would leave on a port that never reaches
    // the dest. No routing-devices entry (single-device dests, e.g. the
    // inter-node NICs set up via SetRoutingEntry) falls through to the plain
    // routing table below, so single-path dests keep exact device selection.
    if (m_sprayingEnabled)
    {
        auto devicesIt = m_routingDevicesTable.find(destRank);
        if (devicesIt != m_routingDevicesTable.end() && devicesIt->second.size() > 1)
        {
            const std::vector<uint32_t>& devs = devicesIt->second;
            uint32_t index = devs[m_currentDeviceIndex % devs.size()];
            m_currentDeviceIndex =
                (m_currentDeviceIndex + 1) % m_devices.size();
            return index;
        }
    }

    // Non-spraying or no multi-path: use routing table
    auto it = m_routingTable.find(destRank);
    if (it != m_routingTable.end())
    {
        return it->second;
    }
    return GetNextDeviceIndex();
}

Mac48Address
FabricEndpoint::ResolveDestMac(uint16_t destRank)
{
    auto it = m_neighborMacTable.find(destRank);
    if (it != m_neighborMacTable.end())
    {
        return it->second;
    }
    return Mac48Address::GetBroadcast();
}

// Send-path methods (SendData, SendCollective, SendP2p, SendMemory*, SendDataInternal)
// are implemented in fabric-endpoint-send.cc.

void
FabricEndpoint::SetNumVirtualChannels(uint8_t numVcs)
{
    m_numVcs = numVcs;
}

void
FabricEndpoint::SetVcCredits(uint8_t vcId, uint32_t credits)
{
    m_creditManager->InitializeVc(vcId, credits);
    // Mark the VC as explicitly configured so the lazy default init in
    // Initialize()/StartApplication() cannot clobber a legitimately
    // exhausted window back to 64 credits (a window fully consumed before
    // StartApplication runs is a working sliding window, not an unset VC).
    if (credits > 0)
    {
        m_configuredVcs.insert(vcId);
    }
}

void
FabricEndpoint::SetLaunchDelay(uint64_t delayNs)
{
    m_launchDelayNs = delayNs;
}

void
FabricEndpoint::SetReceiveCallback(ReceiveCallback cb)
{
    m_receiveCallback = cb;
}

void
FabricEndpoint::SetMemoryResponseCallback(ReceiveCallback cb)
{
    m_memoryResponseCallback = cb;
}

void
FabricEndpoint::SetMemoryRequestCallback(MemoryRequestCallback cb)
{
    m_memoryRequestCallback = cb;
}

void
FabricEndpoint::SetMemorySemanticCallback(MemorySemanticCallback cb)
{
    m_memorySemanticCallback = cb;
}

Mac48Address
FabricEndpoint::GetAddress() const
{
    return m_address;
}

void
FabricEndpoint::SetAddress(Mac48Address addr)
{
    m_address = addr;
}

void
FabricEndpoint::SetLinkDegradationModel(Ptr<LinkDegradationModel> model)
{
    m_linkDegradationModel = model;
}

Ptr<LinkDegradationModel>
FabricEndpoint::GetLinkDegradationModel() const
{
    return m_linkDegradationModel;
}

void
FabricEndpoint::SetProtocolModel(Ptr<ProtocolModel> model)
{
    m_protocolModel = model;
}

Ptr<ProtocolModel>
FabricEndpoint::GetProtocolModel() const
{
    return m_protocolModel;
}

void
FabricEndpoint::SetInterNodeProtocolModel(Ptr<ProtocolModel> model)
{
    m_interNodeProtocolModel = model;
}

Ptr<ProtocolModel>
FabricEndpoint::GetProtocolModelForDest(uint16_t destRank) const
{
    // Use the inter-node (So) protocol model for remote destinations; otherwise
    // fall back to the scale-up (Su) model. IsLocalRank alone decides — no
    // m_localRankCount > 1 gate: with one GPU per node the only local rank is
    // this endpoint itself, and the old count gate would wrongly run remote
    // traffic on the Su model. Single-node setups cover every rank with the
    // local range, so behavior there is unchanged.
    if (m_interNodeProtocolModel && !IsLocalRank(destRank))
    {
        return m_interNodeProtocolModel;
    }
    return m_protocolModel;
}

void
FabricEndpoint::SetProtocolPayloadBuilder(Ptr<ProtocolPayloadBuilder> builder)
{
    m_payloadBuilder = builder;
}

Ptr<ProtocolPayloadBuilder>
FabricEndpoint::GetProtocolPayloadBuilder() const
{
    return m_payloadBuilder;
}

void
FabricEndpoint::SetSoPayloadBuilder(Ptr<ProtocolPayloadBuilder> builder)
{
    m_soPayloadBuilder = builder;
}

Ptr<ProtocolPayloadBuilder>
FabricEndpoint::GetPayloadBuilderForRank(uint16_t rank) const
{
    // Mirrors GetProtocolModelForDest's Su/So split: the So builder (when
    // mounted) frames inter-node traffic; Su traffic keeps the vendor builder.
    // IsLocalRank alone decides (see GetProtocolModelForDest — one GPU per
    // node still has remote ranks).
    if (m_soPayloadBuilder && !IsLocalRank(rank))
    {
        return m_soPayloadBuilder;
    }
    return m_payloadBuilder;
}

void
FabricEndpoint::SetBypassReorderBuffer(bool bypass)
{
    m_bypassReorderBuffer = bypass;
}

bool
FabricEndpoint::GetBypassReorderBuffer() const
{
    return m_bypassReorderBuffer;
}

void
FabricEndpoint::SetMemorySize(uint64_t size)
{
    m_memorySize = size;
}

uint64_t
FabricEndpoint::GetMemorySize() const
{
    return m_memorySize;
}

void
FabricEndpoint::SetMemoryLatency(Time latency)
{
    m_memoryLatency = latency;
}

Time
FabricEndpoint::GetMemoryLatency() const
{
    return m_memoryLatency;
}

void
FabricEndpoint::NotifyMemorySemanticComplete(uint16_t destRank, uint64_t address,
                                              MemoryAccessType accessType)
{
    NS_LOG_FUNCTION(this << destRank << address << static_cast<int>(accessType));
    if (!m_memorySemanticCallback.IsNull())
    {
        m_memorySemanticCallback(destRank, address, accessType);
    }
}

void
FabricEndpoint::SetLlrEnabled(bool enable)
{
    m_llrEnabled = enable;
}

void
FabricEndpoint::SetBulkChunkSize(uint32_t bytes)
{
    NS_ABORT_MSG_IF(bytes == 0, "Bulk chunk size must be greater than zero");
    m_bulkChunkSize = bytes;
}

void
FabricEndpoint::SetSoTxWindow(uint32_t chunks)
{
    m_soTxWindow = chunks;
}

void
FabricEndpoint::SetSoGbnStrict(bool enable)
{
    m_soGbnStrict = enable;
}

bool
FabricEndpoint::IsStrictGbnFlow(const FabricHeader& header) const
{
    if (!m_soGbnStrict || !m_llrEnabled || !m_llrManager || !m_bypassReorderBuffer)
    {
        return false;
    }
    if (m_llrManager->GetLlrMode() != LlrMode::GO_BACK_N)
    {
        return false;
    }
    // So-only: never touch the sprayed Su fabric, where out-of-order arrival
    // is normal (the reorder/dedup layer's job) and a gap does not mean loss.
    return !IsLocalRank(header.GetSourceRank());
}

void
FabricEndpoint::SendStrictGbnNak(const FabricHeader& dataHeader, uint32_t expectedSeq)
{
    FabricHeader nackHeader;
    nackHeader.SetPacketType(FabricPacketType::NACK);
    nackHeader.SetFabricType(GetFabricTypeForDest(dataHeader.GetSourceRank()));
    nackHeader.SetSourceRank(dataHeader.GetDestRank());
    nackHeader.SetDestRank(dataHeader.GetSourceRank());
    nackHeader.SetSequenceNumber(expectedSeq);
    nackHeader.SetFlowId(dataHeader.GetFlowId());
    nackHeader.SetVirtualChannel(dataHeader.GetVirtualChannel());
    nackHeader.SetSourceMac(m_address);
    nackHeader.SetDestMac(ResolveDestMac(dataHeader.GetSourceRank()));

    Ptr<Packet> nackPkt = Create<Packet>(0);
    nackPkt->AddHeader(nackHeader);

    uint32_t deviceIndex = ResolveDeviceIndex(dataHeader.GetSourceRank());
    SendPacketOnDevice(nackPkt, deviceIndex, ResolveDestMac(dataHeader.GetSourceRank()));
}

void
FabricEndpoint::PumpBulkTxWindow()
{
    const uint32_t soHeaderSize = SoHeader().GetSerializedSize();
    while (!m_bulkTxQueue.empty() && m_bulkTxInflight < m_soTxWindow)
    {
        BulkTxChunk desc = m_bulkTxQueue.front();
        m_bulkTxQueue.pop_front();

        uint64_t seqKey = MakeSeqKey(desc.destRank, desc.vcId, desc.flowId);
        uint32_t seqNum = m_nextSeqNum[seqKey]++;

        const bool stamp = desc.stamp && (desc.wireChunk > soHeaderSize);
        Ptr<Packet> chunkPacket = stamp
            ? Create<Packet>(desc.wireChunk - soHeaderSize)
            : Create<Packet>(desc.wireChunk);
        if (stamp)
        {
            SoHeader soHeader;
            soHeader.SetProtocolId(static_cast<uint8_t>(RoceProtocol::SIMPLE));
            soHeader.SetEcn(false); // marked in-flight by the So switch on congestion
            soHeader.SetDataSize(desc.fragmentDataSize);
            soHeader.SetChunkIndex(desc.chunkIndex);
            chunkPacket->AddHeader(soHeader);
        }

        FabricHeader header;
        header.SetPacketType(FabricPacketType::DATA);
        header.SetFabricType(GetFabricTypeForDest(desc.destRank));
        header.SetFlowId(desc.flowId);
        header.SetSequenceNumber(seqNum);
        header.SetVirtualChannel(desc.vcId);
        header.SetSourceRank(m_rank);
        header.SetDestRank(desc.destRank);
        header.SetPayloadSize(desc.wireChunk);
        header.SetSourceMac(m_address);
        header.SetDestMac(ResolveDestMac(desc.destRank));
        header.SetProtocol(stamp ? desc.protocolId : 0);
        header.SetProtocolFramed(stamp);
        header.SetEffectiveDataSize(desc.fragmentDataSize);
        header.SetTtl(64);
        chunkPacket->AddHeader(header);

        m_bulkTxInflight++;
        // Register window membership so the release paths can release a slot
        // exactly once and only for packets that actually held one: ACK /
        // permanent-loss handling sees LLR store entries from ALL send
        // paths (small unfragmented remote sends, local Su packets), which
        // must not free a bulk window slot they never consumed.
        m_bulkTxWindowSeqs.emplace(desc.destRank, desc.vcId, desc.flowId, seqNum);
        SendPacketOnDevice(chunkPacket, desc.deviceIndex, ResolveDestMac(desc.destRank));

        m_txTrace(chunkPacket->GetSize(), seqNum);
        m_txPackets++;
        m_txBytes += chunkPacket->GetSize();
    }
}

void
FabricEndpoint::ReleaseBulkTxSlot()
{
    if (m_bulkTxInflight > 0)
    {
        m_bulkTxInflight--;
    }
    PumpBulkTxWindow();
}

bool
FabricEndpoint::GetLlrEnabled() const
{
    return m_llrEnabled;
}

void
FabricEndpoint::SetLlrRetryLimit(uint32_t limit)
{
    m_llrRetryLimit = limit;
}

uint32_t
FabricEndpoint::GetLlrRetryLimit() const
{
    return m_llrRetryLimit;
}

void
FabricEndpoint::SetFlowControlPolicy(FlowControlPolicy policy)
{
    m_flowControlPolicy = policy;
}

FlowControlPolicy
FabricEndpoint::GetFlowControlPolicy() const
{
    return m_flowControlPolicy;
}

bool
FabricEndpoint::FlowControlGate(uint8_t vcId, uint32_t seqNum, bool creditBypass)
{
    switch (m_flowControlPolicy)
    {
    case FlowControlPolicy::CREDIT:
        if (creditBypass || !m_creditManager)
        {
            return true;
        }
        if (!m_creditManager->HasCredits(vcId))
        {
            return false;
        }
        m_creditManager->ConsumeCredit(vcId, seqNum);
        return true;
    case FlowControlPolicy::WINDOW:
    case FlowControlPolicy::RATE:
        NS_LOG_WARN("FlowControlPolicy " << FlowControlPolicyName(m_flowControlPolicy)
                                         << " not implemented; admitting packet");
        return true;
    }
    return true;
}

Ptr<LlrManager>
FabricEndpoint::GetLlrManager() const
{
    return m_llrManager;
}

void
FabricEndpoint::SetFecModel(Ptr<FecModel> model)
{
    m_fecModel = model;
}

Ptr<FecModel>
FabricEndpoint::GetFecModel() const
{
    return m_fecModel;
}

void
FabricEndpoint::SetFecOpticalOnly(bool opticalOnly)
{
    m_fecOpticalOnly = opticalOnly;
}

bool
FabricEndpoint::LinkUsesFec() const
{
    if (!m_fecModel || !m_fecModel->IsEnabled())
    {
        return false;
    }
    if (!m_fecOpticalOnly)
    {
        return true;
    }
    return m_linkDegradationModel
           && m_linkDegradationModel->GetLinkMetadata().medium == "optical";
}

void
FabricEndpoint::SetLatencyStatistics(Ptr<LatencyStatistics> stats)
{
    m_latencyStatistics = stats;
}

Ptr<LatencyStatistics>
FabricEndpoint::GetLatencyStatistics() const
{
    return m_latencyStatistics;
}

void
FabricEndpoint::SetContentionModel(Ptr<ContentionModel> model)
{
    m_contentionModel = model;
}

Ptr<ContentionModel>
FabricEndpoint::GetContentionModel() const
{
    return m_contentionModel;
}

void
FabricEndpoint::SetSyncMemLatencyNs(uint64_t ns)
{
    m_syncMemLatencyNs = ns;
}

uint64_t
FabricEndpoint::GetSyncMemLatencyNs() const
{
    return m_syncMemLatencyNs;
}

void
FabricEndpoint::SetAsyncMemLatencyNs(uint64_t ns)
{
    m_asyncMemLatencyNs = ns;
}

uint64_t
FabricEndpoint::GetAsyncMemLatencyNs() const
{
    return m_asyncMemLatencyNs;
}

Ptr<ReorderBuffer>
FabricEndpoint::GetOrCreateReorderBuffer(uint8_t vcId, uint16_t flowId, uint16_t sourceRank)
{
    // Key includes the SOURCE rank: sequence numbers are per-sender, so two
    // senders hitting the same (vc, flow) on one receiver would otherwise
    // share a reorder buffer and each discard the other's seq 0 as a
    // duplicate.
    uint64_t key = (static_cast<uint64_t>(sourceRank) << 32) |
                   (static_cast<uint32_t>(vcId) << 16) | (flowId & 0xFFFF);
    auto it = m_reorderBuffers.find(key);
    if (it != m_reorderBuffers.end())
    {
        return it->second;
    }
    Ptr<ReorderBuffer> rb = CreateObject<ReorderBuffer>();
    rb->SetPacketDeliveryCallback(
        MakeCallback(&FabricEndpoint::OnPacketDelivered, this));
    rb->SetPermanentGapCallback(
        MakeCallback(&FabricEndpoint::OnPermanentGapDelivered, this));
    m_reorderBuffers[key] = rb;
    return rb;
}

void
FabricEndpoint::GetReorderBufferStats(uint32_t& totalReorderEvents, uint32_t& maxOccupancy) const
{
    totalReorderEvents = 0;
    maxOccupancy = 0;
    for (const auto& kv : m_reorderBuffers)
    {
        totalReorderEvents += kv.second->GetReorderEventCount();
        uint32_t occ = kv.second->GetMaxOccupancy();
        if (occ > maxOccupancy)
        {
            maxOccupancy = occ;
        }
    }
    totalReorderEvents += m_bypassReorderEvents;
}

bool
FabricEndpoint::ReceiveFromDevice(Ptr<NetDevice> device, Ptr<const Packet> packet,
                                   uint16_t protocol, const Address& source,
                                   const Address& destination, NetDevice::PacketType packetType)
{
    NS_LOG_FUNCTION(this << device << packet);

    Ptr<Packet> pkt = packet->Copy();

    // Deserialize header (needed for NACK path before degradation check)
    FabricHeader header;
    pkt->RemoveHeader(header);

    // Strip FEC parity bytes if FEC is enabled
    if (LinkUsesFec())
    {
        uint32_t payloadSize = header.GetPayloadSize();
        uint32_t currentSize = pkt->GetSize();
        if (currentSize > payloadSize)
        {
            uint32_t paritySize = currentSize - payloadSize;
            pkt->RemoveAtEnd(paritySize);
        }
    }

    // Apply link degradation and FEC models
    bool packetCorrupted = false;
    bool packetSurvived = true;
    (void)packetCorrupted;
    (void)packetSurvived;

    // Protocol control and retry packets are reliable against BER corruption.
    // They still go through link-down and packet-loss-rate checks (physical-layer).
    bool isProtocolControl = header.IsControlPacket() || header.IsRetryPacket();

    if (m_linkDegradationModel)
    {
        // Check link state first: DOWN means physical link unavailable, FEC can't help
        if (!m_linkDegradationModel->IsLinkUp())
        {
            NS_LOG_DEBUG("Link is DOWN, dropping packet regardless of FEC");
            return true;
        }

        // Check packet loss rate (independent of BER, FEC can't help)
        if (m_linkDegradationModel->GetPacketLossRate() > 0)
        {
            double lossSample = m_linkDegradationModel->GetRandomValue();
            if (lossSample < m_linkDegradationModel->GetPacketLossRate())
            {
                NS_LOG_DEBUG("Packet dropped by loss rate, FEC can't help");
                // NACK only lost DATA packets. A dropped control/retry packet
                // (ACK, NACK, CREDIT, RETRY_ACK, ...) must be dropped silently:
                // NACKing it makes the peer treat the control seqNum as an
                // unACKed data seq (NACK-of-NACK -> retryMisses, duplicate
                // retransmits, false timeouts). Real link protocols recover
                // lost acks via the sender's retry timer, not via NACK.
                // Strict So GBN flows are also silent here: a real receiver
                // never sees the dropped packet — the gap is detected on the
                // next in-flight arrival (or the sender's retry timer).
                if (m_llrEnabled && m_llrManager && !isProtocolControl &&
                    !IsStrictGbnFlow(header))
                {
                    FabricHeader nackHeader;
                    nackHeader.SetPacketType(FabricPacketType::NACK);
                    nackHeader.SetFabricType(GetFabricTypeForDest(header.GetSourceRank()));
                    nackHeader.SetSourceRank(header.GetDestRank());
                    nackHeader.SetDestRank(header.GetSourceRank());
                    nackHeader.SetSequenceNumber(header.GetSequenceNumber());
                    nackHeader.SetFlowId(header.GetFlowId());
                    nackHeader.SetVirtualChannel(header.GetVirtualChannel());
                    nackHeader.SetSourceMac(m_address);
                    nackHeader.SetDestMac(ResolveDestMac(header.GetSourceRank()));

                    Ptr<Packet> nackPkt = Create<Packet>(0);
                    nackPkt->AddHeader(nackHeader);

                    uint32_t deviceIndex = ResolveDeviceIndex(header.GetSourceRank());
                    SendPacketOnDevice(nackPkt, deviceIndex, ResolveDestMac(header.GetSourceRank()));
                }
                return true;
            }
        }

        // BER sampling and FEC decode: only for data/collective packets.
        // Protocol control (CREDIT, ACK, NACK) and retry (RETRY_REQUEST, RETRY_ACK)
        // packets are reliable against bit errors.
        if (!isProtocolControl)
        {
            // Get effective BER (may be elevated during burst mode)
            double ber = m_linkDegradationModel->GetBer();

            if (LinkUsesFec())
            {
                // FEC-enabled path: compute post-FEC uncorrectable probability
                // Use DecodePacket for multi-codeword packets
                uint32_t payloadSize = header.GetPayloadSize();
                FecResult fecResult = m_fecModel->DecodePacket(ber, payloadSize);
                if (fecResult == FecResult::UNCORRECTABLE)
                {
                    packetCorrupted = true;
                    packetSurvived = false;
                    // If LLR is enabled, send NACK to trigger retransmission
                    if (m_llrEnabled && m_llrManager)
                    {
                        NS_LOG_DEBUG("Packet uncorrectable after FEC decode, sending NACK for LLR retry");
                        FabricHeader nackHeader;
                        nackHeader.SetPacketType(FabricPacketType::NACK);
                        nackHeader.SetFabricType(GetFabricTypeForDest(header.GetSourceRank()));
                        nackHeader.SetSourceRank(header.GetDestRank());
                        nackHeader.SetDestRank(header.GetSourceRank());
                        nackHeader.SetSequenceNumber(header.GetSequenceNumber());
                        nackHeader.SetFlowId(header.GetFlowId());
                        nackHeader.SetVirtualChannel(header.GetVirtualChannel());
                        nackHeader.SetSourceMac(m_address);
                        nackHeader.SetDestMac(ResolveDestMac(header.GetSourceRank()));

                        Ptr<Packet> nackPkt = Create<Packet>(0);
                        nackPkt->AddHeader(nackHeader);

                        uint32_t deviceIndex = ResolveDeviceIndex(header.GetSourceRank());
                        SendPacketOnDevice(nackPkt, deviceIndex, ResolveDestMac(header.GetSourceRank()));
                    }
                    else
                    {
                        NS_LOG_DEBUG("Packet uncorrectable after FEC decode, no LLR - packet permanently lost");
                    }
                    return true;
                }
                // CORRECTABLE or NO_ERROR: packet survives with FEC decode latency
                NS_LOG_DEBUG("Packet survived with FEC (result=" << static_cast<int>(fecResult) << ")");
            }
            else
            {
                // No FEC: use link degradation's per-packet BER sampling.
                // ProcessPacketBerOnly (not ProcessPacket): the loss-rate
                // draw already happened above — ProcessPacket would draw the
                // loss rate a second time, compounding the effective loss.
                if (!m_linkDegradationModel->ProcessPacketBerOnly(pkt))
                {
                    packetCorrupted = true;
                    packetSurvived = false;

                    // If LLR is enabled, send NACK to trigger retransmission
                    if (m_llrEnabled && m_llrManager)
                    {
                        NS_LOG_DEBUG("Packet dropped by link degradation (no FEC), sending NACK for LLR retry");
                        FabricHeader nackHeader;
                        nackHeader.SetPacketType(FabricPacketType::NACK);
                        nackHeader.SetFabricType(GetFabricTypeForDest(header.GetSourceRank()));
                        nackHeader.SetSourceRank(header.GetDestRank());
                        nackHeader.SetDestRank(header.GetSourceRank());
                        nackHeader.SetSequenceNumber(header.GetSequenceNumber());
                        nackHeader.SetFlowId(header.GetFlowId());
                        nackHeader.SetVirtualChannel(header.GetVirtualChannel());
                        nackHeader.SetSourceMac(m_address);
                        nackHeader.SetDestMac(ResolveDestMac(header.GetSourceRank()));

                        Ptr<Packet> nackPkt = Create<Packet>(0);
                        nackPkt->AddHeader(nackHeader);

                        uint32_t deviceIndex = ResolveDeviceIndex(header.GetSourceRank());
                        SendPacketOnDevice(nackPkt, deviceIndex, ResolveDestMac(header.GetSourceRank()));
                        return true;
                    }
                    NS_LOG_DEBUG("Packet dropped by link degradation model (no FEC, no LLR)");
                    return true;
                }
            }
        }
    }

    // Add FEC decode latency whenever FEC is enabled (decode is a pipeline cost)
    if (LinkUsesFec())
    {
        Time decodeLatency = m_fecModel->GetDecodeLatency();
        if (decodeLatency > Seconds(0))
        {
            // Schedule the rest of receive processing after decode latency.
            // Tracked in m_deferredEgressEvents so DoDispose cancels it: the
            // bound member call targets raw `this` and fires into freed
            // memory when the endpoint is disposed and released before the
            // decode completes (codex round 15, finding 1 — valgrind-confirmed
            // invalid reads/writes in ContinueReceiveAfterFec's traced-callback
            // and stats members, which DoDispose nulls).
            m_deferredEgressEvents.push_back(
                Simulator::Schedule(decodeLatency,
                                    &FabricEndpoint::ContinueReceiveAfterFec,
                                    this, pkt, header));
            return true;
        }
    }

    m_rxTrace(pkt->GetSize(), header.GetSequenceNumber());
    m_rxPackets++;
    m_rxBytes += pkt->GetSize();

    uint16_t destRank = header.GetDestRank();

    // Check if this packet is destined for us or needs forwarding
    if (destRank != m_rank && !m_routingTable.empty())
    {
        // Forward the packet to its destination
        NS_LOG_DEBUG("Forwarding packet from " << header.GetSourceRank()
                     << " to " << destRank << " at node " << m_rank);

        uint32_t deviceIndex = ResolveDeviceIndex(destRank);
        Mac48Address destMac = ResolveDestMac(destRank);
        // Re-target the header's DestMac for THIS hop: switches route on the
        // FabricHeader's MACs (nvswitch.cc), so a stale first-hop DestMac is
        // unknown at the destination node's switch and floods every port --
        // each non-target GPU then re-forwards the flooded copy, storming at
        // high radix (gateway topologies: ring/torus2d/dragonfly).
        header.SetDestMac(destMac);
        pkt->AddHeader(header);
        SendPacketOnDevice(pkt, deviceIndex, destMac);

        return true;
    }

    // Packet is destined for this endpoint - process it
    NS_LOG_DEBUG("Packet destRank=" << destRank << " m_rank=" << m_rank << " processing at this endpoint");
    if (header.IsRetryPacket())
    {
        ProcessRetryPacket(pkt, header);
    }
    else if (header.GetPacketType() == FabricPacketType::NACK)
    {
        // NACK from receiver: FEC uncorrectable, trigger LLR retry
        ProcessNackPacket(pkt, header);
    }
    else if (header.GetPacketType() == FabricPacketType::ACK)
    {
        ProcessAckPacket(pkt, header);
    }
    else if (header.GetPacketType() == FabricPacketType::PERMANENT_LOSS)
    {
        // PERMANENT_LOSS from sender: mark seqNum as permanently lost.
        // Sender already returned the credit in NotifyPermanentLoss, so
        // we do NOT call ProcessCreditPacket here (no credit to return).
        ProcessPermanentLossNotice(header);
    }
    else if (header.GetPacketType() == FabricPacketType::CNP)
    {
        // CNP: drive the DCQCN rate machine (per-flow Alpha backoff). A CNP
        // is control traffic but must not be consumed as a credit, so it is
        // intercepted before the generic control (ProcessCreditPacket) branch.
        ProcessCnpPacket(header);
    }
    else if (header.GetPacketType() == FabricPacketType::RTT_PROBE)
    {
        // RTTCC probe: reflect it straight back to the prober. Intercepted
        // before the generic control branch (a probe is not a credit).
        SendRttResponsePacket(header);
    }
    else if (header.GetPacketType() == FabricPacketType::RTT_RESPONSE)
    {
        // RTTCC echo: turn it into an RTT sample for the rate machine.
        ProcessRttResponsePacket(header);
    }
    else if (header.IsControlPacket())
    {
        ProcessCreditPacket(pkt, header);
    }
    else if (header.IsMemoryPacket())
    {
        ProcessMemoryPacket(pkt, header);
    }
    else
    {
        ProcessDataPacket(pkt, header);
    }

    return true;
}

void
FabricEndpoint::ProcessPermanentLossNotice(const FabricHeader& header)
{
    // PERMANENT_LOSS from sender: the retransmit budget for this sequence
    // number is exhausted and the sender has already returned the credit in
    // NotifyPermanentLoss, so there is no credit to hand back here. Advance
    // terminal progress so the receiver does not stall waiting for a seq that
    // will never arrive.
    uint8_t vcId = header.GetVirtualChannel();
    uint16_t flowId = header.GetFlowId();
    uint32_t lostSeqNum = header.GetSequenceNumber();
    if (IsStrictGbnFlow(header))
    {
        // Strict So GBN keeps its own per-flow expected counter instead of a
        // reorder buffer: advance past the lost seq (or park it for
        // consumption when expected catches up).
        uint64_t flowKey = (static_cast<uint64_t>(vcId) << 48) |
                           (static_cast<uint64_t>(flowId) << 32) |
                           header.GetSourceRank();
        uint32_t& expected = m_strictNextExpected[flowKey];
        if (lostSeqNum == expected)
        {
            expected++;
            auto pl = m_strictPermLost.find(flowKey);
            if (pl != m_strictPermLost.end())
            {
                while (pl->second.erase(expected) > 0)
                {
                    expected++;
                }
                if (pl->second.empty())
                {
                    m_strictPermLost.erase(pl);
                }
            }
            m_strictNakPending.erase(flowKey);
        }
        else if (lostSeqNum > expected)
        {
            m_strictPermLost[flowKey].insert(lostSeqNum);
        }
    }
    else
    {
        // Reorder-buffer flows: mark the gap and release everything below it
        // that is still parked. The header carries the original data packet's
        // metadata (effectiveDataSize, flowId, ...) so the permanent-gap
        // callback can propagate terminal progress.
        Ptr<ReorderBuffer> rb = GetOrCreateReorderBuffer(vcId, flowId,
                                                         header.GetSourceRank());
        rb->MarkPermanentGap(lostSeqNum, header);
        rb->DeliverReadyPackets();
    }
}

void
FabricEndpoint::ContinueReceiveAfterFec(Ptr<Packet> pkt, FabricHeader header)
{
    // Continue receive processing after FEC decode latency
    m_rxTrace(pkt->GetSize(), header.GetSequenceNumber());
    m_rxPackets++;
    m_rxBytes += pkt->GetSize();

    uint16_t destRank = header.GetDestRank();

    if (destRank != m_rank && !m_routingTable.empty())
    {
        uint32_t deviceIndex = ResolveDeviceIndex(destRank);
        Mac48Address destMac = ResolveDestMac(destRank);
        // Re-target DestMac for this hop (see Receive's forwarding branch).
        header.SetDestMac(destMac);
        pkt->AddHeader(header);
        SendPacketOnDevice(pkt, deviceIndex, destMac);
        return;
    }

    if (header.IsRetryPacket())
    {
        ProcessRetryPacket(pkt, header);
    }
    else if (header.GetPacketType() == FabricPacketType::NACK)
    {
        ProcessNackPacket(pkt, header);
    }
    else if (header.GetPacketType() == FabricPacketType::ACK)
    {
        ProcessAckPacket(pkt, header);
    }
    else if (header.GetPacketType() == FabricPacketType::PERMANENT_LOSS)
    {
        // Sender already returned the credit in NotifyPermanentLoss. Shared
        // with the non-FEC receive path: strict GBN advances its expected
        // counter, reorder-buffer flows mark a permanent gap.
        ProcessPermanentLossNotice(header);
    }
    else if (header.GetPacketType() == FabricPacketType::CNP)
    {
        // CNP: drive the DCQCN rate machine (per-flow Alpha backoff). A CNP
        // is control traffic but must not be consumed as a credit, so it is
        // intercepted before the generic control (ProcessCreditPacket) branch.
        ProcessCnpPacket(header);
    }
    else if (header.GetPacketType() == FabricPacketType::RTT_PROBE)
    {
        // RTTCC probe: reflect it straight back to the prober. Intercepted
        // before the generic control branch (a probe is not a credit).
        SendRttResponsePacket(header);
    }
    else if (header.GetPacketType() == FabricPacketType::RTT_RESPONSE)
    {
        // RTTCC echo: turn it into an RTT sample for the rate machine.
        ProcessRttResponsePacket(header);
    }
    else if (header.IsControlPacket())
    {
        ProcessCreditPacket(pkt, header);
    }
    else if (header.IsMemoryPacket())
    {
        ProcessMemoryPacket(pkt, header);
    }
    else
    {
        ProcessDataPacket(pkt, header);
    }
}

void
FabricEndpoint::ProcessDataPacket(Ptr<Packet> packet, const FabricHeader& header)
{
    NS_LOG_FUNCTION(this << packet << header.GetSequenceNumber());

    // Check if this is a protocol-aware packet. The gate is the WIRE-level
    // protocol-framed flag the sender stamped when it framed the payload
    // through a vendor payload builder (SendDataInternal's BuildChunks, the
    // SoHeader stamp in SendBulkWireTransferSize) — NOT an inference from
    // the handling model's efficiency and NOT an NcclProtocol enum
    // interpretation of the protocol byte. Both inferences misfire:
    // (a) efficiency < 1.0 does NOT guarantee the sender framed a header —
    // public SendData, SendP2p, collective uploads, switch multicast results
    // and sub-header-size bulk chunks all send raw under an efficiency<1.0
    // model, and the receiver would strip (RoCE: a 10 B SoHeader deserialize
    // off a 9 B payload aborts; UB: a 16 B strip delivers 64 B as 48 B);
    // (b) vendor protocol ids are not NCCL ids — UB's MEM_SYNC is 0 (NCCL
    // NONE) yet frames a 16-byte transaction header. The flag is set exactly
    // where a builder header is stamped, so send and receive gates provably
    // match; unframed packets keep the raw path regardless of model.
    if (header.IsProtocolFramed())
    {
        // Delegate to protocol-aware processing
        ProcessProtocolDataPacket(packet, header);
        return;
    }

    uint8_t vcId = header.GetVirtualChannel();
    uint16_t flowId = header.GetFlowId();

    // Bypass reorder buffer for direct links (no multi-path reordering needed)
    if (m_bypassReorderBuffer)
    {
        DeliverBypassDataPacket(packet, header);
    }
    else
    {
        Ptr<ReorderBuffer> rb = GetOrCreateReorderBuffer(vcId, flowId,
                                                         header.GetSourceRank());
        bool inserted = rb->Insert(header.GetSequenceNumber(), packet, header);
        rb->DeliverReadyPackets();
        if (!inserted && rb->IsDuplicate(header.GetSequenceNumber()))
        {
            // A rejected DUPLICATE (retransmission of an already-delivered
            // seq, or of one still parked in the buffer): the original
            // delivery's ACK may have been lost, and without this re-ACK
            // the sender's LLR store entry is never purged — the
            // retry-timer backstop retransmits forever and the entry turns
            // into a false permanent loss for data the receiver already
            // holds. Mirrors the bypass path's duplicate branch. A
            // capacity-rejected FUTURE seq is not a duplicate: it rides
            // the sender's retry timer instead (ACKing it would purge the
            // store entry for data the receiver never got).
            SendLlrAckPacket(header, false);
        }
    }

}

bool
FabricEndpoint::DeliverBypassDataPacket(Ptr<Packet> packet, const FabricHeader& header)
{
    uint8_t vcId = header.GetVirtualChannel();
    uint16_t flowId = header.GetFlowId();
    uint64_t flowKey = (static_cast<uint64_t>(vcId) << 48) |
                       (static_cast<uint64_t>(flowId) << 32) |
                       header.GetSourceRank();

    // Strict So go-back-N: in-order-only acceptance, like a real IB
    // responder in GBN mode. Exactly one NAK per gap; everything after
    // the gap is discarded (no delivery, no ACK, no state), so blanket
    // retransmits are real work, not cheap duplicates. Lost NAKs are
    // covered by the sender's retry-timer backstop.
    if (IsStrictGbnFlow(header))
    {
        uint32_t seq = header.GetSequenceNumber();
        uint32_t& expected = m_strictNextExpected[flowKey];
        if (seq == expected)
        {
            expected++;
            // Consume any permanently-lost seqs immediately following
            auto pl = m_strictPermLost.find(flowKey);
            if (pl != m_strictPermLost.end())
            {
                while (pl->second.erase(expected) > 0)
                {
                    expected++;
                }
                if (pl->second.empty())
                {
                    m_strictPermLost.erase(pl);
                }
            }
            m_strictNakPending.erase(flowKey);
            OnPacketDelivered(packet, seq, header);
        }
        else if (seq < expected)
        {
            // Duplicate of an already-delivered seq: its ACK may have
            // been lost, so re-ACK (So flows owe no credit, task #29).
            SendLlrAckPacket(header, false);
        }
        else
        {
            // Gap: discard, and NAK the missing seq once per gap.
            m_bypassReorderEvents++;
            auto nak = m_strictNakPending.find(flowKey);
            if (nak == m_strictNakPending.end() || nak->second != expected)
            {
                m_strictNakPending[flowKey] = expected;
                SendStrictGbnNak(header, expected);
            }
        }
        return true;
    }

    // Track out-of-order arrivals even in bypass mode
    auto it = m_bypassNextExpectedSeq.find(flowKey);
    if (it != m_bypassNextExpectedSeq.end())
    {
        if (header.GetSequenceNumber() != it->second)
        {
            m_bypassReorderEvents++;
        }
    }
    m_bypassNextExpectedSeq[flowKey] = header.GetSequenceNumber() + 1;

    // Skip duplicate delivery from GBN retransmission
    auto deliveredKey = std::make_pair(flowKey, header.GetSequenceNumber());
    if (m_bypassDelivered.count(deliveredKey))
    {
        NS_LOG_DEBUG("Bypass mode: skipping duplicate seq=" << header.GetSequenceNumber()
                     << " flowKey=" << flowKey);
        SendLlrAckPacket(header, false);
        // Still return credit for the retransmitted packet — but only if the
        // send consumed one. OnPacketDelivered's credit return is guarded by
        // IsLocalRank(sourceRank) (So sends bypass the gate), so the duplicate
        // branch must mirror it or an So retransmit duplicate would grow the
        // sender's VC pool it never drew from.
        if (IsLocalRank(header.GetSourceRank()))
        {
            FabricPacketType pktType = header.GetPacketType();
            if (pktType == FabricPacketType::DATA ||
                pktType == FabricPacketType::P2P ||
                header.IsCollectivePacket())
            {
                SendCreditPacket(header.GetSourceRank(),
                                  vcId,
                                  1,
                                  header.GetSequenceNumber(),
                                  header.GetFlowId());
            }
        }
        return true;
    }
    m_bypassDelivered.insert(deliveredKey);

    // Deliver packet directly to callback
    OnPacketDelivered(packet, header.GetSequenceNumber(), header);
    return true;
}

void
FabricEndpoint::NotifyPermanentLoss(FabricHeader originalHeader)
{
    uint32_t seqNum = originalHeader.GetSequenceNumber();
    uint16_t destRank = originalHeader.GetDestRank();
    uint8_t vcId = originalHeader.GetVirtualChannel();

    NS_LOG_FUNCTION(this << seqNum << destRank << static_cast<int>(vcId));

    // A permanently lost chunk will never be ACKed: free its TX-window slot
    // so the window can't leak closed. Gate on window MEMBERSHIP, not just
    // m_soTxWindow > 0: local (Su) packets and small unfragmented remote
    // sends also suffer permanent losses, but they never held a window slot
    // (only PumpBulkTxWindow dispatches occupy one) — releasing for them
    // would dispatch an extra bulk fragment while one is still in flight.
    // The store entry is already purged by the LlrManager before this
    // callback fires, so a late ACK can't double-release.
    if (m_soTxWindow > 0 &&
        m_bulkTxWindowSeqs.erase({destRank, vcId,
                                  originalHeader.GetFlowId(), seqNum}) > 0)
    {
        ReleaseBulkTxSlot();
    }

    // Return the consumed credit for the permanently lost packet (sender-side).
    // Only if the send actually consumed one: So (remote-dest) bulk packets
    // bypass the credit gate (SendBulkWireTransferSize's IsLocalRank guard),
    // so returning a credit for one would grow the pool it never drew from
    // and let an unrelated local send through early. IsLocalRank(destRank) is
    // the mirror of the send-side guard; single-node (m_localRankCount==1)
    // keeps every dest local -> unchanged.
    if (m_creditManager && IsLocalRank(destRank))
    {
        // Credit-bypass types (NVLS/SHARP uploads) consumed no credit on
        // send — none is owed back here (mirror of the ProcessAckPacket gate).
        bool creditBypass = originalHeader.GetPacketType() == FabricPacketType::ALLREDUCE ||
                            originalHeader.GetPacketType() == FabricPacketType::ALLGATHER;
        if (!creditBypass)
        {
            // Record the return in the shared CREDIT/ACK ledger (same key space
            // as the ProcessAckPacket recovery): the receiver still emits a
            // CREDIT for this sequence number if the packet arrives late (after
            // the retry limit declared it permanently lost), and without a
            // ledger entry that late CREDIT would return a second credit for a
            // slot that was already freed here — growing the pool beyond total.
            // insert().second false means this seq was already accounted (e.g.
            // the ACK raced ahead of the permanent-loss declaration).
            uint64_t ackKey = MakeSeqKey(originalHeader.GetDestRank(), vcId,
                                         originalHeader.GetFlowId());
            if (m_creditsReturned.insert({ackKey, seqNum}).second)
            {
                m_creditManager->ReturnCredits(vcId, 1);
                NS_LOG_DEBUG("Returned credit for permanently lost seqNum " << seqNum
                             << " on VC " << static_cast<int>(vcId));
            }
        }
    }

    // Notify the receiver to advance its reorder buffer past the lost sequence number.
    // Send a PERMANENT_LOSS control packet carrying the original packet's metadata
    // (effectiveDataSize, flowId, etc.) so the receiver can advance collective progress.
    Ptr<Packet> pkt = Create<Packet>();
    FabricHeader header;
    header.SetPacketType(FabricPacketType::PERMANENT_LOSS);
    header.SetFabricType(GetFabricTypeForDest(destRank));
    header.SetVirtualChannel(vcId);
    header.SetSequenceNumber(seqNum);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));
    // Carry original packet metadata for collective progress
    header.SetEffectiveDataSize(originalHeader.GetEffectiveDataSize());
    header.SetFlowId(originalHeader.GetFlowId());
    header.SetPayloadSize(originalHeader.GetPayloadSize());

    pkt->AddHeader(header);

    if (!m_devices.empty())
    {
        uint32_t deviceIndex = ResolveDeviceIndex(destRank);
        SendPacketOnDevice(pkt, deviceIndex, ResolveDestMac(destRank));
    }

    // Try sending queued packets now that credits are available
    TrySendQueuedPackets();
}

void
FabricEndpoint::ProcessRetryPacket(Ptr<Packet> packet, const FabricHeader& header)
{
    NS_LOG_FUNCTION(this << static_cast<int>(header.GetPacketType()));

    FabricPacketType type = header.GetPacketType();

    if (type == FabricPacketType::RETRY_REQUEST)
    {
        // Peer is asking us to retransmit a packet
        uint32_t seqNum = header.GetSequenceNumber();
        uint16_t sourceRank = header.GetSourceRank();
        uint16_t flowId = header.GetFlowId();

        NS_LOG_INFO("Received RETRY_REQUEST for seqNum=" << seqNum << " from rank=" << sourceRank);

        if (m_llrEnabled && m_llrManager)
        {
            // Go-Back-N: retransmit all packets from seqNum onwards for this peer
            std::vector<LlrManager::Retransmission> retransmissions =
                m_llrManager->HandleRetryRequest(seqNum, sourceRank, flowId,
                                                 header.GetVirtualChannel());
            for (const auto& retransmission : retransmissions)
            {
                if (retransmission.readyDelay > Time(0))
                {
                    Simulator::Schedule(retransmission.readyDelay,
                                        [this, retransmission]() {
                                            SendRetransmission(retransmission);
                                        });
                }
                else
                {
                    SendRetransmission(retransmission);
                }
            }
        }
    }
    else if (type == FabricPacketType::RETRY_ACK)
    {
        // Peer confirmed successful receipt — remove from retry buffer
        uint32_t seqNum = header.GetSequenceNumber();
        uint16_t flowId = header.GetFlowId();
        uint16_t sourceRank = header.GetSourceRank();

        NS_LOG_INFO("Received RETRY_ACK for seqNum=" << seqNum);

        if (m_llrEnabled && m_llrManager)
        {
            // The VC-qualified key: without it the call defaults to VC0 and a
            // VC0/VC1 pair with matching (seq, flow) would purge the wrong
            // VC's recovery state.
            m_llrManager->RemovePacketsUpTo(seqNum, sourceRank, flowId,
                                            header.GetVirtualChannel());
        }
    }
}

void
FabricEndpoint::ProcessNackPacket(Ptr<Packet> packet, const FabricHeader& header)
{
    NS_LOG_FUNCTION(this << header.GetSequenceNumber());

    uint32_t seqNum = header.GetSequenceNumber();
    uint16_t sourceRank = header.GetSourceRank();
    uint16_t flowId = header.GetFlowId();
    uint8_t vcId = header.GetVirtualChannel();

    NS_LOG_INFO("Received NACK for seqNum=" << seqNum << " from rank=" << sourceRank);

    if (m_llrEnabled && m_llrManager)
    {
        std::vector<LlrManager::Retransmission> retransmissions;

        if (m_llrManager->GetLlrMode() == LlrMode::SACK)
        {
            // SACK: retransmit only the NACKed packet
            std::unordered_set<uint32_t> nackSet{seqNum};
            retransmissions = m_llrManager->HandleSackRequest(nackSet, sourceRank,
                                                              flowId, vcId);
        }
        else
        {
            // Go-Back-N: retransmit from NACK point onward
            retransmissions = m_llrManager->HandleRetryRequest(seqNum, sourceRank,
                                                               flowId, vcId);
        }

        DispatchRetransmissions(retransmissions);
    }
}

void
FabricEndpoint::DispatchRetransmissions(
    const std::vector<LlrManager::Retransmission>& retransmissions)
{
    for (const auto& retransmission : retransmissions)
    {
        if (retransmission.readyDelay > Time(0))
        {
            Simulator::Schedule(retransmission.readyDelay,
                                [this, retransmission]() {
                                    SendRetransmission(retransmission);
                                });
        }
        else
        {
            SendRetransmission(retransmission);
        }
    }
}

void
FabricEndpoint::EnableLlrTimeoutRetransmit()
{
    if (m_llrManager)
    {
        m_llrManager->SetTimeoutCallback(
            MakeCallback(&FabricEndpoint::OnLlrRetryTimeout, this));
    }
}

void
FabricEndpoint::OnLlrRetryTimeout(uint32_t seqNum, uint16_t destRank,
                                  uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << seqNum << destRank << flowId << static_cast<int>(vcId));

    if (!m_llrEnabled || !m_llrManager)
    {
        return;
    }

    // Sender-side timeout backstop: the entry is still un-ACKed when its
    // retry timer fires, so retransmit it exactly as a NACK would have.
    // The Handle*Request paths re-arm the timer and enforce the retry limit,
    // so a persistently-lost packet still degrades to permanent loss instead
    // of looping forever. destRank plays the role of the NACK's sourceRank
    // (both name the flow's remote end); vcId is the timed entry's VC.
    std::vector<LlrManager::Retransmission> retransmissions;
    if (m_llrManager->GetLlrMode() == LlrMode::SACK)
    {
        std::unordered_set<uint32_t> nackSet{seqNum};
        retransmissions = m_llrManager->HandleSackRequest(nackSet, destRank,
                                                          flowId, vcId);
    }
    else
    {
        retransmissions = m_llrManager->HandleRetryRequest(seqNum, destRank,
                                                           flowId, vcId);
    }
    DispatchRetransmissions(retransmissions);
}

void
FabricEndpoint::SendRetransmission(LlrManager::Retransmission retransmission)
{
    if (!retransmission.packet || !m_llrManager)
    {
        return;
    }

    Ptr<Packet> packet = retransmission.packet->Copy();
    FabricHeader oldHeader;
    packet->PeekHeader(oldHeader);
    uint32_t seqNum = oldHeader.GetSequenceNumber();
    uint16_t destRank = oldHeader.GetDestRank();
    uint16_t flowId = oldHeader.GetFlowId();

    if (!m_llrManager->HasOutstandingPacket(seqNum, destRank, flowId,
                                            oldHeader.GetVirtualChannel()))
    {
        return;
    }
    if (retransmission.sourceReload)
    {
        m_llrManager->StorePacket(seqNum, packet, destRank);
    }

    packet->RemoveHeader(oldHeader);
    FabricHeader retryHeader;
    retryHeader.SetPacketType(oldHeader.GetPacketType());
    retryHeader.SetFabricType(GetFabricTypeForDest(destRank));
    retryHeader.SetSourceRank(m_rank);
    retryHeader.SetDestRank(destRank);
    retryHeader.SetSequenceNumber(seqNum);
    retryHeader.SetFlowId(flowId);
    retryHeader.SetVirtualChannel(oldHeader.GetVirtualChannel());
    retryHeader.SetSourceMac(m_address);
    retryHeader.SetDestMac(ResolveDestMac(destRank));
    retryHeader.SetPayloadSize(oldHeader.GetPayloadSize());
    retryHeader.SetEffectiveDataSize(oldHeader.GetEffectiveDataSize());
    retryHeader.SetProtocol(oldHeader.GetProtocol());
    retryHeader.SetProtocolFramed(oldHeader.IsProtocolFramed());
    retryHeader.SetTtl(64);
    packet->AddHeader(retryHeader);

    m_isRetransmitting = true;
    uint32_t deviceIndex = ResolveDeviceIndex(destRank);
    SendPacketOnDevice(packet, deviceIndex, ResolveDestMac(destRank));
    m_isRetransmitting = false;
}

void
FabricEndpoint::ProcessAckPacket(Ptr<Packet> packet, const FabricHeader& header)
{
    NS_LOG_FUNCTION(this << packet << header.GetSequenceNumber());
    if (m_llrEnabled && m_llrManager)
    {
        // Release a TX-window slot exactly once per chunk: the exact
        // window-membership set (destRank, seqNum) says whether this ACK is
        // for a PumpBulkTxWindow-dispatched bulk fragment. The old
        // remoteness + LLR-outstanding heuristic also matched small
        // unfragmented remote sends (they share the LLR store but never
        // held a window slot), releasing a slot that belonged to an
        // in-flight bulk fragment — with soTxWindow=1 the next fragment
        // dispatched while the previous one was still outstanding. The key
        // carries the full (dest, vc, flow) identity because seq counters
        // are allocated per (dest, vc, flow): an ACK from an unwindowed
        // flow sharing only (dest, seq) must not release another flow's
        // bulk slot.
        const bool heldWindowSlot =
            m_soTxWindow > 0 &&
            m_bulkTxWindowSeqs.erase({header.GetSourceRank(),
                                      header.GetVirtualChannel(),
                                      header.GetFlowId(),
                                      header.GetSequenceNumber()}) > 0;
        // Peek the stored entry's type BEFORE RemovePacket destroys it:
        // credit-bypass types (NVLS/SHARP uploads — the switch's LLR ack for
        // them lands here too) consumed no credit on send, so none is owed
        // back; returning one would grow the VC pool past its total.
        uint8_t storedType = 0;
        const bool creditBypassEntry =
            !m_llrManager->PeekOutstandingPacketType(header.GetSequenceNumber(),
                                                     header.GetSourceRank(),
                                                     header.GetFlowId(),
                                                     header.GetVirtualChannel(),
                                                     storedType) ||
            storedType == static_cast<uint8_t>(FabricPacketType::ALLREDUCE) ||
            storedType == static_cast<uint8_t>(FabricPacketType::ALLGATHER);
        const bool removed =
            m_llrManager->RemovePacket(header.GetSequenceNumber(),
                                       header.GetSourceRank(),
                                       header.GetFlowId(),
                                       header.GetVirtualChannel());
        if (heldWindowSlot)
        {
            ReleaseBulkTxSlot();
        }
        if (removed && !creditBypassEntry)
        {
            // R9#2: the ACK proves the receiver got the packet, so the send's
            // consumed credit is owed back even if the receiver's CREDIT
            // packet was lost in flight (CREDIT is BER-exempt but still drawn
            // against the loss rate). Without this backstop the ACK purges
            // the retry entry — nothing ever re-sends that CREDIT — and the
            // VC permanently loses the credit (the next send on the VC
            // queues forever). The insert shares m_creditsReturned with
            // ProcessCreditPacket so the credit is returned EXACTLY once per
            // seq whichever arrives first: ACK first marks the seq credited
            // and the receiver's genuine CREDIT lands in the phantom bucket;
            // CREDIT first returns it and the ACK-side insert fails. A
            // duplicate ACK (removed=false) never reaches here. So
            // (remote-dest) sends never consumed a credit, so only local
            // dests are owed one — mirror of every send-side guard.
            uint64_t ackKey = MakeSeqKey(header.GetSourceRank(),
                                         header.GetVirtualChannel(),
                                         header.GetFlowId());
            if (IsLocalRank(header.GetSourceRank()) &&
                m_creditsReturned.insert({ackKey,
                                          header.GetSequenceNumber()}).second)
            {
                m_creditManager->ReturnCredits(header.GetVirtualChannel(), 1);
                NS_LOG_DEBUG("ACK-side credit recovery for seq "
                             << header.GetSequenceNumber() << " vc="
                             << static_cast<int>(header.GetVirtualChannel()));
            }
        }
    }
}

void
FabricEndpoint::ProcessCreditPacket(Ptr<Packet> packet, const FabricHeader& header)
{
    NS_LOG_FUNCTION(this << packet << header.GetCreditCount());

    uint8_t vcId = header.GetVirtualChannel();
    uint16_t credits = header.GetCreditCount();

    // Dedup per seq: the receiver re-credits a duplicate delivery (the
    // duplicate branch of DeliverBypassDataPacket / the MEMORY_* path) in
    // case the original CREDIT was lost — CREDIT is BER-exempt but still
    // drawn against the loss rate. When the original CREDIT also arrives,
    // the duplicate's credit is phantom: the send consumed exactly one
    // credit and the first CREDIT already returned it. Counting both grows
    // the VC pool beyond its budget and releases queued packets early.
    // The first CREDIT for a seq wins; a genuinely lost CREDIT never
    // enters the set, so its late twin is still accepted.
    uint64_t ackKey = MakeSeqKey(header.GetSourceRank(),
                                 header.GetVirtualChannel(),
                                 header.GetFlowId());
    bool phantomCredit = false;
    if (!m_creditsReturned.insert({ackKey, header.GetSequenceNumber()}).second)
    {
        phantomCredit = true;
        NS_LOG_DEBUG("Ignoring duplicate credit for seq="
                     << header.GetSequenceNumber() << " vc="
                     << static_cast<int>(vcId));
    }

    if (!phantomCredit)
    {
        m_creditManager->ReturnCredits(vcId, credits);
    }
    if (m_llrEnabled && m_llrManager)
    {
        if (m_llrManager->GetLlrMode() == LlrMode::SACK)
        {
            m_llrManager->RemovePacket(header.GetSequenceNumber(),
                                       header.GetSourceRank(),
                                       header.GetFlowId(),
                                       header.GetVirtualChannel());
        }
        else
        {
            uint64_t ackKey = MakeSeqKey(header.GetSourceRank(),
                                         header.GetVirtualChannel(),
                                         header.GetFlowId());
            uint32_t& nextAck = m_gbnNextAckSeq[ackKey];
            auto& pendingAcks = m_gbnPendingAcks[ackKey];
            pendingAcks.insert(header.GetSequenceNumber());

            bool advanced = false;
            while (pendingAcks.erase(nextAck) > 0)
            {
                nextAck++;
                advanced = true;
            }
            if (advanced)
            {
                m_llrManager->RemovePacketsUpTo(nextAck - 1,
                                                header.GetSourceRank(),
                                                header.GetFlowId(),
                                                header.GetVirtualChannel());
            }
        }
    }
    TrySendQueuedPackets();
}

void
FabricEndpoint::ProcessMemoryPacket(Ptr<Packet> packet, const FabricHeader& header)
{
    NS_LOG_FUNCTION(this << static_cast<int>(header.GetPacketType()));

    FabricPacketType type = header.GetPacketType();

    // Duplicate suppression for ALL memory packet types, not just responses:
    // a retransmitted MEMORY_WRITE whose ACK was lost must not execute a
    // second time (write 1, lose its ACK, write 2 — the retry would revert
    // memory back to 1), and a retransmitted MEMORY_READ must not schedule a
    // second response (the fresh response sequence number would defeat the
    // receiver-side response dedup below). A duplicate is still ACKed and
    // credit-returned at the bottom of this function so the sender purges
    // its LLR store entry — only the execution is skipped.
    uint64_t flowKey = (static_cast<uint64_t>(header.GetVirtualChannel()) << 48) |
                       (static_cast<uint64_t>(header.GetFlowId()) << 32) |
                       header.GetSourceRank();
    auto deliveredKey = std::make_pair(flowKey, header.GetSequenceNumber());
    bool duplicate = !m_memoryDelivered.insert(deliveredKey).second;
    if (duplicate)
    {
        NS_LOG_DEBUG("Memory packet duplicate: type="
                     << static_cast<int>(type) << " seq="
                     << header.GetSequenceNumber() << " — ACK but don't execute");
    }

    if (!duplicate && type == FabricPacketType::MEMORY_READ)
    {
        // Extract address and size from payload
        uint32_t pktSize = packet->GetSize();
        if (pktSize < 12)
        {
            NS_LOG_ERROR("Memory read packet too small: " << pktSize);
            return;
        }

        uint8_t buf[12];
        packet->CopyData(buf, 12);
        uint64_t address;
        uint32_t readSize;
        std::memcpy(&address, buf, 8);
        std::memcpy(&readSize, buf + 8, 4);

        // Notify via callback for non-MEMORY endpoints
        if (!m_memoryRequestCallback.IsNull())
        {
            m_memoryRequestCallback(header.GetSourceRank(), address, readSize);
        }

        // For MEMORY endpoints, handle locally (direct service with fixed latency)
        if (GetDeviceType() == DeviceType::MEMORY)
        {
            uint32_t availSize = (address < m_memoryData.size())
                                     ? std::min(readSize, static_cast<uint32_t>(m_memoryData.size() - address))
                                     : 0;
            const uint8_t* dataPtr = (availSize > 0) ? m_memoryData.data() + address : nullptr;
            Simulator::Schedule(m_memoryLatency,
                                &FabricEndpoint::SendMemoryResponse,
                                this, header.GetSourceRank(),
                                dataPtr, availSize);
        }
    }
    else if (!duplicate && type == FabricPacketType::MEMORY_WRITE)
    {
        uint32_t pktSize = packet->GetSize();
        if (pktSize < 8)
        {
            NS_LOG_ERROR("Memory write packet too small: " << pktSize);
            return;
        }

        uint8_t addrBuf[8];
        packet->CopyData(addrBuf, 8);
        uint64_t address;
        std::memcpy(&address, addrBuf, 8);

        if (GetDeviceType() == DeviceType::MEMORY)
        {
            uint32_t writeSize = pktSize - 8;

            // Direct write to backing store. The bounds check must not form
            // address + writeSize: at address = UINT64_MAX-7 with a 16-byte
            // write the sum wraps to 8, passes the check, and memcpy writes
            // eight bytes BEFORE the allocation (ASan heap-buffer-overflow).
            if (!m_memoryData.empty() && address <= m_memoryData.size() &&
                writeSize <= m_memoryData.size() - address)
            {
                std::vector<uint8_t> writeData(pktSize);
                packet->CopyData(writeData.data(), pktSize);
                std::memcpy(m_memoryData.data() + address,
                            writeData.data() + 8, writeSize);
            }
        }
    }
    else if (!duplicate && type == FabricPacketType::MEMORY_RESP)
    {
        // (Duplicate suppression already ran at the top of this function —
        // GO_BACK_N's blanket retransmit can redeliver a response whose
        // sibling was also outstanding, and the memory delivery path has no
        // reorder buffer to swallow it. A duplicate is ACKed below but its
        // payload reaches the application only once.)
        if (!m_memoryResponseCallback.IsNull())
        {
            m_memoryResponseCallback(m_rank, packet, header);
        }
        else if (!m_receiveCallback.IsNull())
        {
            m_receiveCallback(m_rank, packet, header);
        }
    }

    // Return credits for MEMORY_READ, MEMORY_WRITE, and MEMORY_RESP — only
    // when the sender actually consumed one. Local (Su) sources consume a
    // credit on send; remote (So) memory packets bypass the credit gate
    // (SendMemoryWrite/SendMemoryResponse's IsLocalRank guard), so returning
    // a credit for one would grow the remote sender's pool it never drew
    // from — and an over-returned credit releases an unrelated queued local
    // packet early. IsLocalRank(sourceRank) is the mirror of the send-side
    // guards; single-node endpoints keep every source local -> unchanged.
    if (type == FabricPacketType::MEMORY_READ ||
        type == FabricPacketType::MEMORY_WRITE ||
        type == FabricPacketType::MEMORY_RESP)
    {
        if (IsLocalRank(header.GetSourceRank()))
        {
            uint8_t vcId = header.GetVirtualChannel();
            SendCreditPacket(header.GetSourceRank(),
                             vcId,
                             1,
                             header.GetSequenceNumber(),
                             header.GetFlowId());
        }
    }

    // LLR ACK for every memory packet: unlike DATA (whose delivery paths all
    // funnel through OnPacketDelivered -> SendLlrAckPacket), memory packets
    // are processed here directly. The sender stores memory packets in its
    // LLR retry buffer (SendPacketOnDevice's store filter passes them), so
    // without an ACK the store entry is never purged: the retry-timer
    // backstop retransmits forever, duplicating the delivery at every
    // timeout, and the entry degrades into a false permanent loss once the
    // retry limit is hit. SendLlrAckPacket is a no-op when LLR is off, so
    // non-LLR endpoints are unchanged.
    SendLlrAckPacket(header, false);
}

void
FabricEndpoint::SendCreditPacket(uint16_t destRank, uint8_t vcId, uint16_t count,
                                 uint32_t seqNum, uint16_t flowId)
{
    NS_LOG_FUNCTION(this << destRank << vcId << count);

    Ptr<Packet> packet = Create<Packet>();

    FabricHeader header;
    header.SetPacketType(FabricPacketType::CREDIT);
    header.SetFabricType(GetFabricTypeForDest(destRank));
    header.SetVirtualChannel(vcId);
    header.SetCreditCount(count);
    header.SetSequenceNumber(seqNum);
    header.SetFlowId(flowId);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));

    packet->AddHeader(header);

    if (!m_devices.empty())
    {
        uint32_t deviceIndex = ResolveDeviceIndex(destRank);
        SendPacketOnDevice(packet, deviceIndex, ResolveDestMac(destRank));
    }
}

void
FabricEndpoint::SendLlrAckPacket(const FabricHeader& receivedHeader, bool cumulative)
{
    if (!m_llrEnabled || !m_llrManager || m_devices.empty())
    {
        return;
    }

    // Never ack a switch-sourced multicast result (SourceRank 0xFFFF): the
    // switch runs no LLR manager, so nobody consumes the ack — and the ack
    // resolves the sentinel rank to the broadcast MAC, which the switch
    // floods to every port. Each flooded copy lands on endpoints that match
    // nothing (retry misses) while burning egress on all links — with LLR
    // on, an 8-GPU NVLS allgather amplified 64 result deliveries into ~1700
    // switch transmissions even at zero loss. Lost results are recovered by
    // the receiver's NACK (DestRank 0xFFFF), which the switch intercepts and
    // services from its own result store — not by acks.
    if (receivedHeader.GetSourceRank() == 0xFFFF)
    {
        return;
    }

    Ptr<Packet> packet = Create<Packet>();
    FabricHeader header;
    header.SetPacketType(cumulative ? FabricPacketType::RETRY_ACK
                                    : FabricPacketType::ACK);
    header.SetFabricType(GetFabricTypeForDest(receivedHeader.GetSourceRank()));
    header.SetVirtualChannel(receivedHeader.GetVirtualChannel());
    header.SetSequenceNumber(receivedHeader.GetSequenceNumber());
    header.SetFlowId(receivedHeader.GetFlowId());
    header.SetSourceRank(m_rank);
    header.SetDestRank(receivedHeader.GetSourceRank());
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(receivedHeader.GetSourceRank()));
    packet->AddHeader(header);

    uint32_t deviceIndex = ResolveDeviceIndex(receivedHeader.GetSourceRank());
    SendPacketOnDevice(packet, deviceIndex, ResolveDestMac(receivedHeader.GetSourceRank()));
}

void
FabricEndpoint::SendCnpPacket(const FabricHeader& markedHeader)
{
    NS_LOG_FUNCTION(this << markedHeader.GetSourceRank());

    // A CNP is a control echo: it tells the source that a packet it sent was
    // ECN-marked in flight. It carries no data and rides the same lossless
    // control path as CREDIT/ACK (control packets bypass BER sampling and
    // ECN marking, so CNP feedback cannot deadlock).
    Ptr<Packet> packet = Create<Packet>();
    FabricHeader header;
    header.SetPacketType(FabricPacketType::CNP);
    header.SetFabricType(GetFabricTypeForDest(markedHeader.GetSourceRank()));
    header.SetVirtualChannel(markedHeader.GetVirtualChannel());
    header.SetSequenceNumber(markedHeader.GetSequenceNumber());
    header.SetFlowId(markedHeader.GetFlowId());
    header.SetSourceRank(m_rank);
    header.SetDestRank(markedHeader.GetSourceRank());
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(markedHeader.GetSourceRank()));
    packet->AddHeader(header);
    m_cnpPacketsSent++;

    if (!m_devices.empty())
    {
        uint32_t deviceIndex = ResolveDeviceIndex(markedHeader.GetSourceRank());
        SendPacketOnDevice(packet, deviceIndex,
                           ResolveDestMac(markedHeader.GetSourceRank()));
    }
}

void
FabricEndpoint::ProcessCnpPacket(const FabricHeader& cnpHeader)
{
    NS_LOG_FUNCTION(this << cnpHeader.GetSourceRank());
    m_cnpPacketsReceived++;

    // A CNP echoes the original flow back to its source. The CNP's source is
    // the receiver, so at the source endpoint the flow key (destRank=receiver,
    // vc, flowId) maps to MakeSeqKey(cnpHeader.GetSourceRank(), vc, flowId).
    if (!m_dcqcnRate)
    {
        // Dormant baseline: no machine mounted (--dcqcn off). CNP is a no-op.
        return;
    }
    uint64_t flowKey = MakeSeqKey(cnpHeader.GetSourceRank(),
                                 cnpHeader.GetVirtualChannel(),
                                 cnpHeader.GetFlowId());
    m_dcqcnRate->OnCnp(flowKey);
}

void
FabricEndpoint::SetDcqcnRateMachine(Ptr<DcqcnRateMachine> machine)
{
    m_dcqcnRate = machine;
}

Ptr<DcqcnRateMachine>
FabricEndpoint::GetDcqcnRateMachine() const
{
    return m_dcqcnRate;
}

void
FabricEndpoint::SendRttProbePacket(uint16_t destRank, uint8_t vcId, uint16_t flowId,
                                   uint32_t probeSeq)
{
    NS_LOG_FUNCTION(this << destRank << probeSeq);

    // An RTT probe is a control packet: it rides the same VOQ queues and
    // links as the data it accompanies (so the measured RTT includes the
    // queueing the flow experiences) but bypasses BER sampling, lossy
    // tail-drop, LLR retry state, and the rate machines themselves (a probe
    // must never be paced by the rate it is measuring). The send timestamp
    // stays sender-local in the RttccRateMachine, keyed by probeSeq — no
    // timestamp goes on the wire; the response only echoes probeSeq.
    Ptr<Packet> packet = Create<Packet>();
    FabricHeader header;
    header.SetPacketType(FabricPacketType::RTT_PROBE);
    header.SetFabricType(GetFabricTypeForDest(destRank));
    header.SetVirtualChannel(vcId);
    header.SetSequenceNumber(probeSeq);
    header.SetFlowId(flowId);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));
    packet->AddHeader(header);

    if (!m_devices.empty())
    {
        uint32_t deviceIndex = ResolveDeviceIndex(destRank);
        SendPacketOnDevice(packet, deviceIndex, ResolveDestMac(destRank));
    }
}

void
FabricEndpoint::SendRttResponsePacket(const FabricHeader& probeHeader)
{
    NS_LOG_FUNCTION(this << probeHeader.GetSourceRank());

    // Hardware-style immediate reflection: echo the probe's sequence number
    // (and flow coordinates) straight back to the prober. Unconditional — a
    // receiver needs no rate machine to reflect; with --rttcc off no probes
    // exist, so this path never fires on the baseline (parity-safe).
    Ptr<Packet> packet = Create<Packet>();
    FabricHeader header;
    header.SetPacketType(FabricPacketType::RTT_RESPONSE);
    header.SetFabricType(GetFabricTypeForDest(probeHeader.GetSourceRank()));
    header.SetVirtualChannel(probeHeader.GetVirtualChannel());
    header.SetSequenceNumber(probeHeader.GetSequenceNumber());
    header.SetFlowId(probeHeader.GetFlowId());
    header.SetSourceRank(m_rank);
    header.SetDestRank(probeHeader.GetSourceRank());
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(probeHeader.GetSourceRank()));
    packet->AddHeader(header);

    if (!m_devices.empty())
    {
        uint32_t deviceIndex = ResolveDeviceIndex(probeHeader.GetSourceRank());
        SendPacketOnDevice(packet, deviceIndex,
                           ResolveDestMac(probeHeader.GetSourceRank()));
    }
}

void
FabricEndpoint::ProcessRttResponsePacket(const FabricHeader& respHeader)
{
    NS_LOG_FUNCTION(this << respHeader.GetSourceRank());

    // The response's source is the flow's receiver, so at the prober the flow
    // key is MakeSeqKey(respHeader.GetSourceRank(), vc, flowId) — the same
    // key the send path used (mirrors ProcessCnpPacket).
    if (!m_rttccRate)
    {
        return; // dormant baseline: no machine mounted (--rttcc off)
    }
    uint64_t flowKey = MakeSeqKey(respHeader.GetSourceRank(),
                                 respHeader.GetVirtualChannel(),
                                 respHeader.GetFlowId());
    m_rttccRate->OnProbeResponse(flowKey, respHeader.GetSequenceNumber());
}

void
FabricEndpoint::SetRttccRateMachine(Ptr<RttccRateMachine> machine)
{
    m_rttccRate = machine;
}

Ptr<RttccRateMachine>
FabricEndpoint::GetRttccRateMachine() const
{
    return m_rttccRate;
}

void
FabricEndpoint::TrySendQueuedPackets()
{
    NS_LOG_FUNCTION(this);

    while (!m_sendQueue.empty())
    {
        SendQueueEntry& entry = m_sendQueue.front();

        // Credit gate — Su (local-dest) entries only. So (remote-dest) entries
        // bypass credits entirely: the receiver never returns credits for a
        // remote source (OnPacketDelivered's IsLocalRank guard), so consuming
        // one here would strand it. Entries reach this queue from send paths
        // whose own gates were bypassed under the same predicate, so the split
        // here mirrors the send side exactly.
        const bool destIsLocal = IsLocalRank(entry.header.GetDestRank());
        if (destIsLocal && !m_creditManager->HasCredits(entry.vcId))
        {
            NS_LOG_DEBUG("No credits for VC " << static_cast<int>(entry.vcId));
            break;
        }

        if (destIsLocal && !m_creditManager->ConsumeCredit(entry.vcId, entry.seqNum))
        {
            break;
        }

        Ptr<Packet> pkt = entry.packet->Copy();
        pkt->AddHeader(entry.header);

        if (m_devices.empty())
        {
            NS_LOG_ERROR("No NetDevices configured");
            m_sendQueue.pop();
            continue;
        }

        uint32_t deviceIndex = (entry.deviceIndex != UINT32_MAX)
                                   ? entry.deviceIndex
                                   : ResolveDeviceIndex(entry.header.GetDestRank());
        Mac48Address destMac = ResolveDestMac(entry.header.GetDestRank());
        SendPacketOnDevice(pkt, deviceIndex, destMac);

        m_txTrace(pkt->GetSize(), entry.seqNum);
        m_txPackets++;
        m_txBytes += pkt->GetSize();

        m_sendQueue.pop();
    }
}

void
FabricEndpoint::SendPacketOnDevice(Ptr<Packet> packet, uint32_t deviceIndex,
                                    const Address& dest)
{
    NS_LOG_FUNCTION(this << packet << deviceIndex);

    if (deviceIndex >= m_devices.size())
    {
        NS_LOG_ERROR("Invalid device index " << deviceIndex);
        // The packet will never reach the wire, so ArmRetryTimeoutOnWire
        // below never runs and an LLR source entry queued for it would keep
        // no timeout backstop (the retransmission paths cancel the pending
        // timer until wire transmission). Arm it here as the fallback.
        ArmRetryTimeoutOnWire(packet);
        return;
    }

    // Prune fired deferred-egress events so the tracking vector stays
    // bounded (each egress schedules at most one; DoDispose cancels what is
    // still pending).
    m_deferredEgressEvents.erase(
        std::remove_if(m_deferredEgressEvents.begin(), m_deferredEgressEvents.end(),
                       [](const EventId& ev) { return ev.IsExpired(); }),
        m_deferredEgressEvents.end());

    // DCQCN shaping: fold the per-flow rate-backoff delay into the egress
    // chokepoint. So-only (efficiency < 1.0) data/collective traffic; control,
    // memory, retry, and scale-up traffic are untouched. At alpha=0 (no CNP)
    // ShapingDelay is zero, so the baseline is byte-identical (parity-safe).
    Time shapingDelay = Time(0);
    if (m_dcqcnRate)
    {
        FabricHeader hdr;
        packet->PeekHeader(hdr);
        if (!hdr.IsControlPacket() && !hdr.IsMemoryPacket() && !hdr.IsRetryPacket())
        {
            double eff = GetProtocolModelForDest(hdr.GetDestRank())->GetEfficiency(hdr.GetProtocol());
            if (eff < 1.0)
            {
                uint64_t flowKey = MakeSeqKey(hdr.GetDestRank(),
                                             hdr.GetVirtualChannel(),
                                             hdr.GetFlowId());
                shapingDelay = m_dcqcnRate->ShapingDelay(packet->GetSize(), flowKey);
            }
        }
    }

    // RTTCC shaping: RTT-probe-based rate control for So (inter-node) data
    // traffic, fabric-agnostic — it also covers the plain lossy Ethernet
    // fabric, whose WireEfficiency==1.0 makes the DCQCN eff<1.0 gate above
    // unusable and which has no ECN/CNP loop at all. Gated on the dest being
    // a REMOTE rank (the honest So predicate). The pacer delay composes with
    // DCQCN via max (both are earliest-egress constraints on the same chunk).
    // At the line rate (the initial state, before any over-target RTT sample)
    // ShapingDelay is zero, so the baseline is byte-identical (parity-safe).
    if (m_rttccRate)
    {
        FabricHeader hdr;
        packet->PeekHeader(hdr);
        if (!hdr.IsControlPacket() && !hdr.IsMemoryPacket() && !hdr.IsRetryPacket() &&
            !IsLocalRank(hdr.GetDestRank()))
        {
            uint64_t flowKey = MakeSeqKey(hdr.GetDestRank(),
                                         hdr.GetVirtualChannel(),
                                         hdr.GetFlowId());
            shapingDelay = std::max(shapingDelay,
                                    m_rttccRate->ShapingDelay(packet->GetSize(), flowKey));
            // At most one probe per ProbeInterval rides ahead of the data
            // chunk it accompanies (probe = control packet: the recursive
            // SendPacketOnDevice call skips both rate machines).
            uint32_t probeSeq = 0;
            if (m_rttccRate->MaybeStartProbe(flowKey, probeSeq))
            {
                SendRttProbePacket(hdr.GetDestRank(), hdr.GetVirtualChannel(),
                                   hdr.GetFlowId(), probeSeq);
            }
        }
    }

    // WFQ contention: classify packet and increment backlog counter.
    // The actual WFQ scheduling (service time delay) is applied after
    // LLR/FEC processing so that contended packets still go through
    // the reliability pipeline.
    TrafficClass tc = TrafficClass::NUM_CLASSES;
    if (m_contentionModel)
    {
        FabricHeader hdr;
        packet->PeekHeader(hdr);
        tc = ContentionModel::ClassifyPacket(static_cast<uint8_t>(hdr.GetPacketType()));
        m_contentionModel->IncrementBacklog(tc);
    }

    // Store in LLR retry buffer only for data/collective packets.
    // Control/retry/permanent-loss packets must not pollute retry state.
    // NVLS/SHARP uploads (ALLREDUCE/ALLGATHER) ARE stored: the switch
    // consumes them, so the only recovery for a switch-side BER drop is a
    // retransmission from this endpoint (the switch NACKs the drop and the
    // sender re-sends from this store; the switch also LLR-acks accepted
    // uploads so the entry is purged on success). The old exclusion left a
    // dropped upload unrecoverable — the collective never completed.
    // Credit accounting is unaffected: these types bypass credit on send,
    // and the ack-side recovery gate (IsLocalRank of the ack's source) keeps
    // the credit pool untouched for them.
    if (m_llrEnabled && m_llrManager && !m_isRetransmitting)
    {
        FabricHeader hdr;
        packet->PeekHeader(hdr);
        if (hdr.GetSourceRank() == m_rank &&
            !hdr.IsControlPacket() && !hdr.IsRetryPacket())
        {
            m_llrManager->StorePacket(hdr.GetSequenceNumber(), packet, hdr.GetDestRank());
        }
    }

    // FEC encode: expand packet size to account for FEC parity overhead
    if (LinkUsesFec())
    {
        FabricHeader hdr;
        packet->PeekHeader(hdr);
        uint32_t payloadSize = packet->GetSize() - hdr.GetSerializedSize();
        uint32_t encodedPayloadSize = m_fecModel->GetEncodedSize(payloadSize);
        if (encodedPayloadSize > payloadSize)
        {
            uint32_t padding = encodedPayloadSize - payloadSize;
            packet->AddAtEnd(Create<Packet>(padding));
        }
    }

    // Now apply WFQ scheduling after LLR/FEC processing.
    // When multiple classes have backlog, each gets its weighted share.
    if (m_contentionModel && tc != TrafficClass::NUM_CLASSES)
    {
        uint32_t numActive = m_contentionModel->GetNumActiveClasses();
        if (numActive > 1)
        {
            Time serviceTime = m_contentionModel->ComputeServiceTime(packet->GetSize(), tc);
            Time encodeLatency = LinkUsesFec()
                                     ? m_fecModel->GetEncodeLatency()
                                     : Time(0);
            Time serialization = m_contentionModel->GetSerializationTime(packet->GetSize());
            Time totalDelay = serviceTime + encodeLatency + shapingDelay;
            if (totalDelay > Time(0))
            {
                NS_LOG_DEBUG("WFQ contention: class=" << static_cast<int>(tc)
                             << " numActive=" << numActive
                             << " serviceTime=" << serviceTime.GetNanoSeconds() << "ns"
                             << " encodeLatency=" << encodeLatency.GetNanoSeconds() << "ns");
                m_deferredEgressEvents.push_back(
                    Simulator::Schedule(totalDelay, [this, packet, deviceIndex, dest, tc]() {
                        Ptr<NetDevice> dev = m_devices[deviceIndex];
                        ArmRetryTimeoutOnWire(packet);
                        dev->Send(packet, dest, 0x0800);
                    }));
                // Decrement backlog after packet finishes transmission
                // (serialization). Tracked like the deferred send itself:
                // the lambda captures raw `this`, so DoDispose must cancel
                // it or it fires on freed memory (codex round 14, finding 1).
                Time txDone = totalDelay + serialization;
                m_deferredEgressEvents.push_back(
                    Simulator::Schedule(txDone, [this, tc]() {
                        m_contentionModel->DecrementBacklog(tc);
                    }));
                return;
            }
        }
    }

    // No WFQ contention or single class: apply FEC encode latency only
    if (LinkUsesFec())
    {
        Time encodeLatency = m_fecModel->GetEncodeLatency();
        Time delay = encodeLatency + shapingDelay;
        if (delay > Seconds(0))
        {
            m_deferredEgressEvents.push_back(
                Simulator::Schedule(delay, [this, packet, deviceIndex, dest]() {
                    Ptr<NetDevice> device = m_devices[deviceIndex];
                    ArmRetryTimeoutOnWire(packet);
                    device->Send(packet, dest, 0x0800);
                }));
            // Decrement backlog after serialization. Tracked for DoDispose
            // like the deferred send above (raw-`this` capture; codex round
            // 14, finding 1).
            if (m_contentionModel && tc != TrafficClass::NUM_CLASSES)
            {
                Time serialization = m_contentionModel->GetSerializationTime(packet->GetSize());
                m_deferredEgressEvents.push_back(
                    Simulator::Schedule(delay + serialization, [this, tc]() {
                        m_contentionModel->DecrementBacklog(tc);
                    }));
            }
            return;
        }
    }

    // Immediate send (no FEC latency, no WFQ contention). DCQCN rate backoff
    // (shapingDelay > 0) defers the egress; otherwise the send is synchronous
    // (parity-identical to the unmounted baseline).
    if (shapingDelay > Time(0))
    {
        m_deferredEgressEvents.push_back(
            Simulator::Schedule(shapingDelay, [this, packet, deviceIndex, dest]() {
                Ptr<NetDevice> dev = m_devices[deviceIndex];
                ArmRetryTimeoutOnWire(packet);
                dev->Send(packet, dest, 0x0800);
            }));
    }
    else
    {
        Ptr<NetDevice> dev = m_devices[deviceIndex];
        ArmRetryTimeoutOnWire(packet);
        dev->Send(packet, dest, 0x0800);
    }
    if (m_contentionModel && tc != TrafficClass::NUM_CLASSES)
    {
        // Tracked for DoDispose like the deferred sends above (raw-`this`
        // capture; codex round 14, finding 1).
        Time serialization = m_contentionModel->GetSerializationTime(packet->GetSize());
        m_deferredEgressEvents.push_back(
            Simulator::Schedule(shapingDelay + serialization, [this, tc]() {
                m_contentionModel->DecrementBacklog(tc);
            }));
    }
}

void
FabricEndpoint::ArmRetryTimeoutOnWire(Ptr<Packet> packet)
{
    NS_LOG_FUNCTION(this << packet);
    // The packet is only now leaving the endpoint. Any LLR retry timer armed
    // at StorePacket (enqueue) time measured the pacing/contention/FEC hold,
    // not the network — re-arm so the timeout window starts at first wire
    // transmission (see ArmRetryTimeoutAtTransmission). No-op for control
    // packets (never stored) and for entries already ACKed.
    if (!m_llrEnabled || !m_llrManager)
    {
        return;
    }
    FabricHeader hdr;
    if (!packet->PeekHeader(hdr) || hdr.IsControlPacket() || hdr.IsRetryPacket())
    {
        return;
    }
    if (hdr.GetSourceRank() != m_rank)
    {
        return;
    }
    m_llrManager->ArmRetryTimeoutAtTransmission(hdr.GetSequenceNumber(),
                                                hdr.GetDestRank(),
                                                hdr.GetFlowId(),
                                                hdr.GetVirtualChannel());
}

uint32_t
FabricEndpoint::GetNextDeviceIndex()
{
    if (m_devices.empty())
    {
        return 0;
    }

    uint32_t index = m_currentDeviceIndex;
    m_currentDeviceIndex = (m_currentDeviceIndex + 1) % m_devices.size();
    return index;
}

void
FabricEndpoint::OnCreditAvailable(uint8_t vcId)
{
    NS_LOG_FUNCTION(this << static_cast<int>(vcId));
    TrySendQueuedPackets();
}

void
FabricEndpoint::OnPacketDelivered(Ptr<Packet> packet, uint32_t seqNum, FabricHeader header)
{
    NS_LOG_FUNCTION(this << packet << seqNum);
    NS_LOG_DEBUG("OnPacketDelivered: rank=" << m_rank << " seq=" << seqNum << " callback null=" << m_receiveCallback.IsNull());

    // Record flow completion in latency statistics
    if (m_latencyStatistics)
    {
        m_latencyStatistics->RecordFlowComplete(header.GetFlowId());
    }

    FabricPacketType packetType = header.GetPacketType();
    SendLlrAckPacket(header, false);
    // Return credits only for intra-node (Su) traffic. Scale-out (So) packets
    // bypassed credit on send (SendBulkWireTransferSize's IsLocalRank guard),
    // so there is no credit owed back. Returning one for an So packet would
    // corrupt the Su credit bookkeeping it never consumed from. The
    // IsLocalRank(sourceRank) predicate is the mirror of the send-side guard;
    // single-node (m_localRankCount==1) keeps all sources local -> unchanged.
    if ((packetType == FabricPacketType::DATA
         || packetType == FabricPacketType::P2P
         || header.IsCollectivePacket())
        && header.GetSourceRank() != 0xFFFF
        && IsLocalRank(header.GetSourceRank()))
    {
        SendCreditPacket(header.GetSourceRank(),
                         header.GetVirtualChannel(),
                         1,
                         header.GetSequenceNumber(),
                         header.GetFlowId());
    }

    if (!m_receiveCallback.IsNull())
    {
        m_receiveCallback(m_rank, packet, header);
    }
}

void
FabricEndpoint::OnPermanentGapDelivered(uint32_t seqNum, FabricHeader header)
{
    NS_LOG_FUNCTION(this << seqNum);
    NS_LOG_DEBUG("OnPermanentGapDelivered: rank=" << m_rank << " seq=" << seqNum
                 << " effectiveDataSize=" << header.GetEffectiveDataSize());

    // Create a synthetic empty packet and invoke the collective receive callback
    // so the collective layer can advance its progress using the original
    // packet's effectiveDataSize.
    if (!m_receiveCallback.IsNull())
    {
        Ptr<Packet> emptyPkt = Create<Packet>(0);
        // Set packet type to PERMANENT_LOSS so the collective layer can distinguish
        // terminal loss from normal DATA delivery
        header.SetPacketType(FabricPacketType::PERMANENT_LOSS);
        m_receiveCallback(m_rank, emptyPkt, header);
    }
}

void
FabricEndpoint::SendDataWithProtocol(uint16_t destRank, const uint8_t* data, uint32_t size,
                                      NcclProtocol protocol, uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << destRank << size << static_cast<int>(protocol) << flowId << vcId);
    SendDataInternal(destRank, data, size, static_cast<uint8_t>(protocol), flowId, vcId);
}

void
FabricEndpoint::SendDataAutoProtocol(uint16_t destRank, const uint8_t* data, uint32_t size,
                                      uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << destRank << size << flowId << vcId);

    uint8_t protocolId = GetProtocolModelForDest(destRank)->GetProtocolId(size);
    SendDataInternal(destRank, data, size, protocolId, flowId, vcId);
}

void
FabricEndpoint::SendDataWithProtocol(uint16_t destRank, const uint8_t* data, uint32_t size,
                                      uint8_t protocolId, uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << destRank << size << static_cast<int>(protocolId) << flowId << vcId);

    SendDataInternal(destRank, data, size, protocolId, flowId, vcId);
}

void
FabricEndpoint::SendBulkWireTransfer(uint16_t destRank, const uint8_t* data, uint32_t dataSize,
                                      uint8_t protocolId, uint16_t flowId, uint8_t vcId)
{
    (void)data;
    SendBulkWireTransferSize(destRank, dataSize, protocolId, flowId, vcId);
}

void
FabricEndpoint::SendBulkWireTransferSize(uint16_t destRank, uint64_t dataSize,
                                          uint8_t protocolId, uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << destRank << dataSize << static_cast<int>(protocolId) << flowId << vcId);

    // So payload-builder seam: when a dedicated So builder is mounted (inline
    // runs opting into the RoCE ECN/DCQCN chain via --dcqcn), remote-dest bulk
    // sends use it together with the So protocol model's own protocol id
    // (RoceProtocol::SIMPLE) so the wire protocol matches the SoHeader framing
    // that the switch ECN-mark and receiver CNP-echo guards key on. Su
    // (local-rank) sends keep the caller's protocol id and the Su builder.
    Ptr<ProtocolPayloadBuilder> payloadBuilder = GetPayloadBuilderForRank(destRank);
    if (payloadBuilder != m_payloadBuilder)
    {
        protocolId = GetProtocolModelForDest(destRank)->GetProtocolId(dataSize);
    }

    const double efficiency = GetProtocolModelForDest(destRank)->GetEfficiency(protocolId);
    const uint64_t wireSize = GetProtocolModelForDest(destRank)->GetWireSize(dataSize, protocolId);

    NS_LOG_INFO("Bulk wire transfer: dataSize=" << dataSize << " wireSize=" << wireSize
               << " efficiency=" << efficiency << " protocol=" << static_cast<int>(protocolId));

    // Seam B (WU-SoH5): activate the dormant SoHeader/ECN/CNP loop on the So
    // (RoCE) fabric. When efficiency < 1.0 (inter-node RoCE) and the mounted
    // payload builder is RocePayloadBuilder, stamp a within-budget SoHeader +
    // SetProtocol(SIMPLE) on each coalesced bulk packet. The 10-byte SoHeader
    // sits WITHIN the wireSize budget (Create(wireChunk-10) + AddHeader(SoHeader)
    // == wireChunk on-wire body), so chunk count and egress serialization are
    // unchanged -> bit-identical at zero congestion (ECN never marks). Su
    // (efficiency == 1.0) and non-RoCE builders stay on SetProtocol(0) with no
    // SoHeader -> dormant, no breakage. This lights up
    // EthernetSwitch::MarkEcnIfCongested (guard GetProtocol()==SIMPLE),
    // SoHeader::PeekEcn, and SendCnpPacket on real So bulk traffic.
    // NOTE: RoceProtocol::SIMPLE(1) collides with NcclProtocol::LL(1) on the
    // receive dispatch; the protocol-framed wire flag stamped only when a
    // SoHeader is actually present keeps Su NVLink LL packets (framed by the
    // Su builder but byte-1) out of the So extraction path, so the two
    // byte-1 populations never cross-fire. Chunks too small to hold the
    // 10-byte header (wireChunk <= soHeaderSize) stay raw + unframed — the
    // receiver delivers them through the raw path instead of aborting on a
    // header deserialize that cannot fit.
    const uint32_t soHeaderSize = SoHeader().GetSerializedSize();
    const bool stampSoHeader = (efficiency < 1.0) && payloadBuilder &&
        (payloadBuilder->GetInstanceTypeId() == RocePayloadBuilder::GetTypeId());

    std::vector<uint32_t> devices = GetPhysicalDevicesForDest(destRank);
    const uint64_t MAX_CHUNK_BYTES = m_bulkChunkSize;
    bool needFragment = (devices.size() > 1) ||
                        (wireSize > MAX_CHUNK_BYTES);

    if (needFragment)
    {
        std::vector<uint32_t> sendDevices = devices;
        if (sendDevices.empty())
        {
            sendDevices.push_back(ResolveDeviceIndex(destRank));
        }

        uint64_t minChunks = sendDevices.size();
        uint64_t sizeBasedChunks = (wireSize + m_sprayChunkSize - 1) / m_sprayChunkSize;
        // Coalesce bulk transfers while retaining at least one chunk per path.
        // Total wire bytes and path striping are unchanged. Queue and credit
        // events occur at this coarser granularity, so this API is reserved for
        // bulk-transfer timing rather than fine-grained contention tests.
        // numChunks = max(one chunk per path, chunks needed to honor the
        // bulk cap): a count below memCapChunks emits fragments above
        // MAX_CHUNK_BYTES, which is how bulkChunkSize < sprayChunkSize broke
        // the cap (58 KiB fragments over a 4 KiB bulk cap). With the default
        // bulkChunkSize (8 MiB) > sprayChunkSize this is one chunk per path,
        // the same coarsening as before. numChunks is bounded above by
        // wireSize: one 1-byte chunk per wire byte — otherwise trailing
        // per-lane fragments compute sizes past the transfer end
        // (wireSize - offset underflows) and emit zero/phantom data bytes.
        uint64_t memCapChunks = (wireSize + MAX_CHUNK_BYTES - 1) / MAX_CHUNK_BYTES;
        uint64_t numChunks = std::max(minChunks, memCapChunks);
        (void)sizeBasedChunks;
        numChunks = std::min(numChunks, std::max<uint64_t>(wireSize, 1));
        uint64_t wireChunkSize = (wireSize + numChunks - 1) / numChunks;
        // Only the first sentChunks fragments are actually emitted (the loops
        // below skip trailing lane-pad fragments once wireOffset >= wireSize).
        // Split the data over the fragments that are sent, not over numChunks:
        // splitting over numChunks can assign trailing data bytes to fragments
        // that are never emitted (dataSize=400 over 8 GPUs -> 50 B ring
        // chunks; LL wireSize=128 over numChunks=18 lanes sends 16 fragments
        // but ceil(50/18)=3 B each -> 48 of 50 effective bytes delivered,
        // stalling the receiver). dataSize <= wireSize (efficiency <= 1)
        // keeps dataChunkSize <= wireChunkSize, so the last sent fragment
        // still fits its wire budget, and skipped fragments get zero data.
        // Identical to the old numChunks split whenever every fragment is
        // sent (the regime all calibrated runs live in).
        uint64_t sentChunks = (wireSize + wireChunkSize - 1) / wireChunkSize;
        uint64_t dataChunkSize = (dataSize + sentChunks - 1) / sentChunks;

        // So TX window (perftest tx_depth analog): bound the number of
        // in-flight chunks instead of dispatching the whole message
        // synchronously at t=0. Chunk descriptors queue here; seqNums are
        // assigned at dispatch (FIFO, so send order == seq order). The window
        // is ACK-clocked: ProcessAckPacket releases a slot when the per-packet
        // LLR ACK purges the retry store. So-only (remote rank, no credit
        // interaction); 0 = legacy unbounded path below (parity default).
        // This also moves StorePacket (and its retry-timer arming) to the
        // window-gated dispatch instant, so the LLR retry timeout can be
        // RTT-scale instead of whole-transfer-scale.
        if (m_soTxWindow > 0 && !IsLocalRank(destRank))
        {
            // Only fragments that still carry data bytes are emitted. The two
            // skips below are load-bearing in opposite directions:
            // - wireOffset >= wireSize: trailing lane-pad fragment past the
            //   wire end (wireSize - wireOffset would underflow).
            // - dataOffset >= dataSize: trailing wire-padding fragment. It
            //   carries zero effective bytes, and a zero-effectiveDataSize
            //   header is NOT inert on the receive side — receivers read
            //   effectiveDataSize==0 as "the whole packet" (protocol-config-
            //   runner.cc / the collective injectors), so a padding-only
            //   fragment over-counts and can complete a transfer early. Its
            //   wire bytes are pure protocol padding; dropping the fragment
            //   keeps the effective-byte ledger exact and the wire smaller.
            //   dataSize <= wireSize guarantees at least one fragment carries
            //   data, so this never skips the whole transfer.
            for (uint64_t i = 0; i < numChunks; i++)
            {
                uint64_t wireOffset = i * wireChunkSize;
                if (wireOffset >= wireSize)
                {
                    continue; // trailing lane-pad fragment past the wire end
                }
                uint64_t dataOffset = i * dataChunkSize;
                if (dataOffset >= dataSize)
                {
                    break; // wire-padding fragment: no data bytes left to send
                }
                BulkTxChunk desc;
                desc.destRank = destRank;
                desc.flowId = flowId;
                desc.vcId = vcId;
                desc.protocolId = protocolId;
                desc.wireChunk = static_cast<uint32_t>(
                    std::min(wireChunkSize, wireSize - wireOffset));
                desc.fragmentDataSize = static_cast<uint32_t>(
                    std::min(dataChunkSize, dataSize - dataOffset));
                desc.chunkIndex = static_cast<uint32_t>(i);
                desc.deviceIndex = sendDevices[(m_globalSprayOffset + i) % sendDevices.size()];
                desc.stamp = stampSoHeader && (desc.wireChunk > soHeaderSize);
                m_bulkTxQueue.push_back(desc);
            }
            m_globalSprayOffset = static_cast<uint32_t>(
                (m_globalSprayOffset + numChunks) % sendDevices.size());
            PumpBulkTxWindow();
            return;
        }

        // Same fragment skips as the windowed path above: stop at the first
        // fragment past the data end (wire padding carries no bytes and a
        // zero-effectiveDataSize header over-counts on the receive side), and
        // skip fragments past the wire end.
        for (uint64_t i = 0; i < numChunks; i++)
        {
            uint64_t wireOffset = i * wireChunkSize;
            if (wireOffset >= wireSize)
            {
                continue; // trailing lane-pad fragment past the transfer end
            }
            uint32_t thisWireChunk = static_cast<uint32_t>(
                std::min(wireChunkSize, wireSize - wireOffset));

            // Ensure fragment effectiveDataSize sums to original dataSize
            uint64_t dataOffset = i * dataChunkSize;
            if (dataOffset >= dataSize)
            {
                break; // wire-padding fragment: no data bytes left to send
            }
            uint32_t fragmentDataSize = static_cast<uint32_t>(
                std::min(dataChunkSize, dataSize - dataOffset));

            uint64_t seqKey = MakeSeqKey(destRank, vcId, flowId);
            uint32_t seqNum = m_nextSeqNum[seqKey]++;

            const bool stamp = stampSoHeader && (thisWireChunk > soHeaderSize);
            Ptr<Packet> chunkPacket = stamp
                ? Create<Packet>(thisWireChunk - soHeaderSize)
                : Create<Packet>(thisWireChunk);
            if (stamp)
            {
                SoHeader soHeader;
                soHeader.SetProtocolId(static_cast<uint8_t>(RoceProtocol::SIMPLE));
                soHeader.SetEcn(false); // marked in-flight by the So switch on congestion
                soHeader.SetDataSize(fragmentDataSize);
                soHeader.SetChunkIndex(static_cast<uint32_t>(i));
                chunkPacket->AddHeader(soHeader);
            }

            FabricHeader header;
            header.SetPacketType(FabricPacketType::DATA);
            header.SetFabricType(GetFabricTypeForDest(destRank));
            header.SetFlowId(flowId);
            header.SetSequenceNumber(seqNum);
            header.SetVirtualChannel(vcId);
            header.SetSourceRank(m_rank);
            header.SetDestRank(destRank);
            header.SetPayloadSize(thisWireChunk);
            header.SetSourceMac(m_address);
            header.SetDestMac(ResolveDestMac(destRank));
            header.SetProtocol(stamp ? protocolId : 0);
            header.SetProtocolFramed(stamp);
            header.SetEffectiveDataSize(fragmentDataSize);
            header.SetTtl(64);

            // Credit-based flow control: check and consume credits before sending.
            // Scale-out (So) traffic bypasses credit: inter-node fabric uses
            // ECN/DCQCN (RoCE) or VOQ tail-drop + LLR retransmit (Ethernet) for
            // flow control, not per-packet credits. The Su/So split mirrors
            // GetProtocolModelForDest's IsLocalRank() predicate. So packets
            // never queue on credit (they'd deadlock since no credit is ever
            // returned for So -- see OnPacketDelivered). Su (local rank)
            // keeps full credit gating, unchanged.
            if (m_creditManager && IsLocalRank(destRank) && !m_creditManager->HasCredits(vcId))
            {
                NS_LOG_DEBUG("No credits for BULK chunk " << i << " on VC " << static_cast<int>(vcId)
                             << ", queuing remaining chunks");
                SendQueueEntry entry;
                entry.packet = chunkPacket;
                entry.header = header;
                entry.vcId = vcId;
                entry.seqNum = seqNum;
                entry.deviceIndex = sendDevices[(m_globalSprayOffset + i) % sendDevices.size()];
                m_sendQueue.push(entry);

                // Queue remaining chunks
                for (uint64_t j = i + 1; j < numChunks; j++)
                {
                    uint64_t remWireOffset = j * wireChunkSize;
                    if (remWireOffset >= wireSize)
                    {
                        continue; // trailing lane-pad fragment past the transfer end
                    }
                    uint32_t remWireChunk = static_cast<uint32_t>(
                        std::min(wireChunkSize, wireSize - remWireOffset));
                    uint64_t remDataOffset = j * dataChunkSize;
                    if (remDataOffset >= dataSize)
                    {
                        break; // wire-padding fragment: no data bytes left to send
                    }
                    uint32_t remFragmentData = static_cast<uint32_t>(
                        std::min(dataChunkSize, dataSize - remDataOffset));
                    uint64_t remSeqKey = MakeSeqKey(destRank, vcId, flowId);
                    uint32_t remSeqNum = m_nextSeqNum[remSeqKey]++;

                    const bool remStamp = stampSoHeader && (remWireChunk > soHeaderSize);
                    Ptr<Packet> remPacket = remStamp
                        ? Create<Packet>(remWireChunk - soHeaderSize)
                        : Create<Packet>(remWireChunk);
                    FabricHeader remHeader;
                    remHeader.SetPacketType(FabricPacketType::DATA);
                    remHeader.SetFabricType(GetFabricTypeForDest(destRank));
                    remHeader.SetFlowId(flowId);
                    remHeader.SetSequenceNumber(remSeqNum);
                    remHeader.SetVirtualChannel(vcId);
                    remHeader.SetSourceRank(m_rank);
                    remHeader.SetDestRank(destRank);
                    remHeader.SetPayloadSize(remWireChunk);
                    remHeader.SetSourceMac(m_address);
                    remHeader.SetDestMac(ResolveDestMac(destRank));
                    remHeader.SetProtocol(remStamp ? protocolId : 0);
                    remHeader.SetProtocolFramed(remStamp);
                    remHeader.SetEffectiveDataSize(remFragmentData);
                    remHeader.SetTtl(64);

                    if (remStamp)
                    {
                        SoHeader soHeader;
                        soHeader.SetProtocolId(static_cast<uint8_t>(RoceProtocol::SIMPLE));
                        soHeader.SetEcn(false);
                        soHeader.SetDataSize(remFragmentData);
                        soHeader.SetChunkIndex(static_cast<uint32_t>(j));
                        remPacket->AddHeader(soHeader);
                    }

                    SendQueueEntry remEntry;
                    remEntry.packet = remPacket;
                    remEntry.header = remHeader;
                    remEntry.vcId = vcId;
                    remEntry.seqNum = remSeqNum;
                    remEntry.deviceIndex = sendDevices[(m_globalSprayOffset + j) % sendDevices.size()];
                    m_sendQueue.push(remEntry);
                }
                TrySendQueuedPackets();
                break;
            }
            if (m_creditManager && IsLocalRank(destRank))
            {
                m_creditManager->ConsumeCredit(vcId, seqNum);
            }

            chunkPacket->AddHeader(header);

            uint32_t deviceIdx = sendDevices[(m_globalSprayOffset + i) % sendDevices.size()];
            Mac48Address destMac = ResolveDestMac(destRank);
            SendPacketOnDevice(chunkPacket, deviceIdx, destMac);

            m_txTrace(chunkPacket->GetSize(), seqNum);
            m_txPackets++;
            m_txBytes += chunkPacket->GetSize();
        }

        m_globalSprayOffset = static_cast<uint32_t>(
            (m_globalSprayOffset + numChunks) % sendDevices.size());
    }
    else
    {
        uint64_t seqKey = MakeSeqKey(destRank, vcId, flowId);
        uint32_t seqNum = m_nextSeqNum[seqKey]++;

        const uint32_t packetSize = static_cast<uint32_t>(wireSize);
        const bool stamp = stampSoHeader && (packetSize > soHeaderSize);
        Ptr<Packet> packet = stamp
            ? Create<Packet>(packetSize - soHeaderSize)
            : Create<Packet>(packetSize);
        if (stamp)
        {
            SoHeader soHeader;
            soHeader.SetProtocolId(static_cast<uint8_t>(RoceProtocol::SIMPLE));
            soHeader.SetEcn(false); // marked in-flight by the So switch on congestion
            soHeader.SetDataSize(static_cast<uint32_t>(dataSize));
            soHeader.SetChunkIndex(0);
            packet->AddHeader(soHeader);
        }

        FabricHeader header;
        header.SetPacketType(FabricPacketType::DATA);
        header.SetFabricType(GetFabricTypeForDest(destRank));
        header.SetFlowId(flowId);
        header.SetSequenceNumber(seqNum);
        header.SetVirtualChannel(vcId);
        header.SetSourceRank(m_rank);
        header.SetDestRank(destRank);
        header.SetPayloadSize(packetSize);
        header.SetSourceMac(m_address);
        header.SetDestMac(ResolveDestMac(destRank));
        header.SetProtocol(stamp ? protocolId : 0);
        header.SetProtocolFramed(stamp);
        header.SetEffectiveDataSize(static_cast<uint32_t>(dataSize));
        header.SetTtl(64);

        // Credit-based flow control: check and consume credits before sending.
        // So (inter-node) traffic bypasses credit (see multi-chunk branch
        // above for the rationale); Su (local rank) keeps full gating.
        if (m_creditManager && IsLocalRank(destRank) && !m_creditManager->HasCredits(vcId))
        {
            NS_LOG_DEBUG("No credits for BULK single packet on VC " << static_cast<int>(vcId));
            SendQueueEntry entry;
            entry.packet = packet;
            entry.header = header;
            entry.vcId = vcId;
            entry.seqNum = seqNum;
            m_sendQueue.push(entry);
            TrySendQueuedPackets();
            return;
        }
        if (m_creditManager && IsLocalRank(destRank))
        {
            m_creditManager->ConsumeCredit(vcId, seqNum);
        }

        packet->AddHeader(header);

        uint32_t deviceIdx;
        if (!devices.empty())
        {
            deviceIdx = devices[(m_globalSprayOffset) % devices.size()];
        }
        else
        {
            deviceIdx = ResolveDeviceIndex(destRank);
        }
        Mac48Address destMac = ResolveDestMac(destRank);
        SendPacketOnDevice(packet, deviceIdx, destMac);

        m_txTrace(packet->GetSize(), seqNum);
        m_txPackets++;
        m_txBytes += packet->GetSize();

        if (devices.size() > 1)
        {
            m_globalSprayOffset = (m_globalSprayOffset + 1) % devices.size();
        }
    }
}

void
FabricEndpoint::ProcessProtocolDataPacket(Ptr<Packet> packet, const FabricHeader& header)
{
    NS_LOG_FUNCTION(this << packet << header.GetSequenceNumber());

    uint8_t protocolId = header.GetProtocol();
    uint32_t effectiveSize = header.GetEffectiveDataSize();

    NS_LOG_DEBUG("ProcessProtocolDataPacket: protocol=" << static_cast<int>(protocolId)
                 << " wireSize=" << packet->GetSize()
                 << " effectiveSize=" << effectiveSize);

    // Check if this packet actually carries a framed payload. Deliver
    // directly through the receive pipeline (reorder buffer / bypass
    // delivery) when it does not — do NOT recurse through ProcessDataPacket:
    // it dispatches on the same wire flag and would route straight back
    // here, recursing until the stack overflows. The efficiency re-check the
    // flag replaced had exactly this shape: a framed-marked packet from an
    // efficiency-1.0 model (MCCL SIMPLE with WireEfficiency=1.0, raw NCCL
    // NONE) takes this direct-delivery arm; framed packets (efficiency<1.0
    // models, a stamped builder header) fall through to ExtractData.
    if (!header.IsProtocolFramed())
    {
        uint8_t vcId = header.GetVirtualChannel();
        uint16_t flowId = header.GetFlowId();
        if (m_bypassReorderBuffer)
        {
            DeliverBypassDataPacket(packet, header);
        }
        else
        {
            Ptr<ReorderBuffer> rb = GetOrCreateReorderBuffer(vcId, flowId,
                                                             header.GetSourceRank());
            bool inserted = rb->Insert(header.GetSequenceNumber(), packet, header);
            rb->DeliverReadyPackets();
            if (!inserted && rb->IsDuplicate(header.GetSequenceNumber()))
            {
                // A rejected DUPLICATE (a retransmission of an
                // already-delivered seq, or of one still parked in the
                // reorder buffer): the original delivery's ACK may have been
                // lost, and without this re-ACK the sender's LLR store entry
                // is never purged — the retry-timer backstop retransmits
                // forever and the entry degrades into a false permanent loss
                // for data the receiver already holds. Same gate as the raw
                // DATA reorder path: a capacity-rejected FUTURE seq is not a
                // duplicate and rides the sender's retry timer instead.
                SendLlrAckPacket(header, false);
            }
        }
        return;
    }

    // RoCEv2 ECN echo: if an So switch marked this segment's SoHeader ECN bit
    // in flight, emit a Congestion Notification Packet back to the source. The
    // FabricHeader was peeled on the receive path, so the SoHeader is now
    // outermost and PeekEcn reads it directly. Peek (non-mutating) so the
    // SoHeader is still present for ExtractData below. The fabric-type gate is
    // load-bearing: RoceProtocol::SIMPLE(1) collides with NcclProtocol::LL(1),
    // and Su LL/LL128 packets (framed, but not SoHeader-framed) would
    // otherwise reach here and PeekEcn would read 10 B of NCCL payload as a
    // SoHeader (spurious CNP). Only RoCE-fabric (So) packets carry a SoHeader.
    if (header.GetFabricType() == FabricType::ROCE &&
        protocolId == static_cast<uint8_t>(RoceProtocol::SIMPLE) &&
        SoHeader::PeekEcn(packet))
    {
        SendCnpPacket(header);
    }

    // Extract pure data from protocol-aware payload (So sources use the So
    // builder when one is mounted -- matches the send-side framing split)
    std::vector<uint8_t> extractedData(effectiveSize);
    uint64_t extracted = GetPayloadBuilderForRank(header.GetSourceRank())
                             ->ExtractData(packet, protocolId,
                                           extractedData.data(), effectiveSize);

    if (extracted > 0)
    {
        // Create a new packet with extracted data
        Ptr<Packet> dataPacket = Create<Packet>(extractedData.data(), extracted);

        // Deliver to application via callback
        uint8_t vcId = header.GetVirtualChannel();
        uint16_t flowId = header.GetFlowId();

        if (m_bypassReorderBuffer)
        {
            // Same bypass pipeline as raw DATA packets: strict-GBN acceptance
            // and duplicate suppression (a retransmitted So segment must not
            // deliver twice).
            DeliverBypassDataPacket(dataPacket, header);
        }
        else
        {
            // Use reorder buffer
            Ptr<ReorderBuffer> rb = GetOrCreateReorderBuffer(vcId, flowId,
                                                             header.GetSourceRank());
            bool inserted = rb->Insert(header.GetSequenceNumber(), dataPacket, header);
            rb->DeliverReadyPackets();
            if (!inserted && rb->IsDuplicate(header.GetSequenceNumber()))
            {
                // Re-ACK a rejected duplicate exactly like the raw DATA
                // reorder path (above) and ProcessDataPacket: a retransmitted
                // LL/LL128 segment whose original delivery's ACK was lost
                // must still purge the sender's LLR store entry, or the retry
                // timer runs out and records a false permanent loss for data
                // the receiver already delivered. A capacity-rejected future
                // seq is not a duplicate and is not ACKed.
                SendLlrAckPacket(header, false);
            }
        }

    }
}

} // namespace ns3
