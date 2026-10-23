/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * NVSwitch Hardware Model for L2 routing and VOQ scheduling
 */

#ifndef NVSWITCH_H
#define NVSWITCH_H

#include "fabric-header.h"
#include "fabric-switch.h"
#include "fec-model.h"
#include "link-degradation.h"
#include "voq-entry.h"
#include "arbiter.h"

#include "ns3/mac48-address.h"
#include "ns3/node.h"
#include "ns3/channel.h"
#include "ns3/traced-callback.h"

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <deque>
#include <set>
#include <cstdint>
#include <functional>

namespace ns3
{

/**
 * @brief Hash function for Mac48Address in unordered_map
 */
struct Mac48AddressHash
{
    std::size_t operator()(const Mac48Address& addr) const
    {
        uint8_t buffer[6];
        addr.CopyTo(buffer);
        std::size_t h = 0;
        for (int i = 0; i < 6; ++i)
        {
            h = h * 31 + buffer[i];
        }
        return h;
    }
};

/**
 * @brief Hash function for pair<Mac48Address, uint16_t> for source-based routing
 */
struct MacRankPairHash
{
    std::size_t operator()(const std::pair<Mac48Address, uint16_t>& p) const
    {
        Mac48AddressHash macHash;
        return macHash(p.first) * 31 + p.second;
    }
};

/**
 * @ingroup gpu-cluster
 * @brief Wire vs effective chunk size for a per-GPU All-Reduce contribution
 */
struct AllReduceChunkSize
{
    uint32_t wireBytes;
    uint32_t effectiveBytes;
};

/**
 * @ingroup gpu-cluster
 * @brief NVSwitch Model
 *
 * This class models an NVSwitch that:
 * - Performs L2 MAC learning and routing
 * - Implements Virtual Output Queues (VOQ)
 * - Has Crossbar arbitration with RR scheduling
 * - Supports In-Network Computing (All-Reduce aggregation)
 */
class NvSwitch : public FabricSwitch
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    NvSwitch();
    ~NvSwitch() override;

    // Delete copy constructor and assignment operator
    NvSwitch(const NvSwitch&) = delete;
    NvSwitch& operator=(const NvSwitch&) = delete;

    /**
     * @brief Add a port (NetDevice) to the switch
     * @param device The NetDevice to add as a port
     * @return Port number assigned
     */
    uint32_t AddPort(Ptr<NetDevice> device) override;

    /**
     * @brief Get the number of ports
     * @return Number of ports
     */
    uint32_t GetNPorts() const override;

    /**
     * @brief Get a port by index
     * @param index Port index
     * @return The NetDevice for that port
     */
    Ptr<NetDevice> GetPort(uint32_t index) const override;

    /**
     * @brief Set VOQ depth (number of packets per output queue)
     * @param depth Queue depth in packets
     */
    void SetVoqDepth(uint32_t depth);

    /**
     * @brief Set arbitration interval
     * @param interval Arbitration interval in nanoseconds
     */
    void SetArbitrationInterval(uint64_t intervalNs);

    /**
     * @brief Set the crossbar arbitration strategy.
     *
     * Default is RoundRobinArbiter (the historical non-blocking crossbar
     * model). Pass a subclass of Arbiter (e.g. a weighted / strict-priority
     * policy) to adapt a different arbitration method without touching the
     * switch datapath. Must be set before the simulation starts.
     * @param arbiter The arbitration strategy (must not be null)
     */
    void SetArbiter(Ptr<Arbiter> arbiter);

    /**
     * @brief Get the crossbar arbitration strategy in use.
     */
    Ptr<Arbiter> GetArbiter() const;

    /**
     * @brief Set cut-through forwarding delay
     * @param delayNs Cut-through latency in nanoseconds (0=store-and-forward)
     */
    void SetCutThroughDelay(uint64_t delayNs);

    /**
     * @brief Enable/disable All-Reduce aggregation
     * @param enable True to enable
     */
    void SetAllReduceEnabled(bool enable);

    /**
     * @brief Set All-Reduce aggregation threshold (number of GPUs)
     * @param threshold Number of GPUs that must contribute before aggregating
     */
    void SetAllReduceThreshold(uint32_t threshold);

    /**
     * @brief Set All-Reduce aggregation delay (switch reduction latency)
     * @param delayNs Delay in nanoseconds for in-switch reduction
     */
    void SetAllReduceAggregationDelay(uint64_t delayNs);

