/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Fabric Endpoint Model for heterogeneous device-to-device communication
 */

#ifndef FABRIC_ENDPOINT_H
#define FABRIC_ENDPOINT_H

#include "fabric-header.h"
#include "device-type.h"
#include "nccl-protocol.h"
#include "credit-manager.h"
#include "reorder-buffer.h"
#include "link-degradation.h"
#include "llr-manager.h"
#include "fec-model.h"
#include "latency-statistics.h"
#include "contention-model.h"
#include "protocol-model.h"
#include "protocol-payload-builder.h"
#include "flow-control-policy.h"

#include "ns3/application.h"
#include "ns3/net-device.h"
#include "ns3/mac48-address.h"
#include "ns3/traced-callback.h"
#include "ns3/timer.h"
#include <set>

#include <vector>
#include <queue>
#include <unordered_map>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Queue entry for outgoing packets
 */
struct SendQueueEntry
{
    Ptr<Packet> packet;                                       ///< The packet to send
    FabricHeader header;                                      ///< The fabric header
    uint8_t vcId = 0;                                        ///< Virtual channel ID
    uint32_t seqNum = 0;                                     ///< Sequence number
    uint32_t deviceIndex = UINT32_MAX;                        ///< Pre-selected spray device (UINT32_MAX = re-resolve)
};

/**
 * @ingroup gpu-cluster
 * @brief Fabric Endpoint Application
 *
 * This class models a fabric endpoint that:
 * - Supports heterogeneous device types (GPU, CPU, TPU, Memory)
 * - Sends/receives data directly via NetDevice (no TCP/IP)
 * - Implements credit-based flow control
 * - Supports multi-path packet spraying with routing table
 * - Handles out-of-order packet reassembly
 * - Integrates link degradation model
 */
class FabricEndpoint : public Application
{
  public:
    static TypeId GetTypeId();

    FabricEndpoint();
    ~FabricEndpoint() override;

    FabricEndpoint(const FabricEndpoint&) = delete;
    FabricEndpoint& operator=(const FabricEndpoint&) = delete;

    // Device type
    void SetDeviceType(DeviceType type);
    DeviceType GetDeviceType() const;

    // Fabric type
    void SetFabricType(FabricType type);
    FabricType GetFabricType() const;

    // Inter-node fabric type (for RDMA NICs)
    void SetInterNodeFabricType(FabricType type);
    FabricType GetInterNodeFabricType() const;

    // Node ID (which node this GPU belongs to)
    void SetNodeId(uint32_t nodeId);
    uint32_t GetNodeId() const;

    // Local rank range (ranks belonging to same node)
    void SetLocalRankRange(uint16_t base, uint16_t count);
    bool IsLocalRank(uint16_t rank) const;

    // Topology-aware collective embedding (set by GpuClusterTopologyHelper).
    // 0xFFFF = unset -> injectors fall back to rank-arithmetic. When set,
    // ring/tree send targets follow the physical locality of the fabric.
    void SetRingNeighbors(uint16_t next, uint16_t prev);
    uint16_t GetRingNext() const;
    uint16_t GetRingPrev() const;
    void SetTreeNeighbors(uint16_t parent, uint16_t leftChild, uint16_t rightChild);
    uint16_t GetTreeParent() const;
    uint16_t GetTreeLeftChild() const;
    uint16_t GetTreeRightChild() const;

    // Get fabric type for a destination (returns interNodeFabricType for remote ranks)
    FabricType GetFabricTypeForDest(uint16_t destRank) const;

    // Rank
    void SetRank(uint16_t rank);
    uint16_t GetRank() const;

    // NetDevice management
    uint32_t AddNetDevice(Ptr<NetDevice> device);  // Returns device index
    uint32_t GetNNetDevices() const;
    Ptr<NetDevice> GetNetDevice(uint32_t index) const;

    // Routing table (for direct-connect topologies: Ring, Full-Mesh)
    void SetRoutingEntry(uint16_t destRank, uint32_t deviceIndex);
    void ClearRoutingTable();
    uint32_t GetRoutingDeviceIndex(uint16_t destRank) const;

    // Multi-path routing (for spraying)
    void SetRoutingDevices(uint16_t destRank, const std::vector<uint32_t>& deviceIndices);
    std::vector<uint32_t> GetRoutingDevices(uint16_t destRank) const;

