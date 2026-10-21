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
#include "dcqcn-rate-machine.h"
#include "rttcc-rate-machine.h"

#include "ns3/application.h"
#include "ns3/net-device.h"
#include "ns3/mac48-address.h"
#include "ns3/traced-callback.h"
#include "ns3/timer.h"
#include <set>

#include <vector>
#include <queue>
#include <deque>
#include <tuple>
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
    // Opt-in sender-side retry-timeout backstop: registers the LlrManager
    // timeout callback so an un-ACKed entry is RETRANSMITTED when its retry
    // timer fires (SACK: that seq; GBN: from that seq onward). Without this
    // the timeout is a no-op, and under random loss a lost NACK deadlocks
    // SACK mode (the NACK is the only recovery trigger). Default-off for
    // parity: under heavy congestion RETRY_ACKs arrive later than the retry
    // timeout and the backstop would inject spurious duplicates.
    void EnableLlrTimeoutRetransmit();

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

    // DCQCN rate machine (scale-out RoCEv2). Mounted only on So endpoints
    // (via --dcqcn); null => the send path is byte-identical to the unmounted
    // baseline (parity-safe).
    void SetDcqcnRateMachine(Ptr<DcqcnRateMachine> machine);
    Ptr<DcqcnRateMachine> GetDcqcnRateMachine() const;

    // RTTCC rate machine (scale-out; RTT-probe-based, fabric-agnostic — works
    // on lossy plain Ethernet where no ECN/CNP loop exists). Mount ONE
    // machine per endpoint (the flow key carries no source rank). Null =>
    // byte-identical to the unmounted baseline (parity-safe, --rttcc off).
    void SetRttccRateMachine(Ptr<RttccRateMachine> machine);
    Ptr<RttccRateMachine> GetRttccRateMachine() const;
    /**
     * @brief Send-gate dispatch. Returns true if the packet is admitted
     *        (credit consumed under CREDIT); false => caller backpressures.
     *        WINDOW/RATE are admitted (stub) and log a warning.
     *        creditBypass=true skips credit accounting (NVLS/SHARP packets).
     *
     *        Virtual so a FabricEndpoint subclass can plug an arbitrary flow
     *        control method (beyond the CREDIT/WINDOW/RATE enum) without
     *        editing this base or the send-gate call sites.
     */
    virtual bool FlowControlGate(uint8_t vcId, uint32_t seqNum, bool creditBypass);
    void SetLaunchDelay(uint64_t delayNs);
    void SetBulkChunkSize(uint32_t bytes);
    /**
     * @brief Bound the number of in-flight bulk chunks to a remote (So) rank,
     *        modeling a NIC send-queue depth (perftest tx_depth). 0 (default)
     *        keeps the legacy unbounded synchronous dispatch. The window is
     *        ACK-clocked: a slot is released when the per-packet LLR ACK
     *        purges the retry store (or on permanent loss). So-only; requires
     *        LLR (without LLR ACKs no slot is ever released).
     */
    void SetSoTxWindow(uint32_t chunks);

    /**
     * @brief Strict go-back-N receive semantics for So (remote-rank) flows in
     *        GOBACKN mode: the receiver only accepts the next expected
     *        sequence number per flow, discards out-of-order arrivals
     *        (no delivery, no ACK), and emits ONE NAK per gap (further OOO
     *        arrivals for the same gap are dropped silently, like an IB
     *        responder). Duplicates below the expected seq are re-ACKed only.
     *        Default false = legacy bypass-mode dedup (keeps OOO originals).
     *        Su (local-rank) traffic is never affected: on the 18-lane sprayed
     *        NVLink fabric out-of-order arrival is normal, not loss.
     */
    void SetSoGbnStrict(bool enable);

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
    // Scale-out (So) protocol model for inter-node destinations. When set, the
    // endpoint uses it for remote-rank timing (e.g. Ethernet RoCEv2) while
    // keeping the scale-up (Su) m_protocolModel for local ranks. Null (default)
    // -> GetProtocolModelForDest falls back to m_protocolModel everywhere, so
    // single-node and no-So-model behavior is byte-identical (parity-safe).
    void SetInterNodeProtocolModel(Ptr<ProtocolModel> model);
    Ptr<ProtocolModel> GetProtocolModelForDest(uint16_t destRank) const;

    // Protocol payload builder
    void SetProtocolPayloadBuilder(Ptr<ProtocolPayloadBuilder> builder);
    Ptr<ProtocolPayloadBuilder> GetProtocolPayloadBuilder() const;

    // Inter-node (So) payload builder seam, mirroring SetInterNodeProtocolModel:
    // when non-null the endpoint frames remote-rank bulk sends with it (and the
    // So protocol model's own protocol id) while local-rank (Su) traffic keeps
    // m_payloadBuilder. Null (default) -> m_payloadBuilder everywhere,
    // byte-identical (parity-safe). Inline runs mount a RocePayloadBuilder here
    // to light the RoCE SoHeader/ECN/CNP/DCQCN chain without touching Su framing.
    void SetSoPayloadBuilder(Ptr<ProtocolPayloadBuilder> builder);
    Ptr<ProtocolPayloadBuilder> GetPayloadBuilderForRank(uint16_t rank) const;

    // Link degradation
    void SetLinkDegradationModel(Ptr<LinkDegradationModel> model);
    Ptr<LinkDegradationModel> GetLinkDegradationModel() const;

    // Reorder buffer control
    void SetBypassReorderBuffer(bool bypass);
    bool GetBypassReorderBuffer() const;

    void GetReorderBufferStats(uint32_t& totalReorderEvents, uint32_t& maxOccupancy) const;
    uint32_t GetTxPackets() const { return m_txPackets; }
    uint32_t GetRxPackets() const { return m_rxPackets; }
    uint64_t GetCnpPacketsSent() const { return m_cnpPacketsSent; }
    uint64_t GetCnpPacketsReceived() const { return m_cnpPacketsReceived; }

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

    /**
     * @brief Sequence counter key for MEMORY_* packets (read/write/response/
     * semantic requests)
     *
     * Memory packets ride their own counter namespace, disjoint from the
     * DATA/collective counter space MakeSeqKey(dest, vc, flow) covers: the
     * receiver's reorder/bypass per-flow sequence tracking only sees
     * ProcessDataPacket traffic, so a memory seq drawn from the shared
     * (dest, vc=0, flow=0) counter leaves a permanent gap in the DATA flow
     * (DATA seq N+1 waits forever for the seq N a MEMORY_RESP consumed).
     * Bit 63 is never set by MakeSeqKey (dest occupies bits 24-39), so the
     * namespaces cannot collide.
     */
    static uint64_t MakeMemorySeqKey(uint16_t destRank)
    {
        return MakeSeqKey(destRank, 0, 0) | (1ULL << 63);
    }

    /// Wire flow id stamped on every MEMORY_* packet. Memory senders stamp
    /// flow 0 on the wire, the same flow id DATA/collective traffic uses, so
    /// every identity-bearing receiver structure keyed on (vc, flow, seq)
    /// collides memory with DATA: the CREDIT dedup (m_creditsReturned)
    /// discards DATA flow 0's CREDIT as a duplicate once a memory packet with
    /// the same seq was re-credited (with one credit the next DATA send
    /// blocks forever), and two LLR RetryKey entries with the same
    /// (dest, flow=0, vc=0, seq) alias — the second store overwrites the
    /// first's retransmit payload. MEMORY_FLOW_ID is outside the flow-id
    /// space the collective drivers allocate (they number their flows from 1
    /// upward, 16 bits wide; 0xFFFF is reserved for memory).
    static constexpr uint16_t MEMORY_FLOW_ID = 0xFFFF;

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
    /**
     * \brief Re-arm the LLR retry timer at first wire transmission. The
     *        timer otherwise starts at StorePacket (enqueue) and can expire
     *        while a pacer/contention/FEC hold still gates the send — a
     *        loss-free link would then produce permanent losses.
     */
    void ArmRetryTimeoutOnWire(Ptr<Packet> packet);
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
     * @brief In bypass mode, run strict-GBN acceptance / duplicate suppression
     *        on a DATA packet and deliver it if it passes.
     *
     * Shared by ProcessDataPacket and ProcessProtocolDataPacket (the latter
     * after payload extraction) so protocol-aware packets get the same
     * duplicate/GBN guards as raw ones. Returns true if the packet was
     * handled here (delivered, discarded as a duplicate, or NAKed).
     */
    bool DeliverBypassDataPacket(Ptr<Packet> packet, const FabricHeader& header);

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
    /**
     * @brief Handle a PERMANENT_LOSS notice from the sender.
     *
     * The retransmit budget for header.GetSequenceNumber() is exhausted; the
     * sender already returned the credit in NotifyPermanentLoss. Strict GBN
     * flows advance their expected counter past the lost seq (parking
     * out-of-order notices); reorder-buffer flows mark a permanent gap and
     * release everything parked below it. Shared by the FEC and non-FEC
     * receive paths.
     */
    void ProcessPermanentLossNotice(const FabricHeader& header);
    void OnLlrRetryTimeout(uint32_t seqNum, uint16_t destRank, uint16_t flowId, uint8_t vcId);
    void DispatchRetransmissions(const std::vector<LlrManager::Retransmission>& retransmissions);
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
    Ptr<ReorderBuffer> GetOrCreateReorderBuffer(uint8_t vcId, uint16_t flowId,
                                                uint16_t sourceRank = 0);

    private:
    bool ReceiveFromDevice(Ptr<NetDevice> device, Ptr<const Packet> packet,
                           uint16_t protocol, const Address& source,
                           const Address& destination, NetDevice::PacketType packetType);

    void SendCreditPacket(uint16_t destRank, uint8_t vcId, uint16_t count,
                          uint32_t seqNum, uint16_t flowId);
    void SendLlrAckPacket(const FabricHeader& receivedHeader, bool cumulative);
    /// @brief Emit a RoCEv2 Congestion Notification Packet back to the source
    /// of an ECN-marked data packet (scale-out ECN echo). Mirrors the
    /// SendCreditPacket control-packet shape so it rides the same lossless
    /// control path and bypasses BER/ECN marking.
    void SendCnpPacket(const FabricHeader& markedHeader);

    /// @brief Receive-side handler for an inbound CNP: drives the DCQCN rate
    ///        machine (per-flow Alpha backoff) for the flow the CNP echoes.
    ///        No-op when no machine is mounted (dormant baseline).
    void ProcessCnpPacket(const FabricHeader& cnpHeader);

    /// @brief Emit an RTTCC RTT probe on a flow (control packet: rides the
    ///        same VOQ queues as data — the RTT sample sees congestion — but
    ///        bypasses BER/tail-drop/LLR/shaping). Timestamps stay sender-
    ///        local in the RttccRateMachine, keyed by probeSeq.
    void SendRttProbePacket(uint16_t destRank, uint8_t vcId, uint16_t flowId,
                            uint32_t probeSeq);
    /// @brief Reflect an inbound RTT probe back to its source (hardware-style
    ///        immediate echo; needs no rate machine on the receiver).
    void SendRttResponsePacket(const FabricHeader& probeHeader);
    /// @brief Receive-side handler for an RTT_RESPONSE: turns the echo into
    ///        an RTT sample for the RTTCC machine. No-op when unmounted.
    void ProcessRttResponsePacket(const FabricHeader& respHeader);
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
    std::set<uint8_t> m_configuredVcs;              ///< VCs whose credits were set explicitly (SetVcCredits, credits>0) — exempt from the lazy 64-credit default init
    FlowControlPolicy m_flowControlPolicy{FlowControlPolicy::CREDIT}; ///< Policy seam (default: credit)
    Ptr<DcqcnRateMachine> m_dcqcnRate{nullptr}; ///< DCQCN rate machine (So-only; null=dormant)
    Ptr<RttccRateMachine> m_rttccRate{nullptr}; ///< RTTCC rate machine (So-only; null=dormant)
    std::unordered_map<uint64_t, Ptr<ReorderBuffer>> m_reorderBuffers; ///< Per-(sourceRank,VC,flowId) reorder buffers, key=(srcRank<<32)|(vcId<<16)|flowId

    // Send queue
    std::queue<SendQueueEntry> m_sendQueue;         ///< Pending send queue
    std::unordered_map<uint64_t, uint32_t> m_nextSeqNum; ///< Next sequence number per (destRank, vcId) key
    /// Deferred-egress events scheduled by SendPacketOnDevice (pacer/FEC/WFQ
    /// holds). Each captures raw `this` and touches m_devices, so DoDispose
    /// must cancel every pending one before the endpoint goes away — a
    /// survivor fires into cleared state (codex round 13, finding 2).
    std::vector<EventId> m_deferredEgressEvents;

    // Configuration
    uint8_t m_numVcs;                               ///< Number of virtual channels
    uint64_t m_launchDelayNs;                       ///< NCCL launch delay
    uint32_t m_bulkChunkSize;                       ///< Maximum packet size for bulk transfers

    // So TX window (NIC send-queue depth, perftest tx_depth analog).
    // 0 = legacy unbounded synchronous bulk dispatch (parity default).
    struct BulkTxChunk
    {
        uint16_t destRank;
        uint16_t flowId;
        uint8_t vcId;
        uint8_t protocolId;
        uint32_t wireChunk;         ///< on-wire bytes for this chunk
        uint32_t fragmentDataSize;  ///< effective data bytes for this chunk
        uint32_t chunkIndex;        ///< index within the bulk transfer (SoHeader)
        uint32_t deviceIndex;       ///< egress device chosen at enqueue (spray order)
        bool stamp;                 ///< stamp SoHeader (RoCE Seam B)
    };
    uint32_t m_soTxWindow{0};                       ///< Max in-flight bulk chunks to remote ranks (0=off)
    uint32_t m_bulkTxInflight{0};                   ///< Chunks dispatched but not yet ACK-purged
    std::deque<BulkTxChunk> m_bulkTxQueue;          ///< Chunks awaiting a window slot
    /// (destRank, seqNum) of window-dispatched bulk chunks, for exact slot
    /// release: only a packet that OCCUPIED a slot (PumpBulkTxWindow
    /// dispatch) may free one — small unfragmented remote sends and local
    /// (Su) packets share the LLR store but never touch the window.
    /// (destRank, vcId, flowId, seqNum) of window-dispatched bulk chunks, for
    /// exact slot release: only a packet that OCCUPIED a slot (PumpBulkTxWindow
    /// dispatch) may free one. The key must carry the full flow identity —
    /// seq allocation is per (dest, vc, flow), so two flows to one dest run
    /// independent seq counters and collide on bare (dest, seq): an ACK from
    /// an unwindowed flow would release another flow's bulk slot.
    std::set<std::tuple<uint16_t, uint8_t, uint16_t, uint32_t>> m_bulkTxWindowSeqs;
    void PumpBulkTxWindow();                        ///< Dispatch queued chunks while window has room
    void ReleaseBulkTxSlot();                       ///< Free one window slot and pump

    // Strict So go-back-N receive semantics (opt-in, see SetSoGbnStrict)
    bool m_soGbnStrict{false};                      ///< In-order-only So receive in GOBACKN mode
    std::map<uint64_t, uint32_t> m_strictNextExpected; ///< Per-flow next expected seq (strict GBN)
    std::map<uint64_t, uint32_t> m_strictNakPending;   ///< Per-flow seq a NAK was already sent for
    std::map<uint64_t, std::set<uint32_t>> m_strictPermLost; ///< Permanently lost seqs ahead of expected
    bool IsStrictGbnFlow(const FabricHeader& header) const; ///< Strict semantics apply to this packet?
    void SendStrictGbnNak(const FabricHeader& dataHeader, uint32_t expectedSeq); ///< One NAK per gap

    // Link degradation
    Ptr<LinkDegradationModel> m_linkDegradationModel; ///< Link degradation model

    // Protocol model (defaults to NcclProtocolModel)
    Ptr<ProtocolModel> m_protocolModel;               ///< Vendor-specific protocol model
    Ptr<ProtocolModel> m_interNodeProtocolModel{nullptr}; ///< Inter-node (So) model; null -> use m_protocolModel

    // Protocol payload builder (defaults to NcclProtocolPayloadBuilder)
    Ptr<ProtocolPayloadBuilder> m_payloadBuilder;     ///< Vendor-specific payload builder
    Ptr<ProtocolPayloadBuilder> m_soPayloadBuilder{nullptr}; ///< Inter-node (So) builder; null -> use m_payloadBuilder

    // Reorder buffer control
    bool m_bypassReorderBuffer;                     ///< Bypass reorder buffer for direct links
    uint32_t m_bypassReorderEvents{0};              ///< Out-of-order arrivals counted in bypass mode
    std::map<uint64_t, uint32_t> m_bypassNextExpectedSeq; ///< Next expected seq per flow (bypass mode)
    std::set<std::pair<uint64_t, uint32_t>> m_bypassDelivered; ///< (flowKey, seqNum) already delivered
    /// (flowKey, seqNum) MEMORY_* packets already EXECUTED (reads serviced,
    /// writes applied, responses delivered). GO_BACK_N retransmission is a
    /// blanket resend: a retransmitted memory packet arrives twice, and the
    /// memory path has no reorder buffer to swallow the duplicate. A
    /// duplicate is still ACKed and credit-returned, but never re-executed —
    /// a retried write would revert newer data, a retried read would mint a
    /// fresh response seq that defeats response dedup.
    std::set<std::pair<uint64_t, uint32_t>> m_memoryDelivered;

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
    /// (ackKey, seqNum) credits already received from a peer. The receiver
    /// re-credits a duplicate delivery in case the original CREDIT was lost
    /// (CREDIT is BER-exempt but still subject to the loss-rate draw). When
    /// the original CREDIT also arrives, the second one is phantom — the
    /// send consumed exactly one credit and the first CREDIT already
    /// returned it — so counting both grows the VC pool beyond its budget
    /// and dispatches queued packets early.
    std::set<std::pair<uint64_t, uint32_t>> m_creditsReturned;

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
    uint64_t m_cnpPacketsSent;     ///< CNPs emitted (receiver saw ECN-marked RoCE data)
    uint64_t m_cnpPacketsReceived; ///< CNPs received back at the flow source
};

// Backward compatibility typedef
using GpuEndpoint = FabricEndpoint;

} // namespace ns3

#endif /* FABRIC_ENDPOINT_H */