    /**
     * @brief Set expected data size per GPU for SHARP AllReduce
     * @param size Data size in bytes
     */
    void SetAllReduceDataSize(uint64_t size);

    /**
     * @brief Set number of partitions for pipelined NVLS AllReduce multicast
     * @param numPartitions Number of partitions (0=old single-multicast mode, >=2=pipelined)
     */
    void SetAllReduceNumPartitions(uint32_t numPartitions);

    /**
     * @brief Set link degradation model for a specific port
     * @param port Port index
     * @param model Link degradation model
     */
    void SetPortDegradationModel(uint32_t port, Ptr<LinkDegradationModel> model);

    /**
     * @brief Enable/disable LLR on switch NVLink ports
     * When enabled, the switch sends NACK back on CRC/FEC failure at ingress,
     * triggering endpoint retry. Matches real NVLink behavior where both link
     * endpoints detect errors and signal for retransmission.
     * @param enabled True to enable LLR on switch ports
     */
    void SetLlrEnabled(bool enabled);

    /**
     * @brief Set FEC model for switch port BER decode
     * NVLink FEC operates at the link level: both GPU and NVSwitch ASIC endpoints
     * have FEC encode/decode on their NVLink ports. This model enables FEC
     * decode on switch ingress, matching physical NVLink behavior.
     * @param model FEC model (shared with endpoint FEC configuration)
     */
    void SetFecModel(Ptr<FecModel> model);

    /**
     * @brief Apply FEC only to ports tagged as optical.
     */
    void SetFecOpticalOnly(bool opticalOnly);

    /**
     * @brief Get FEC model
     */
    Ptr<FecModel> GetFecModel() const;

    /**
     * @brief Get link degradation model for a specific port
     * @param port Port index
     * @return Link degradation model (nullptr if not set)
     */
    Ptr<LinkDegradationModel> GetPortDegradationModel(uint32_t port) const;

    /**
     * @brief Enable/disable All-Gather aggregation (NVLS)
     * @param enable True to enable
     */
    void SetAllGatherEnabled(bool enable);

    /**
     * @brief Set All-Gather threshold (number of GPUs for aggregation)
     * @param threshold Number of GPUs that must contribute
     */
    void SetAllGatherThreshold(uint32_t threshold);

    /**
     * @brief Set expected chunk size per GPU for NVLS AllGather
     * @param size Chunk size in bytes (dataSize/numGpus)
     */
    void SetAllGatherChunkSize(uint64_t size);

    /**
     * @brief Set expected full data size for NVLS AllGather broadcast
     * @param size Full data size in bytes
     */
    void SetAllGatherDataSize(uint64_t size);

    /**
     * @brief Add static MAC routing entry (for pre-configured FatTree routing)
     * @param addr Destination MAC address
     * @param port Output port number
     */
    void AddStaticRoute(Mac48Address addr, uint32_t port) override;

    /**
     * @brief Register a rank-to-port mapping for multicast result delivery.
     *
     * m_portRankMap only learns from GPUs that UPLOAD collective fragments, so
     * a rank whose chunk is zero-sized under the frozen chunk grid (rank >=
     * dataSize when dataSize < numGpus) never appears in it and the multicast
     * result loop skips it — its receiver stalls forever at done=N-1 of N
     * (codex round 19, finding 6). Helpers call this alongside AddStaticRoute
     * to pre-register every rank's port(s); the multicast builders union this
     * table with the learned map so zero-chunk ranks still get the result.
     */
    void AddRankPort(uint16_t rank, uint32_t port);

    std::string GetVendorName() const override;

    /**
     * @brief Add source-based routing entry (for crossbar non-blocking behavior)
     * Routes based on both destination MAC and source rank
     * @param dstMac Destination MAC address
     * @param srcRank Source GPU rank
     * @param port Output port number
     */
    void AddSourceBasedRoute(Mac48Address dstMac, uint16_t srcRank, uint32_t port);

    /**
     * @brief Enable source-based routing mode
     * When enabled, routes based on both source rank and destination MAC
     * @param enable True to enable source-based routing
     */
    void SetSourceBasedRouting(bool enable);

    /**
     * @brief Add multiple ports for spray routing to a destination
     * Packets to this destination will be distributed round-robin across these ports
     * @param dstMac Destination MAC address
     * @param ports List of output ports for this destination
     */
    void AddSprayPorts(Mac48Address dstMac, const std::vector<uint32_t>& ports);