    // Lane groups (sub-device spraying)
    void SetNumLanes(uint32_t numLanes);
    uint32_t GetNumLanes() const;
    void SetLaneGroup(uint32_t logicalDevIdx, const std::vector<uint32_t>& physicalDevIndices);
    std::vector<uint32_t> GetPhysicalDevicesForDest(uint16_t destRank) const;

    // Packet spraying control
    void SetSprayingEnabled(bool enable);
    bool GetSprayingEnabled() const;
    void SetSprayChunkSize(uint32_t chunkSize);  // Chunk size for spraying (bytes)

    // Neighbor MAC table
    void SetNeighborMac(uint16_t rank, Mac48Address addr);
    Mac48Address GetNeighborMac(uint16_t rank) const;

    /**
     * @brief Initialize the endpoint for packet transmission/reception
     *
     * Must be called after adding NetDevices and setting up routing.
     * This sets up device callbacks and initializes internal state.
     */
    void Initialize();

    // Send methods
    void SendData(uint16_t destRank, const uint8_t* data, uint32_t size,
                  uint16_t flowId, uint8_t vcId = 0);

    /**
     * @brief Send data with NCCL protocol-aware payload
     *
     * This method builds protocol-aware payloads according to NCCL specifications:
     * - LL: 64 bytes data + 64 bytes flags per 128-byte line (50% efficiency)
     * - LL128: 8 bytes header + 120 bytes data per 128-byte chunk (93.75% efficiency)
     * - SIMPLE: Pure data with no per-chunk overhead (100% efficiency)
     *
     * @param destRank Destination rank
     * @param data Raw data buffer
     * @param size Data size in bytes
     * @param protocol NCCL protocol to use
     * @param flowId Flow identifier
     * @param vcId Virtual channel ID
     */
    void SendDataWithProtocol(uint16_t destRank, const uint8_t* data, uint32_t size,
                              NcclProtocol protocol, uint16_t flowId, uint8_t vcId = 0);

    /**
     * @brief Send data with auto-selected NCCL protocol
     *
     * Automatically selects protocol based on message size:
     * - <8KB: LL protocol
     * - 8KB-2MB: LL128 protocol
     * - >2MB: SIMPLE protocol
     *
     * @param destRank Destination rank
     * @param data Raw data buffer
     * @param size Data size in bytes
     * @param flowId Flow identifier
     * @param vcId Virtual channel ID
     */
    void SendDataAutoProtocol(uint16_t destRank, const uint8_t* data, uint32_t size,
                              uint16_t flowId, uint8_t vcId = 0);
    void SendDataWithProtocol(uint16_t destRank, const uint8_t* data, uint32_t size,
                              uint8_t protocolId, uint16_t flowId, uint8_t vcId = 0);

    /**
     * @brief Send data with protocol wire overhead as bulk transfer
     *
     * Instead of creating per-protocol-line sub-packets (which causes massive
     * FabricHeader overhead for LL/LL128 protocols), this method computes the
     * wire size = GetWireSize(dataSize, protocolId) and sends the entire
     * adjusted-size data as one (or a few spray-fragmented) packets.
     *
     * FabricHeader.effectiveDataSize carries the original data size, so
     * receivers can correctly track progress against the data size.
     * FabricHeader.protocol is set to 0 (NONE) to bypass per-line processing.
     */
    void SendBulkWireTransfer(uint16_t destRank, const uint8_t* data, uint32_t dataSize,
                              uint8_t protocolId, uint16_t flowId, uint8_t vcId = 0);
    void SendBulkWireTransferSize(uint16_t destRank, uint64_t dataSize,
                                  uint8_t protocolId, uint16_t flowId, uint8_t vcId = 0);

    void SendCollective(FabricPacketType type, uint16_t destRank,
                        const uint8_t* data, uint32_t size);
    void SendCollectiveBulk(FabricPacketType type, uint16_t destRank, uint64_t size);
    void SendCollectiveBulk(FabricPacketType type,
                            uint16_t destRank,
                            uint64_t effectiveSize,
                            uint64_t wireSize);
    void SendP2p(uint16_t destRank, const uint8_t* data, uint32_t size,
                 uint16_t flowId, uint8_t vcId = 0);
    void SendMemoryRead(uint16_t destRank, uint64_t address, uint32_t size);
    void SendMemoryWrite(uint16_t destRank, uint64_t address,
                         const uint8_t* data, uint32_t size);
    void SendMemoryResponse(uint16_t destRank, const uint8_t* data, uint32_t size);

