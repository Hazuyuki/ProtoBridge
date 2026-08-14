/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Fabric Endpoint Model Implementation
 */

#include "fabric-endpoint.h"

#include "nccl-protocol-model.h"
#include "nccl-protocol-payload-builder.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/boolean.h"
#include "ns3/pointer.h"
#include "ns3/enum.h"

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
      m_rxPackets(0)
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
    m_devices.clear();
    m_creditManager = nullptr;
    m_payloadBuilder = nullptr;
    m_protocolModel = nullptr;
    m_llrManager = nullptr;
    m_gbnNextAckSeq.clear();
    m_gbnPendingAcks.clear();
    m_reorderBuffers.clear();
    m_linkDegradationModel = nullptr;
    m_fecModel = nullptr;
    m_latencyStatistics = nullptr;
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
        if (m_creditManager->GetAvailableCredits(vc) == 0)
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
    for (uint8_t vc = 0; vc < m_numVcs; ++vc)
    {
        if (m_creditManager->GetAvailableCredits(vc) == 0)
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

FabricType
FabricEndpoint::GetInterNodeFabricType() const
{
    return static_cast<FabricType>(m_interNodeFabricType);
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
    if (!IsLocalRank(destRank) && m_localRankCount > 1)
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
    // For spraying: use round-robin from routing devices table
    if (m_sprayingEnabled)
    {
        auto devicesIt = m_routingDevicesTable.find(destRank);
        if (devicesIt != m_routingDevicesTable.end() && devicesIt->second.size() > 0)
        {
            return GetNextDeviceIndex();
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
FabricEndpoint::GetOrCreateReorderBuffer(uint8_t vcId, uint16_t flowId)
{
    uint32_t key = (static_cast<uint32_t>(vcId) << 16) | (flowId & 0xFFFF);
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
                if (m_llrEnabled && m_llrManager)
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
                // No FEC: use link degradation's per-packet BER sampling
                if (!m_linkDegradationModel->ProcessPacket(pkt))
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
            // Schedule the rest of receive processing after decode latency
            Simulator::Schedule(decodeLatency, &FabricEndpoint::ContinueReceiveAfterFec,
                                this, pkt, header);
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

        // Re-add the header and forward
        pkt->AddHeader(header);

        uint32_t deviceIndex = ResolveDeviceIndex(destRank);
        Mac48Address destMac = ResolveDestMac(destRank);
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
        // Pass the PERMANENT_LOSS header (which carries original metadata:
        // effectiveDataSize, flowId, etc.) to the reorder buffer so the
        // permanent-gap callback can propagate terminal progress.
        uint8_t vcId = header.GetVirtualChannel();
        uint16_t flowId = header.GetFlowId();
        uint32_t lostSeqNum = header.GetSequenceNumber();
        Ptr<ReorderBuffer> rb = GetOrCreateReorderBuffer(vcId, flowId);
        rb->MarkPermanentGap(lostSeqNum, header);
        rb->DeliverReadyPackets();
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
FabricEndpoint::ContinueReceiveAfterFec(Ptr<Packet> pkt, FabricHeader header)
{
    // Continue receive processing after FEC decode latency
    m_rxTrace(pkt->GetSize(), header.GetSequenceNumber());
    m_rxPackets++;
    m_rxBytes += pkt->GetSize();

    uint16_t destRank = header.GetDestRank();

    if (destRank != m_rank && !m_routingTable.empty())
    {
        pkt->AddHeader(header);
        uint32_t deviceIndex = ResolveDeviceIndex(destRank);
        Mac48Address destMac = ResolveDestMac(destRank);
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
        // Sender already returned the credit in NotifyPermanentLoss.
        // Pass the PERMANENT_LOSS header with original metadata for
        // terminal progress propagation.
        uint8_t vcId = header.GetVirtualChannel();
        uint16_t flowId = header.GetFlowId();
        uint32_t lostSeqNum = header.GetSequenceNumber();
        Ptr<ReorderBuffer> rb = GetOrCreateReorderBuffer(vcId, flowId);
        rb->MarkPermanentGap(lostSeqNum, header);
        rb->DeliverReadyPackets();
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

    // Check if this is a protocol-aware packet
    NcclProtocol protocol = header.GetNcclProtocol();
    if (protocol != NcclProtocol::NONE && protocol != NcclProtocol::SIMPLE)
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
        // Track out-of-order arrivals even in bypass mode
        uint64_t flowKey = (static_cast<uint64_t>(vcId) << 48) |
                           (static_cast<uint64_t>(flowId) << 32) |
                           header.GetSourceRank();
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
            // Still return credit for the retransmitted packet
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
            return;
        }
        m_bypassDelivered.insert(deliveredKey);

        // Deliver packet directly to callback
        OnPacketDelivered(packet, header.GetSequenceNumber(), header);
    }
    else
    {
        Ptr<ReorderBuffer> rb = GetOrCreateReorderBuffer(vcId, flowId);
        rb->Insert(header.GetSequenceNumber(), packet, header);
        rb->DeliverReadyPackets();
    }

}

void
FabricEndpoint::NotifyPermanentLoss(FabricHeader originalHeader)
{
    uint32_t seqNum = originalHeader.GetSequenceNumber();
    uint16_t destRank = originalHeader.GetDestRank();
    uint8_t vcId = originalHeader.GetVirtualChannel();

    NS_LOG_FUNCTION(this << seqNum << destRank << static_cast<int>(vcId));

    // Return the consumed credit for the permanently lost packet (sender-side)
    if (m_creditManager)
    {
        m_creditManager->ReturnCredits(vcId, 1);
        NS_LOG_DEBUG("Returned credit for permanently lost seqNum " << seqNum
                     << " on VC " << static_cast<int>(vcId));
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
                m_llrManager->HandleRetryRequest(seqNum, sourceRank, flowId);
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
            m_llrManager->RemovePacketsUpTo(seqNum, sourceRank, flowId);
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

    NS_LOG_INFO("Received NACK for seqNum=" << seqNum << " from rank=" << sourceRank);

    if (m_llrEnabled && m_llrManager)
    {
        std::vector<LlrManager::Retransmission> retransmissions;

        if (m_llrManager->GetLlrMode() == LlrMode::SACK)
        {
            // SACK: retransmit only the NACKed packet
            std::unordered_set<uint32_t> nackSet{seqNum};
            retransmissions = m_llrManager->HandleSackRequest(nackSet, sourceRank, flowId);
        }
        else
        {
            // Go-Back-N: retransmit from NACK point onward
            retransmissions = m_llrManager->HandleRetryRequest(seqNum, sourceRank, flowId);
        }

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

    if (!m_llrManager->HasOutstandingPacket(seqNum, destRank, flowId))
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
        m_llrManager->RemovePacket(header.GetSequenceNumber(),
                                   header.GetSourceRank(),
                                   header.GetFlowId());
    }
}

void
FabricEndpoint::ProcessCreditPacket(Ptr<Packet> packet, const FabricHeader& header)
{
    NS_LOG_FUNCTION(this << packet << header.GetCreditCount());

    uint8_t vcId = header.GetVirtualChannel();
    uint16_t credits = header.GetCreditCount();

    m_creditManager->ReturnCredits(vcId, credits);
    if (m_llrEnabled && m_llrManager)
    {
        if (m_llrManager->GetLlrMode() == LlrMode::SACK)
        {
            m_llrManager->RemovePacket(header.GetSequenceNumber(),
                                       header.GetSourceRank(),
                                       header.GetFlowId());
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
                                                header.GetFlowId());
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

    if (type == FabricPacketType::MEMORY_READ)
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
    else if (type == FabricPacketType::MEMORY_WRITE)
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

            // Direct write to backing store
            if (!m_memoryData.empty() && address + writeSize <= m_memoryData.size())
            {
                std::vector<uint8_t> writeData(pktSize);
                packet->CopyData(writeData.data(), pktSize);
                std::memcpy(m_memoryData.data() + address,
                            writeData.data() + 8, writeSize);
            }
        }
    }
    else if (type == FabricPacketType::MEMORY_RESP)
    {
        if (!m_memoryResponseCallback.IsNull())
        {
            m_memoryResponseCallback(m_rank, packet, header);
        }
        else if (!m_receiveCallback.IsNull())
        {
            m_receiveCallback(m_rank, packet, header);
        }
    }

    // Return credits for MEMORY_READ, MEMORY_WRITE, and MEMORY_RESP.
    // All three consume fabric bandwidth and credits on the send side.
    if (type == FabricPacketType::MEMORY_READ ||
        type == FabricPacketType::MEMORY_WRITE ||
        type == FabricPacketType::MEMORY_RESP)
    {
        uint8_t vcId = header.GetVirtualChannel();
        SendCreditPacket(header.GetSourceRank(),
                         vcId,
                         1,
                         header.GetSequenceNumber(),
                         header.GetFlowId());
    }
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
FabricEndpoint::TrySendQueuedPackets()
{
    NS_LOG_FUNCTION(this);

    while (!m_sendQueue.empty())
    {
        SendQueueEntry& entry = m_sendQueue.front();

        if (!m_creditManager->HasCredits(entry.vcId))
        {
            NS_LOG_DEBUG("No credits for VC " << static_cast<int>(entry.vcId));
            break;
        }

        if (!m_creditManager->ConsumeCredit(entry.vcId, entry.seqNum))
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
        return;
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
    // NVLS/SHARP packets (ALLREDUCE/ALLGATHER) use credit bypass — the
    // receiver never sends RETRY_ACK, so storing them would leak memory
    // at 2+ GB/s (256MB × BER retransmissions, never freed). The switch's
    // SHARP engine handles its own reliability for these packet types.
    if (m_llrEnabled && m_llrManager && !m_isRetransmitting)
    {
        FabricHeader hdr;
        packet->PeekHeader(hdr);
        FabricPacketType ptype = hdr.GetPacketType();
        if (hdr.GetSourceRank() == m_rank &&
            !hdr.IsControlPacket() && !hdr.IsRetryPacket() &&
            ptype != FabricPacketType::ALLREDUCE &&
            ptype != FabricPacketType::ALLGATHER)
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
            Time totalDelay = serviceTime + encodeLatency;
            if (totalDelay > Time(0))
            {
                NS_LOG_DEBUG("WFQ contention: class=" << static_cast<int>(tc)
                             << " numActive=" << numActive
                             << " serviceTime=" << serviceTime.GetNanoSeconds() << "ns"
                             << " encodeLatency=" << encodeLatency.GetNanoSeconds() << "ns");
                Simulator::Schedule(totalDelay, [this, packet, deviceIndex, dest, tc]() {
                    Ptr<NetDevice> dev = m_devices[deviceIndex];
                    dev->Send(packet, dest, 0x0800);
                });
                // Decrement backlog after packet finishes transmission (serialization)
                Time txDone = totalDelay + serialization;
                Simulator::Schedule(txDone, [this, tc]() {
                    m_contentionModel->DecrementBacklog(tc);
                });
                return;
            }
        }
    }

    // No WFQ contention or single class: apply FEC encode latency only
    if (LinkUsesFec())
    {
        Time encodeLatency = m_fecModel->GetEncodeLatency();
        if (encodeLatency > Seconds(0))
        {
            Simulator::Schedule(encodeLatency, [this, packet, deviceIndex, dest]() {
                Ptr<NetDevice> device = m_devices[deviceIndex];
                device->Send(packet, dest, 0x0800);
            });
            // Decrement backlog after serialization
            if (m_contentionModel && tc != TrafficClass::NUM_CLASSES)
            {
                Time serialization = m_contentionModel->GetSerializationTime(packet->GetSize());
                Simulator::Schedule(encodeLatency + serialization, [this, tc]() {
                    m_contentionModel->DecrementBacklog(tc);
                });
            }
            return;
        }
    }

    // Immediate send (no FEC latency, no WFQ contention)
    Ptr<NetDevice> dev = m_devices[deviceIndex];
    dev->Send(packet, dest, 0x0800);
    if (m_contentionModel && tc != TrafficClass::NUM_CLASSES)
    {
        Time serialization = m_contentionModel->GetSerializationTime(packet->GetSize());
        Simulator::Schedule(serialization, [this, tc]() {
            m_contentionModel->DecrementBacklog(tc);
        });
    }
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
    if ((packetType == FabricPacketType::DATA
         || packetType == FabricPacketType::P2P
         || header.IsCollectivePacket())
        && header.GetSourceRank() != 0xFFFF)
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

    uint8_t protocolId = m_protocolModel->GetProtocolId(size);
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

    const double efficiency = m_protocolModel->GetEfficiency(protocolId);
    const uint64_t wireSize = m_protocolModel->GetWireSize(dataSize, protocolId);

    NS_LOG_INFO("Bulk wire transfer: dataSize=" << dataSize << " wireSize=" << wireSize
               << " efficiency=" << efficiency << " protocol=" << static_cast<int>(protocolId));

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
        uint64_t numChunks = std::max(minChunks, sizeBasedChunks);
        // Coalesce bulk transfers while retaining at least one chunk per path.
        // Total wire bytes and path striping are unchanged. Queue and credit
        // events occur at this coarser granularity, so this API is reserved for
        // bulk-transfer timing rather than fine-grained contention tests.
        uint64_t memCapChunks = (wireSize + MAX_CHUNK_BYTES - 1) / MAX_CHUNK_BYTES;
        numChunks = std::max(minChunks, std::min(sizeBasedChunks, memCapChunks));
        uint64_t wireChunkSize = (wireSize + numChunks - 1) / numChunks;
        uint64_t dataChunkSize = (dataSize + numChunks - 1) / numChunks;

        for (uint64_t i = 0; i < numChunks; i++)
        {
            uint64_t wireOffset = i * wireChunkSize;
            uint32_t thisWireChunk = static_cast<uint32_t>(
                std::min(wireChunkSize, wireSize - wireOffset));

            // Ensure fragment effectiveDataSize sums to original dataSize
            uint64_t dataOffset = i * dataChunkSize;
            uint32_t fragmentDataSize = static_cast<uint32_t>(
                std::min(dataChunkSize, dataSize - dataOffset));

            uint64_t seqKey = MakeSeqKey(destRank, vcId, flowId);
            uint32_t seqNum = m_nextSeqNum[seqKey]++;

            Ptr<Packet> chunkPacket = Create<Packet>(thisWireChunk);

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
            header.SetProtocol(0);
            header.SetEffectiveDataSize(fragmentDataSize);
            header.SetTtl(64);

            // Credit-based flow control: check and consume credits before sending
            if (m_creditManager && !m_creditManager->HasCredits(vcId))
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
                    uint32_t remWireChunk = static_cast<uint32_t>(
                        std::min(wireChunkSize, wireSize - remWireOffset));
                    uint64_t remDataOffset = j * dataChunkSize;
                    uint32_t remFragmentData = static_cast<uint32_t>(
                        std::min(dataChunkSize, dataSize - remDataOffset));
                    uint64_t remSeqKey = MakeSeqKey(destRank, vcId, flowId);
                    uint32_t remSeqNum = m_nextSeqNum[remSeqKey]++;

                    Ptr<Packet> remPacket = Create<Packet>(remWireChunk);
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
                    remHeader.SetProtocol(0);
                    remHeader.SetEffectiveDataSize(remFragmentData);
                    remHeader.SetTtl(64);

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
            if (m_creditManager)
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
        Ptr<Packet> packet = Create<Packet>(packetSize);

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
        header.SetProtocol(0);
        header.SetEffectiveDataSize(static_cast<uint32_t>(dataSize));
        header.SetTtl(64);

        // Credit-based flow control: check and consume credits before sending
        if (m_creditManager && !m_creditManager->HasCredits(vcId))
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
        if (m_creditManager)
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

    // Check if this protocol has no overhead
    double efficiency = m_protocolModel->GetEfficiency(protocolId);
    if (efficiency >= 1.0)
    {
        ProcessDataPacket(packet, header);
        return;
    }

    // Extract pure data from protocol-aware payload
    std::vector<uint8_t> extractedData(effectiveSize);
    uint64_t extracted = m_payloadBuilder->ExtractData(packet, protocolId,
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
            // Deliver directly
            OnPacketDelivered(dataPacket, header.GetSequenceNumber(), header);
        }
        else
        {
            // Use reorder buffer
            Ptr<ReorderBuffer> rb = GetOrCreateReorderBuffer(vcId, flowId);
            rb->Insert(header.GetSequenceNumber(), dataPacket, header);
            rb->DeliverReadyPackets();
        }

    }
}

} // namespace ns3