    /**
     * @brief Enable spray routing mode
     * When enabled, distributes packets round-robin across destination's ports
     * @param enable True to enable spray routing
     */
    void SetSprayRouting(bool enable);

    /**
     * @brief Remove installed forwarding state before rebuilding routes.
     */
    void ClearRoutingTables();

    /**
     * @brief Drop packets on a route miss instead of flooding them.
     *
     * Failure-aware routing enables this after it installs a complete set of
     * routes over the surviving topology. Flooding a disconnected destination
     * would otherwise create loops and hide the loss of reachability.
     */
    void SetFailureAwareRouting(bool enable);

    // From NetDevice base class
    void SetIfIndex(const uint32_t index) override;
    uint32_t GetIfIndex() const override;
    Ptr<Channel> GetChannel() const override;
    void SetAddress(Address address) override;
    Address GetAddress() const override;
    bool SetMtu(const uint16_t mtu) override;
    uint16_t GetMtu() const override;
    bool IsLinkUp() const override;
    void AddLinkChangeCallback(Callback<void> callback) override;
    bool IsBroadcast() const override;
    Address GetBroadcast() const override;
    bool IsMulticast() const override;
    Address GetMulticast(Ipv4Address multicastGroup) const override;
    Address GetMulticast(Ipv6Address address) const override;
    bool IsPointToPoint() const override;
    bool IsBridge() const override;
    bool Send(Ptr<Packet> packet, const Address& dest, uint16_t protocolNumber) override;
    bool SendFrom(Ptr<Packet> packet, const Address& source, const Address& dest, uint16_t protocolNumber) override;
    Ptr<Node> GetNode() const override;
    void SetNode(Ptr<Node> node) override;
    bool NeedsArp() const override;
    void SetReceiveCallback(NetDevice::ReceiveCallback cb) override;
    void SetPromiscReceiveCallback(PromiscReceiveCallback cb) override;
    bool SupportsSendFrom() const override;
    uint32_t GetTxPackets() const { return m_txPackets; }
    uint32_t GetRxPackets() const { return m_rxPackets; }
    uint32_t GetDroppedPackets() const { return m_droppedPackets; }
    uint32_t GetUnknownPortDrops() const { return m_unknownPortDrops; }
    uint32_t GetLinkErrorDrops() const { return m_linkErrorDrops; }
    uint32_t GetTtlDrops() const { return m_ttlDrops; }
    uint32_t GetVoqDrops() const { return m_voqDrops; }
    uint32_t GetRouteUnavailableDrops() const { return m_routeUnavailableDrops; }
    uint32_t GetSharpDuplicateUploads() const { return m_sharpDuplicateUploads; }
    /// Monotonic count of multicast result packets emitted (0 = no result yet).
    uint32_t GetMulticastSeqNum() const { return m_multicastSeqNum; }

  protected:
    void DoDispose() override;

    /**
     * @brief Test seam: feed an upload packet straight into the SHARP/NVLS
     * aggregation path, bypassing ReceiveFromPort's routing/preamble.
     *
     * Mirrors the FabricEndpoint test subclasses' Inject* pattern. Used by
     * the upload-dedup regression test to drive ProcessAllGather without a
     * full fabric.
     */
    void InjectAggregateUpload(uint16_t flowId, uint16_t srcRank, uint32_t seqNum,
                               uint32_t effectiveSize, bool allGather);

    /**
     * @brief Test seam: drive the multicast-result retransmit path directly
     * with a receiver NACK (DestRank 0xFFFF), bypassing the p2p loopback.
     *
     * Returns HandleSwitchNack's verdict: true when a stored result matched
     * and was retransmitted. Used by the result-cache eviction regression
     * test.
     */
    bool InjectResultNack(uint16_t requestingRank, uint32_t seqNum);

    /**
     * @brief Test seam: number of result copies currently retained for a
     * destination rank in the switch-side LLR store.
     */
    size_t GetSharpResultStoreSize(uint16_t destRank) const;