    /**
     * @brief Send data with memory-semantic access
     *
     * Supports three access types:
     * - DMA_BULK: Uses existing SendMemoryRead/Write (backward compat)
     * - SYNC_LOAD_STORE: Direct load/store, scheduled at syncMemLatencyNs
     * - ASYNC_URMA: Asynchronous remote memory access, scheduled at asyncMemLatencyNs
     *
     * @param destRank Destination rank
     * @param address Remote memory address
     * @param data Data buffer (nullptr for read operations)
     * @param size Data size in bytes
     * @param accessType Memory access type
     */
    void SendMemorySemantic(uint16_t destRank, uint64_t address,
                            const uint8_t* data, uint32_t size,
                            MemoryAccessType accessType);

    // LLR (Link Layer Retry) attributes
    void SetLlrEnabled(bool enable);
    bool GetLlrEnabled() const;
    void SetLlrRetryLimit(uint32_t limit);
    uint32_t GetLlrRetryLimit() const;
    Ptr<LlrManager> GetLlrManager() const;

    // FEC model
    void SetFecModel(Ptr<FecModel> model);
    Ptr<FecModel> GetFecModel() const;
    void SetFecOpticalOnly(bool opticalOnly);

    // Latency statistics
    void SetLatencyStatistics(Ptr<LatencyStatistics> stats);
    Ptr<LatencyStatistics> GetLatencyStatistics() const;

    // Contention model
    void SetContentionModel(Ptr<ContentionModel> model);
    Ptr<ContentionModel> GetContentionModel() const;

    // Memory semantic latency attributes
    void SetSyncMemLatencyNs(uint64_t ns);
    uint64_t GetSyncMemLatencyNs() const;
    void SetAsyncMemLatencyNs(uint64_t ns);
    uint64_t GetAsyncMemLatencyNs() const;

    // Virtual channels and credits
    void SetNumVirtualChannels(uint8_t numVcs);
    void SetVcCredits(uint8_t vcId, uint32_t credits);

    // Flow-control policy seam (send gate dispatches on this)
    void SetFlowControlPolicy(FlowControlPolicy policy);
    FlowControlPolicy GetFlowControlPolicy() const;
    /**
     * @brief Send-gate dispatch. Returns true if the packet is admitted
     *        (credit consumed under CREDIT); false => caller backpressures.
     *        WINDOW/RATE are admitted (stub) and log a warning.
     *        creditBypass=true skips credit accounting (NVLS/SHARP packets).
     */
    bool FlowControlGate(uint8_t vcId, uint32_t seqNum, bool creditBypass);
    void SetLaunchDelay(uint64_t delayNs);
    void SetBulkChunkSize(uint32_t bytes);

    // Callbacks
    typedef Callback<void, uint16_t, Ptr<Packet>, FabricHeader> ReceiveCallback;
    typedef Callback<void, uint16_t, uint64_t, uint32_t> MemoryRequestCallback;
    typedef Callback<void, uint16_t, uint64_t, MemoryAccessType> MemorySemanticCallback;

    void SetReceiveCallback(ReceiveCallback cb);
    void SetMemoryResponseCallback(ReceiveCallback cb);
    void SetMemoryRequestCallback(MemoryRequestCallback cb);
    void SetMemorySemanticCallback(MemorySemanticCallback cb);

    // Address
    Mac48Address GetAddress() const;
    void SetAddress(Mac48Address addr);

    // Protocol model
    void SetProtocolModel(Ptr<ProtocolModel> model);
    Ptr<ProtocolModel> GetProtocolModel() const;

    // Protocol payload builder
    void SetProtocolPayloadBuilder(Ptr<ProtocolPayloadBuilder> builder);
    Ptr<ProtocolPayloadBuilder> GetProtocolPayloadBuilder() const;

    // Link degradation
    void SetLinkDegradationModel(Ptr<LinkDegradationModel> model);
    Ptr<LinkDegradationModel> GetLinkDegradationModel() const;

    // Reorder buffer control
    void SetBypassReorderBuffer(bool bypass);
    bool GetBypassReorderBuffer() const;

    void GetReorderBufferStats(uint32_t& totalReorderEvents, uint32_t& maxOccupancy) const;
    uint32_t GetTxPackets() const { return m_txPackets; }
    uint32_t GetRxPackets() const { return m_rxPackets; }

    // Memory attributes (for MEMORY device type)
    void SetMemorySize(uint64_t size);
    uint64_t GetMemorySize() const;
    void SetMemoryLatency(Time latency);
    Time GetMemoryLatency() const;

    // Traced callbacks
    typedef TracedCallback<uint32_t, uint32_t> TxRxTracedCallback;

    /**
     * @brief Make a sequence number counter key from destRank, vcId, and flowId
     * @return Composite key: (destRank << 24) | (vcId << 16) | flowId
     */
    static uint64_t MakeSeqKey(uint16_t destRank, uint8_t vcId, uint16_t flowId)
    {
        return (static_cast<uint64_t>(destRank) << 24) | (static_cast<uint64_t>(vcId) << 16) | (static_cast<uint64_t>(flowId) & 0xFFFF);
    }

  protected:
    void DoDispose() override;
    void StartApplication() override;
    void StopApplication() override;

    /**
     * @brief Send a packet on a specific device
     * @param packet The packet to send
     * @param deviceIndex Index of the device to use
     * @param dest Destination MAC address
     */
    void SendPacketOnDevice(Ptr<Packet> packet, uint32_t deviceIndex,
                            const Address& dest);
    bool LinkUsesFec() const;

    /**
     * @brief Get the next device index for spraying
     * @return Device index
     */
    uint32_t GetNextDeviceIndex();

    /**
     * @brief Resolve device index for a destination
     * @param destRank Destination rank
     * @return Device index
     */
    uint32_t ResolveDeviceIndex(uint16_t destRank);

    /**
     * @brief Resolve destination MAC address
     * @param destRank Destination rank
     * @return MAC address
     */
    Mac48Address ResolveDestMac(uint16_t destRank);

    /**
     * @brief Callback when credits become available
     * @param vcId Virtual channel ID
     */
    void OnCreditAvailable(uint8_t vcId);

    /**
     * @brief Callback when packet is delivered
     */
    void OnPacketDelivered(Ptr<Packet> packet, uint32_t seqNum, FabricHeader header);

    /**
     * @brief Callback when a permanent gap is crossed in sequence order
     *
     * Creates a synthetic empty packet and invokes the collective receive
     * callback so the collective layer can advance its progress using
     * the original packet's effectiveDataSize.
     */
    void OnPermanentGapDelivered(uint32_t seqNum, FabricHeader header);

    /**
     * @brief Process a data packet
     * @param packet The packet
     * @param header The fabric header
     */
    void ProcessDataPacket(Ptr<Packet> packet, const FabricHeader& header);
    void ContinueReceiveAfterFec(Ptr<Packet> pkt, FabricHeader header);

    /**
     * @brief Process a credit packet
     * @param packet The packet
     * @param header The fabric header
     */
    void ProcessCreditPacket(Ptr<Packet> packet, const FabricHeader& header);

    /**
     * @brief Process a memory packet
     * @param packet The packet
     * @param header The fabric header
     */
    void ProcessMemoryPacket(Ptr<Packet> packet, const FabricHeader& header);

    /**
     * @brief Process a retry packet (RETRY_REQUEST or RETRY_ACK)
     * @param packet The packet
     * @param header The fabric header
     */
    void ProcessRetryPacket(Ptr<Packet> packet, const FabricHeader& header);

    /**
     * @brief Handle permanent delivery failure (retry limit exceeded)
     * @param originalHeader The original packet's FabricHeader with full metadata
     *
     * The header carries seqNum, destRank, vcId, effectiveDataSize, flowId, etc.
     * from the stored original packet. These are used to:
     * 1. Return credit locally (sender-side)
     * 2. Populate the PERMANENT_LOSS notification packet sent to the receiver
     */
    void NotifyPermanentLoss(FabricHeader originalHeader);

    /**
     * @brief Process a NACK packet (FEC uncorrectable, trigger LLR retry)
     * @param packet The packet
     * @param header The fabric header
     */
    void ProcessNackPacket(Ptr<Packet> packet, const FabricHeader& header);
    void ProcessAckPacket(Ptr<Packet> packet, const FabricHeader& header);
    void SendRetransmission(LlrManager::Retransmission retransmission);