    /**
     * @brief Congestion hook called when a packet is enqueued to a VOQ.
     *
     * Returns false to DROP the packet (the caller skips the push and counts
     * a VOQ drop); returns true to admit it. Default returns true (no-op, so
     * Su parity is preserved on NVSwitch instances). A scale-out switch
     * overrides this: EthernetSwitch marks the SoHeader ECN bit on RoCE data
     * packets (lossless), and tail-drops plain-Ethernet data packets past the
     * VoqDropThreshold with a NACK to the source (lossy). At call time the
     * packet still carries its FabricHeader (outermost) over any inner
     * SoHeader, matching the in-flight state.
     * @param packet The packet being enqueued (mutable)
     * @param outPort Output port the packet is queued for
     * @param inPort Input port the packet arrived on (return path for a NACK)
     * @param voqDepth Current VOQ depth (packets, excluding this one)
     * @return true to admit (enqueue); false to drop (caller counts m_voqDrops)
     */
    virtual bool OnPacketEnqueue(Ptr<Packet> packet, uint32_t outPort,
                                 uint32_t inPort, size_t voqDepth);

    /**
     * @brief Emit a NACK control packet back to the source of @p orig on @p returnPort.
     *
     * Used by the lossy Ethernet drop path: when a plain-Ethernet data packet
     * is tail-dropped at the VoqDropThreshold, the switch tells the sender to
     * retransmit via the existing LLR/SACK path. Mirrors the BER-NACK emission
     * in ReceiveFromPort. No-op when LLR is disabled (the sender could not
     * process the NACK anyway). Protected so EthernetSwitch (which cannot reach
     * the private m_ports table) can call it.
     * @param orig The dropped packet's FabricHeader (source becomes NACK dest)
     * @param returnPort Port back toward the source (the packet's input port)
     * @param destMac L2 destination for the NACK (the source's NIC MAC)
     */
    void EmitNackBack(const FabricHeader& orig, uint32_t returnPort,
                      Mac48Address destMac);

  private:
    /**
     * @brief Receive handler for packets from ports (promiscuous mode)
     * @return Always true to indicate packet was processed
     */
    bool ReceiveFromPort(Ptr<NetDevice> device, Ptr<const Packet> packet,
                         uint16_t protocol, const Address& source,
                         const Address& destination, NetDevice::PacketType packetType);

    /**
     * @brief Learn source MAC address
     */
    void LearnMacAddress(Mac48Address addr, uint32_t port);

    /**
     * @brief Lookup output port for destination MAC
     */
    int32_t LookupOutputPort(Mac48Address addr);

    /**
     * @brief Return true when the output port and its modeled link are usable.
     */
    bool IsPortOperational(uint32_t port) const;

    /**
     * @brief Dedup gate for NVLS/SHARP upload packets.
     *
     * Returns true the FIRST time (flowId, srcRank, seqNum) is seen; false on
     * every retransmission of an already-counted upload. The aggregation
     * ledgers (received bytes + chunk lists) must only advance on the first
     * arrival — a retransmitted upload whose LLR ack was lost otherwise
     * double-counts and the switch assembles a "complete" result from fewer
     * unique bytes than the collective requires.
     */
    bool RegisterUploadSeq(uint16_t flowId, uint16_t srcRank, uint32_t seqNum);

    /**
     * @brief Enqueue packet to VOQ
     */
    void EnqueueToVoq(uint32_t outPort, const VoqEntry& entry);

    /**
     * @brief Arbitrate and forward packets
     */
    void Arbitrate();

    /**
     * @brief Schedule next arbitration event
     */
    void ScheduleArbitration();

    /**
     * @brief Forward packet to output port
     */
    void ForwardPacket(Ptr<Packet> packet, Mac48Address srcAddr,
                       Mac48Address dstAddr, uint32_t outPort);

    bool PortUsesFec(uint32_t port) const;

    /**
     * @brief Process All-Reduce packets (SHARP in-network reduction)
     */
    void ProcessAllReduce(Ptr<Packet> packet, const FabricHeader& header,
                          uint32_t inPort);

    /**
     * @brief Multicast reduced result to all ports after aggregation delay
     */
    void MulticastAllReduceResult(uint16_t flowId, uint32_t sourceCount);

    /**
     * @brief Multicast a single partition of reduced result (pipelined mode)
     */
    void MulticastAllReducePartition(uint16_t flowId, uint32_t partitionIdx,
                                      uint64_t effectivePartitionSize,
                                      uint64_t wirePartitionSize,
                                      uint32_t numPartitions);

    // Member variables
    Ptr<Node> m_node;                               ///< The node owning this device
    Mac48Address m_address;                         ///< Switch MAC address
    std::vector<Ptr<NetDevice>> m_ports;            ///< Switch ports
    uint32_t m_ifIndex;                             ///< Interface index
    uint16_t m_mtu;                                 ///< MTU

    // MAC learning table: MAC -> port
    std::unordered_map<Mac48Address, uint32_t, Mac48AddressHash> m_macTable;

    // Source-based routing table: (dstMac, srcRank) -> port (for crossbar non-blocking)
    std::unordered_map<std::pair<Mac48Address, uint16_t>, uint32_t, MacRankPairHash> m_sourceBasedTable;
    bool m_sourceBasedRoutingEnabled;  ///< Flag to enable source-based routing

    // Spray routing: destination MAC -> list of ports for round-robin distribution
    std::unordered_map<Mac48Address, std::vector<uint32_t>, Mac48AddressHash> m_sprayPortsTable;
    std::unordered_map<Mac48Address, uint32_t, Mac48AddressHash> m_sprayRoundRobin;  ///< Current index per destination
    bool m_sprayRoutingEnabled;  ///< Flag to enable spray routing
    bool m_failureAwareRoutingEnabled; ///< Route misses indicate disconnection

    // Virtual Output Queues: outPort -> queue
    std::vector<std::queue<VoqEntry>> m_voqs;
    uint32_t m_voqDepth;                            ///< Max packets per VOQ

    // Link degradation
    std::vector<Ptr<LinkDegradationModel>> m_portDegradationModels; ///< Per-port degradation models
    Ptr<FecModel> m_fecModel;                     ///< FEC decode model (shared with endpoints)
    bool m_fecOpticalOnly = false;                ///< Restrict FEC to optical ports
    bool m_llrEnabled = false;                    ///< LLR enabled on switch NVLink ports

    // Arbitration
    uint64_t m_arbitrationIntervalNs;               ///< Arbitration interval
    uint64_t m_cutThroughDelayNs = 200;              ///< Cut-through forwarding delay (ns), 0=store-and-forward
    EventId m_arbitrationEvent;                     ///< Scheduled arbitration event
    uint32_t m_currentArbitrationPort;              ///< Current port for RR arbitration
    std::vector<Time> m_outputBusyUntil;            ///< Per-port egress busy time (serialization)
    Ptr<Arbiter> m_arbiter;                         ///< Crossbar arbitration strategy (default: RoundRobinArbiter)
    /// Deferred collective-result multicasts (aggregation delay + FEC-coding
    /// egress holds). Each callback targets raw `this` (bound member call or
    /// lambda capture) and dereferences state DoDispose clears, so DoDispose
    /// must cancel whatever is still pending (codex round 15, finding 2).
    std::vector<EventId> m_deferredResultEvents;

    /**
     * @brief Process All-Gather packets (NVLS concatenation + multicast)
     */
    void ProcessAllGather(Ptr<Packet> packet, const FabricHeader& header,
                          uint32_t inPort);

    /**
     * @brief Multicast concatenated result to all ports after aggregation delay
     */
    void MulticastAllGatherResult(uint16_t flowId, uint32_t sourceCount);

    /**
     * @brief LLR ack an accepted SHARP/NVLS upload packet back to its sender.
     *
     * The GPU endpoints store ALLREDUCE/ALLGATHER uploads in their LLR
     * managers (the receive-side store-filter exclusion for these types was
     * removed — without storage a switch-side drop could never be
     * retransmitted and the collective stalled forever). The switch is the
     * receiving end of that link, so it must ACK what it consumed to purge
     * the sender's retry entry; otherwise the entry lives until the retry
     * limit turns it into a false permanent loss. The ack carries
     * SourceRank = the upload's DestRank (the switch-addressed destination,
     * the same key space the switch's BER NACK already uses), so the
     * sender's RemovePacket((seq, dest, flow, vc)) lookup matches.
     */
    void SendSharpLlrAck(const FabricHeader& uploadHeader, uint32_t inPort);