    /**
     * @brief Process a protocol-aware data packet
     *
     * Extracts pure data from protocol-aware payload and delivers to application.
     *
     * @param packet The packet with protocol overhead
     * @param header The fabric header
     */
    void ProcessProtocolDataPacket(Ptr<Packet> packet, const FabricHeader& header);

    /**
     * @brief Notify completion of a memory-semantic operation
     * @param destRank Destination rank
     * @param address Remote memory address
     * @param accessType Memory access type
     */
    void NotifyMemorySemanticComplete(uint16_t destRank, uint64_t address,
                                      MemoryAccessType accessType);

  /**
     * @brief Get or create reorder buffer for a (VC, flowId) pair
     * @param vcId Virtual channel ID
     * @param flowId Flow ID
     * @return Pointer to the reorder buffer
     */
    Ptr<ReorderBuffer> GetOrCreateReorderBuffer(uint8_t vcId, uint16_t flowId);

    private:
    bool ReceiveFromDevice(Ptr<NetDevice> device, Ptr<const Packet> packet,
                           uint16_t protocol, const Address& source,
                           const Address& destination, NetDevice::PacketType packetType);

    void SendCreditPacket(uint16_t destRank, uint8_t vcId, uint16_t count,
                          uint32_t seqNum, uint16_t flowId);
    void SendLlrAckPacket(const FabricHeader& receivedHeader, bool cumulative);
    void TrySendQueuedPackets();

    /**
     * @brief Internal method to send data with NCCL protocol
     *
     * @param destRank Destination rank
     * @param data Raw data buffer
     * @param size Data size in bytes
     * @param protocol NCCL protocol to use
     * @param flowId Flow identifier
     * @param vcId Virtual channel ID
     */
    void SendDataInternal(uint16_t destRank, const uint8_t* data, uint32_t size,
                          uint8_t protocolId, uint16_t flowId, uint8_t vcId);

    // Device identity
    uint8_t m_deviceType;                           ///< Type of this endpoint (DeviceType value)
    uint8_t m_fabricType;                           ///< Fabric type (FabricType value)
    uint8_t m_interNodeFabricType;                 ///< Inter-node fabric type (for RDMA NICs)
    uint16_t m_rank;                                ///< Endpoint rank
    uint32_t m_nodeId;                              ///< Node ID (which node this GPU belongs to)
    uint16_t m_localRankBase;                       ///< Base rank of this node (first rank on same node)
    uint16_t m_localRankCount;                      ///< Number of GPUs on same node
    // Topology-aware collective embedding (0xFFFF = unset, use rank arithmetic)
    uint16_t m_ringNext{0xFFFF};                    ///< Ring reduce-scatter neighbor (clockwise)
    uint16_t m_ringPrev{0xFFFF};                    ///< Ring all-gather neighbor (counterclockwise)
    uint16_t m_treeParent{0xFFFF};                  ///< Tree reduce parent
    uint16_t m_treeLeftChild{0xFFFF};               ///< Tree broadcast left child
    uint16_t m_treeRightChild{0xFFFF};              ///< Tree broadcast right child
    Mac48Address m_address;                         ///< MAC address

    // Network interfaces
    std::vector<Ptr<NetDevice>> m_devices;          ///< NetDevices for multi-path
    uint32_t m_currentDeviceIndex;                  ///< Current device index for RR spraying

    // Routing
    std::unordered_map<uint16_t, uint32_t> m_routingTable;       ///< destRank -> deviceIndex
    std::unordered_map<uint16_t, std::vector<uint32_t>> m_routingDevicesTable; ///< destRank -> vector of deviceIndices (for spraying)
    std::unordered_map<uint16_t, Mac48Address> m_neighborMacTable; ///< destRank -> MAC address

    // Packet spraying
    bool m_sprayingEnabled;                         ///< Enable packet spraying across multiple links
    uint32_t m_sprayChunkSize;                      ///< Chunk size for spraying (default: 4KB)
    uint32_t m_globalSprayOffset;                   ///< Global spray offset (incremented per SendData call)

    // Lane groups (sub-device spraying)
    uint32_t m_numLanes;                            ///< Number of physical lanes per logical link (default 1)
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_laneGroups; ///< logical device idx -> physical device indices