    /**
     * @brief Keep a multicast result packet for switch-side LLR retransmit.
     *
     * Result packets carry SourceRank 0xFFFF (switch sentinel); the endpoint
     * receiver's NACK for a lost one is addressed DestRank 0xFFFF and
     * arrives on the losing GPU's port. HandleSwitchNack looks the packet
     * up here and retransmits it on the stored port. Bounded FIFO per
     * (destination, flow) — a new flow's results REPLACE the old flow's
     * entries wholesale (stale: its collective completed), so the window
     * only ever bounds one flow's fragments.
     */
    void StoreMulticastResult(uint16_t destRank, uint16_t flowId,
                              uint32_t portIdx, uint32_t seqNum, Ptr<Packet> pkt);

    /**
     * @brief Egress helper for multicast result sends (FEC-aware).
     *
     * Pads the packet body to GetEncodedSize on FEC ports (the receiver
     * strips parity via the header's payload size) and holds the send for
     * the codec latency, matching the endpoint egress convention. Direct
     * port->Send calls otherwise skip FEC wire expansion entirely.
     */
    void SendMulticastResultOnPort(uint32_t portIdx, Ptr<Packet> pkt);

    /**
     * @brief Retransmit a multicast result on a receiver NACK (DestRank 0xFFFF).
     *
     * @return true when a stored result matched and was retransmitted.
     */
    bool HandleSwitchNack(const FabricHeader& nackHeader, uint32_t inPort);

    /**
     * @brief Build rank -> ports for a multicast result pass.
     *
     * Unions the upload-learned m_portRankMap with the helper-registered
     * m_rankPortMap so ranks that never uploaded (zero-sized chunk under the
     * frozen grid) still receive the result (codex round 19, finding 6).
     */
    std::unordered_map<uint16_t, std::vector<uint32_t>> BuildRankPorts() const;

    // In-Network Computing (SHARP)
    bool m_allReduceEnabled;                        ///< All-Reduce enabled flag
    uint32_t m_allReduceThreshold;                  ///< Number of GPUs for aggregation
    uint64_t m_allReduceAggregationDelayNs;          ///< Switch reduction latency (ns)
    uint64_t m_allReduceDataSize;                   ///< Expected data size per GPU for SHARP
    uint32_t m_allReduceNumPartitions;              ///< Number of partitions for pipelined multicast (0=single)
    // FlowID -> count of completed partitions (for pipelined multicast)
    std::unordered_map<uint16_t, uint32_t> m_allReduceCompletedPartitions;
    /// Flows whose legacy single-multicast result already went out (codex
    /// round 22, finding 1). The allComplete test re-runs on every upload —
    /// including late padding fragments that land after the multicast
    /// cleared the buffer — and without this marker a second result event
    /// was scheduled onto a cleared buffer (empty begin() deref).
    std::unordered_set<uint16_t> m_allReduceLegacyMulticastDone;
    // Multicast result seqnums are PER (DESTINATION, FLOW): endpoint reorder
    // buffers (the module default, SetBypassReorderBuffer(false)) key on
    // (vc, flow, source) and expect each stream to start at 0 — a counter
    // spanning destinations OR flows leaves every stream after the first
    // buffering its seqs forever awaiting a 0 that never comes. Counters
    // start at 0 for each (destination, flow) pair.
    std::unordered_map<uint64_t, uint32_t> m_multicastSeqNums; ///< (destRank<<16|flowId) -> next seq
    uint32_t m_multicastSeqNum = 0; ///< Total result packets emitted (stats/tests)
    // FlowID -> (sourceRank -> chunk sizes) — track per-GPU contribution sizes only.
    // Full Packet objects are NOT stored to avoid memory explosion (256MB × N GPUs
    // would hold GBs). Data content is dummy; only sizes matter for multicast.
    std::unordered_map<
        uint16_t,
        std::unordered_map<uint16_t, std::vector<AllReduceChunkSize>>> m_allReduceBuffer;
    // FlowID -> (sourceRank -> bytes received) — for chunk-level tracking
    std::unordered_map<uint16_t, std::unordered_map<uint16_t, uint64_t>> m_allReduceReceivedBytes;
    std::unordered_map<uint32_t, uint16_t> m_portRankMap; ///< port -> GPU rank (learned from ALLREDUCE packets)
    // rank -> port(s), pre-registered by helpers (see AddRankPort): covers
    // zero-chunk ranks that never upload and thus never enter m_portRankMap.
    std::unordered_map<uint16_t, std::vector<uint32_t>> m_rankPortMap;