    // Flow control
    Ptr<CreditManager> m_creditManager;             ///< Credit manager
    FlowControlPolicy m_flowControlPolicy{FlowControlPolicy::CREDIT}; ///< Policy seam (default: credit)
    std::unordered_map<uint32_t, Ptr<ReorderBuffer>> m_reorderBuffers; ///< Per-(VC,flowId) reorder buffers, key=(vcId<<16)|flowId

    // Send queue
    std::queue<SendQueueEntry> m_sendQueue;         ///< Pending send queue
    std::unordered_map<uint64_t, uint32_t> m_nextSeqNum; ///< Next sequence number per (destRank, vcId) key

    // Configuration
    uint8_t m_numVcs;                               ///< Number of virtual channels
    uint64_t m_launchDelayNs;                       ///< NCCL launch delay
    uint32_t m_bulkChunkSize;                       ///< Maximum packet size for bulk transfers

    // Link degradation
    Ptr<LinkDegradationModel> m_linkDegradationModel; ///< Link degradation model

    // Protocol model (defaults to NcclProtocolModel)
    Ptr<ProtocolModel> m_protocolModel;               ///< Vendor-specific protocol model

    // Protocol payload builder (defaults to NcclProtocolPayloadBuilder)
    Ptr<ProtocolPayloadBuilder> m_payloadBuilder;     ///< Vendor-specific payload builder

    // Reorder buffer control
    bool m_bypassReorderBuffer;                     ///< Bypass reorder buffer for direct links
    uint32_t m_bypassReorderEvents{0};              ///< Out-of-order arrivals counted in bypass mode
    std::map<uint64_t, uint32_t> m_bypassNextExpectedSeq; ///< Next expected seq per flow (bypass mode)
    std::set<std::pair<uint64_t, uint32_t>> m_bypassDelivered; ///< (flowKey, seqNum) already delivered

    // Memory attributes
    uint64_t m_memorySize;                          ///< Memory size in bytes (for MEMORY type)
    Time m_memoryLatency;                           ///< Memory access latency (for MEMORY type)
    std::vector<uint8_t> m_memoryData;              ///< Backing store for MEMORY type
    uint64_t m_syncMemLatencyNs;                    ///< Sync load/store latency (default 500ns for NVLink, 200ns for UB)
    uint64_t m_asyncMemLatencyNs;                   ///< Async URMA latency (default 1000ns for NVLink, 2000ns for UB)

    // LLR attributes
    bool m_llrEnabled;                              ///< Enable link-level retry (default false for NVLink, true for UB)
    uint32_t m_llrRetryLimit;                       ///< Maximum retry count for LLR (default 3)
    Ptr<LlrManager> m_llrManager;                   ///< LLR manager for retry buffer
    bool m_isRetransmitting;                        ///< True during retransmission, prevents StorePacket reset
    std::unordered_map<uint64_t, uint32_t> m_gbnNextAckSeq;
    std::unordered_map<uint64_t, std::set<uint32_t>> m_gbnPendingAcks;

    // FEC model
    Ptr<FecModel> m_fecModel;                       ///< Forward error correction model
    bool m_fecOpticalOnly;                          ///< Restrict FEC to optical links

    // Latency statistics
    Ptr<LatencyStatistics> m_latencyStatistics;     ///< Per-flow latency statistics

    // Contention model (WFQ bandwidth arbitration)
    Ptr<ContentionModel> m_contentionModel;          ///< Contention model for WFQ bandwidth sharing

    // Callbacks
    ReceiveCallback m_receiveCallback;              ///< Receive callback
    ReceiveCallback m_memoryResponseCallback;       ///< Memory-response callback
    MemoryRequestCallback m_memoryRequestCallback;  ///< Memory request callback
    MemorySemanticCallback m_memorySemanticCallback; ///< Memory-semantic completion callback

    // Traced sources
    TracedCallback<uint32_t, uint32_t> m_txTrace;   ///< TX: packet size, seqNum
    TracedCallback<uint32_t, uint32_t> m_rxTrace;   ///< RX: packet size, seqNum

    // Statistics
    uint64_t m_txBytes;
    uint64_t m_rxBytes;
    uint32_t m_txPackets;
    uint32_t m_rxPackets;
};

// Backward compatibility typedef
using GpuEndpoint = FabricEndpoint;

} // namespace ns3

#endif /* FABRIC_ENDPOINT_H */