    // NVLS AllGather
    bool m_allGatherEnabled;                        ///< All-Gather enabled flag
    uint32_t m_allGatherThreshold;                  ///< Number of GPUs for AllGather aggregation
    uint64_t m_allGatherChunkSize;                  ///< Expected chunk size per GPU (dataSize/numGpus)
    uint64_t m_allGatherDataSize;                   ///< Full data size for multicast (N * chunkSize)
    // FlowID -> (sourceRank -> bytes received) — track per-GPU chunk contributions
    std::unordered_map<uint16_t, std::unordered_map<uint16_t, uint64_t>> m_allGatherReceivedBytes;
    std::unordered_map<uint16_t, std::unordered_map<uint16_t, std::vector<uint32_t>>> m_allGatherBuffer;

    // Switch-side LLR for multicast results: (destRank<<16|flowId) -> FIFO of
    // (seq, portIdx, retransmitCount, header). Only the stamped header is
    // kept — the packet BODY is dummy bytes (only sizes matter for timing),
    // so a retransmit rebuilds a body of the header's payload size instead of
    // holding a full copy per fragment. Entries live until evicted by the
    // window bound; a NACK re-sends a rebuilt packet (bounded retries).
    struct SharpResultEntry
    {
        uint32_t seqNum;        ///< FabricHeader sequence number of the result
        uint32_t portIdx;       ///< Port the result was multicast on
        uint32_t retransmits;   ///< NACK-driven retransmits issued so far
        FabricHeader header;    ///< Header of the multicast result packet
    };
    std::unordered_map<uint64_t, std::deque<SharpResultEntry>> m_sharpResultStore;
    // destRank -> most recent flow whose results were stored. Bookkeeping
    // only (diagnostics); HandleSwitchNack must NOT use it to re-key a
    // named-flow NACK onto another flow's window (codex round 24, finding 4).
    std::unordered_map<uint16_t, uint16_t> m_lastResultFlow;
    // Result-fragment retention. Result entries belong to a FLOW and the
    // store is keyed per (dest, flow), so concurrent flows keep independent
    // windows — a new flow's results do NOT erase a prior flow's in-flight
    // repair state (completion of one collective does not imply delivery of
    // another's results; codex round 24, finding 4). Each window is bounded
    // by SHARP_RESULT_WINDOW on its own. The eviction harnesses pinned the
    // true scale: 1,024 fragments/dest for an 8-GPU 128-MiB allgather,
    // 16,384 for 2 GiB — and header-only entries are ~144 B, so 65,536
    // entries per (dest, flow) bounds memory at ~9.4 MB per live flow while
    // covering an 8-GPU 8-GiB allgather with headroom. The window is a
    // safety valve for absurd geometries; the observed scales fit well
    // inside it.
    static constexpr size_t SHARP_RESULT_WINDOW = 65536; ///< Max stored results per (dest, flow)
    static constexpr uint32_t SHARP_RESULT_RETRY_LIMIT = 64; ///< Max retransmits per result

    // Upload dedup: (flowId, srcRank) -> seen upload sequence numbers. A
    // retransmitted NVLS/SHARP upload (its LLR ack lost) must not count as a
    // new contribution — the aggregation ledgers (received bytes + chunk
    // lists) would double-count it and return a "complete" result assembled
    // from fewer unique bytes than the collective requires.
    std::unordered_map<uint16_t, std::unordered_map<uint16_t, std::set<uint32_t>>> m_seenUploadSeqs;
    uint32_t m_sharpDuplicateUploads = 0; ///< Duplicate uploads dropped by the dedup gate

    // Callbacks
    NetDevice::ReceiveCallback m_rxCallback;
    NetDevice::PromiscReceiveCallback m_promiscRxCallback;

    // Statistics
    uint64_t m_txBytes;
    uint64_t m_rxBytes;
    uint32_t m_txPackets;
    uint32_t m_rxPackets;
    uint32_t m_droppedPackets;                      ///< Dropped due to VOQ overflow
    uint32_t m_unknownPortDrops;
    uint32_t m_linkErrorDrops;
    uint32_t m_ttlDrops;
    uint32_t m_voqDrops;
    uint32_t m_routeUnavailableDrops;

    // Traced sources
    TracedCallback<Ptr<const Packet>> m_txTrace;
    TracedCallback<Ptr<const Packet>> m_rxTrace;
    TracedCallback<Ptr<const Packet>> m_dropTrace;
};

} // namespace ns3

#endif /* NVSWITCH_H */
