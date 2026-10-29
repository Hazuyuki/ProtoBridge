/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Fabric Endpoint Send Path Implementation
 */

#include "fabric-endpoint.h"

#include "roce-payload-builder.h"  // RocePayloadBuilder::GetTypeId (union gate below)
#include "roce-protocol-model.h"   // RoceProtocolModel::GetTypeId

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("FabricEndpointSend");

void
FabricEndpoint::SendData(uint16_t destRank, const uint8_t* data, uint32_t size,
                          uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << destRank << size << flowId << vcId);

    // Record flow start for latency statistics
    if (m_latencyStatistics)
    {
        m_latencyStatistics->RecordFlowStart(flowId);
    }

    // Fragment when: (a) spraying across multiple links (always fragment to ensure
    // all links are utilized — avoids flat latency when numChunks < numLinks),
    // or (b) data large enough that a single packet would thrash memory across 8
    // concurrent GPUs (copy + VOQ blowup observed at >=256 MB packets).
    std::vector<uint32_t> devices = GetPhysicalDevicesForDest(destRank);
    const uint32_t OVERFLOW_THRESHOLD = 500 * 1024 * 1024;
    const uint32_t MEM_SAFE_THRESHOLD = 128 * 1024 * 1024;
    bool needFragment = (devices.size() > 1) ||
                        (size > MEM_SAFE_THRESHOLD) ||
                        (size > OVERFLOW_THRESHOLD);

    if (needFragment)
    {
        std::vector<uint32_t> sendDevices = devices;
        if (sendDevices.empty())
        {
            sendDevices.push_back(ResolveDeviceIndex(destRank));
        }

        // Ensure at least one chunk per link for proper BW utilization.
        // When numChunks < numLinks, some links sit idle, causing flat latency.
        uint32_t minChunks = static_cast<uint32_t>(sendDevices.size());
        uint32_t sizeBasedChunks = (size + m_sprayChunkSize - 1) / m_sprayChunkSize;
        uint32_t numChunks = std::max(minChunks, sizeBasedChunks);
        // Coalesce very large transfers into memory-safe chunks: tiny 128 KB
        // chunks flood the event loop at GB scale (millions of packet-events),
        // so cap each chunk at MAX_CHUNK_BYTES. Gated on memCapChunks > minChunks
        // so small/medium multi-link transfers keep their 128 KB spray granularity
        // unchanged (only coalesce when 8 MB chunks still keep every link busy).
        // Total wire bytes and path striping are unchanged. Queue and credit
        // events occur at the coalesced granularity, so this path is intended
        // for bulk-transfer timing rather than fine-grained contention tests.
        const uint32_t MAX_CHUNK_BYTES = 8 * 1024 * 1024;
        uint32_t memCapChunks = (size + MAX_CHUNK_BYTES - 1) / MAX_CHUNK_BYTES;
        if (memCapChunks > minChunks)
        {
            numChunks = std::max(minChunks, std::min(sizeBasedChunks, memCapChunks));
        }
        // Bound the chunk count by the transfer size: the one-chunk-per-link
        // spray can otherwise produce trailing fragments whose offset is past
        // the buffer end (size - offset underflows; Create<Packet> would read
        // out of bounds). Never more than one 1-byte chunk per data byte.
        numChunks = std::min(numChunks, std::max<uint32_t>(size, 1));
        uint32_t chunkSize = (size + numChunks - 1) / numChunks;

        NS_LOG_INFO("Fragmenting " << size << " bytes to dest " << destRank
                    << " across " << sendDevices.size() << " links in " << numChunks << " chunks"
                    << " starting at offset " << m_globalSprayOffset);

        for (uint32_t i = 0; i < numChunks; i++)
        {
            uint32_t offset = i * chunkSize;
            if (offset >= size)
            {
                continue; // trailing lane-pad fragment past the buffer end
            }
            uint32_t thisChunkSize = std::min(chunkSize, size - offset);

            // Generate sequence number for each chunk
            uint64_t seqKey = MakeSeqKey(destRank, vcId, flowId);
            uint32_t seqNum = m_nextSeqNum[seqKey]++;

            Ptr<Packet> chunkPacket = Create<Packet>(data + offset, thisChunkSize);

            FabricHeader header;
            header.SetPacketType(FabricPacketType::DATA);
            header.SetFabricType(GetFabricTypeForDest(destRank));
            header.SetFlowId(flowId);
            header.SetSequenceNumber(seqNum);
            header.SetVirtualChannel(vcId);
            header.SetSourceRank(m_rank);
            header.SetDestRank(destRank);
            header.SetPayloadSize(thisChunkSize);
            header.SetEffectiveDataSize(thisChunkSize);
            header.SetSourceMac(m_address);
            header.SetDestMac(ResolveDestMac(destRank));

            // Admission through the virtual FlowControlGate (the documented
            // send-gate seam) — Su (local-dest) traffic only. So (remote-dest)
            // DATA bypasses credits, mirroring SendBulkWireTransferSize /
            // SendP2p: the receiver returns credits only for local sources
            // (OnPacketDelivered's IsLocalRank guard), so a credit-consuming
            // remote send would drain the VC pool with no return path and
            // stall after vcCredits sends. The gate performs the
            // HasCredits/ConsumeCredit pair for the CREDIT policy, so a
            // subclass override is no longer bypassed on this path (codex
            // round 29, finding 3).
            if (!FlowControlGate(vcId, seqNum, !IsLocalRank(destRank)))
            {
                NS_LOG_DEBUG("No credits for DATA chunk " << i << " on VC " << static_cast<int>(vcId)
                             << ", queuing remaining chunks");
                // Don't add header here — TrySendQueuedPackets adds it when dequeuing
                SendQueueEntry entry;
                entry.packet = chunkPacket;
                entry.header = header;
                entry.vcId = vcId;
                entry.seqNum = seqNum;
                entry.deviceIndex = sendDevices[(m_globalSprayOffset + i) % sendDevices.size()];
                m_sendQueue.push_back(entry);

                // Queue remaining chunks
                for (uint32_t j = i + 1; j < numChunks; j++)
                {
                    uint32_t remOffset = j * chunkSize;
                    if (remOffset >= size)
                    {
                        continue; // trailing lane-pad fragment past the buffer end
                    }
                    uint32_t remChunkSize = std::min(chunkSize, size - remOffset);
                    uint64_t remSeqKey = MakeSeqKey(destRank, vcId, flowId);
                    uint32_t remSeqNum = m_nextSeqNum[remSeqKey]++;

                    Ptr<Packet> remPacket = Create<Packet>(data + remOffset, remChunkSize);
                    FabricHeader remHeader;
                    remHeader.SetPacketType(FabricPacketType::DATA);
                    remHeader.SetFabricType(GetFabricTypeForDest(destRank));
                    remHeader.SetFlowId(flowId);
                    remHeader.SetSequenceNumber(remSeqNum);
                    remHeader.SetVirtualChannel(vcId);
                    remHeader.SetSourceRank(m_rank);
                    remHeader.SetDestRank(destRank);
                    remHeader.SetPayloadSize(remChunkSize);
                    remHeader.SetEffectiveDataSize(remChunkSize);
                    remHeader.SetSourceMac(m_address);
                    remHeader.SetDestMac(ResolveDestMac(destRank));

                    SendQueueEntry remEntry;
                    remEntry.packet = remPacket;
                    remEntry.header = remHeader;
                    remEntry.vcId = vcId;
                    remEntry.seqNum = remSeqNum;
                    remEntry.deviceIndex = sendDevices[(m_globalSprayOffset + j) % sendDevices.size()];
                    m_sendQueue.push_back(remEntry);
                }
                TrySendQueuedPackets();
                break;
            }

            chunkPacket->AddHeader(header);

            // Select device using round-robin with global offset
            uint32_t deviceIdx = sendDevices[(m_globalSprayOffset + i) % sendDevices.size()];
            Mac48Address destMac = ResolveDestMac(destRank);
            if (!SendPacketOnDevice(chunkPacket, deviceIdx, destMac))
            {
                // PFC-held: parked on the send queue by the chokepoint; do
                // not count a transmission (the resume drains it). KEEP
                // BUILDING the remaining fragments — each parks itself the
                // same way — instead of dropping them (codex round 27,
                // finding 1; same shape as SendBulkWireTransferSize's loop).
                continue;
            }

            m_txTrace(chunkPacket->GetSize(), seqNum);
            m_txPackets++;
            m_txBytes += chunkPacket->GetSize();
        }

        // Increment global offset so next SendData call starts from different link
        m_globalSprayOffset = (m_globalSprayOffset + numChunks) % sendDevices.size();
    }
    else
    {
        // Single packet mode (no spraying)
        uint64_t seqKey = MakeSeqKey(destRank, vcId, flowId);
        uint32_t seqNum = m_nextSeqNum[seqKey]++;

        Ptr<Packet> packet = Create<Packet>(data, size);

        FabricHeader header;
        header.SetPacketType(FabricPacketType::DATA);
        header.SetFabricType(GetFabricTypeForDest(destRank));
        header.SetFlowId(flowId);
        header.SetSequenceNumber(seqNum);
        header.SetVirtualChannel(vcId);
        header.SetSourceRank(m_rank);
        header.SetDestRank(destRank);
        header.SetPayloadSize(size);
        header.SetEffectiveDataSize(size);
        header.SetSourceMac(m_address);
        header.SetDestMac(ResolveDestMac(destRank));

        SendQueueEntry entry;
        entry.packet = packet;
        entry.header = header;
        entry.vcId = vcId;
        entry.seqNum = header.GetSequenceNumber();

        m_sendQueue.push_back(entry);
        TrySendQueuedPackets();
    }
}

// NVLS/SHARP packets (ALLREDUCE, ALLGATHER) are consumed by the NVSwitch's
// dedicated SHARP engine, which has its own buffer pool separate from the
// per-VC credit-based flow control. Real NVLS hardware uses multimem loads
// through this separate path, not NVLink DMA credits. Bypassing credit
// consumption for these packet types prevents artificial deadlock when the
// switch buffers data faster than credits can recycle.
static bool
IsCreditBypassType(FabricPacketType type)
{
    return type == FabricPacketType::ALLREDUCE ||
           type == FabricPacketType::ALLGATHER;
}

void
FabricEndpoint::SendCollective(FabricPacketType type, uint16_t destRank,
                                const uint8_t* data, uint64_t size)
{
    // (void)data — the body is dummy bytes (only sizes matter for timing);
    // the parameter is uint64 so collective drivers computing uint64_t
    // volumes (NVLS AllGather's chunk grid) don't truncate: a 4-GiB chunk
    // through a uint32_t parameter became zero and SendCollectiveBulk
    // aborted the upload (codex round 25, finding 3 — same widening
    // SendBulkWireTransfer received in round 24, finding 5).
    (void)data;
    SendCollectiveBulk(type, destRank, size);
}

void
FabricEndpoint::SendCollectiveBulk(FabricPacketType type, uint16_t destRank, uint64_t size)
{
    SendCollectiveBulk(type, destRank, size, size);
}

void
FabricEndpoint::SendCollectiveBulk(FabricPacketType type,
                                   uint16_t destRank,
                                   uint64_t effectiveSize,
                                   uint64_t wireSize)
{
    NS_LOG_FUNCTION(this << static_cast<int>(type) << destRank << effectiveSize << wireSize);

    NS_ABORT_MSG_IF(effectiveSize == 0 || wireSize == 0,
                    "Collective transfers require nonzero effective and wire sizes");

    // Record flow start for latency statistics (use destRank as flowId)
    if (m_latencyStatistics)
    {
        m_latencyStatistics->RecordFlowStart(destRank);
    }

    // NVLS/SHARP packets bypass credit-based flow control — the switch's
    // SHARP engine has a dedicated buffer pool, not per-VC credits.
    bool creditBypass = IsCreditBypassType(type);

    // Check if spraying is enabled and we have multiple paths
    std::vector<uint32_t> devices = GetPhysicalDevicesForDest(destRank);

    // For collective operations on switched topology, all links reach the switch.
    // Use all available devices when routing returns fewer than available,
    // or when no specific routes exist for the destRank.
    std::vector<uint32_t> sendDevices = devices;
    if (!m_devices.empty() &&
        (sendDevices.empty() || sendDevices.size() < m_devices.size() / 2))
    {
        sendDevices.clear();
        for (uint32_t i = 0; i < m_devices.size(); ++i)
        {
            sendDevices.push_back(i);
        }
    }
    if (sendDevices.empty())
    {
        sendDevices.push_back(ResolveDeviceIndex(destRank));
    }

    // Always fragment when multiple links available for spraying, even for
    // small data. This ensures all links are utilized so transfer time scales
    // as size/aggregateBW, avoiding flat latency when numChunks < numLinks.
    const uint32_t OVERFLOW_THRESHOLD_COLLECTIVE = 500 * 1024 * 1024;
    bool needFragmentColl = (sendDevices.size() > 1) ||
                            (wireSize > OVERFLOW_THRESHOLD_COLLECTIVE);

    if (needFragmentColl)
    {
        // Ensure at least one chunk per link for proper BW utilization.
        uint64_t minChunks = sendDevices.size();
        uint64_t sizeBasedChunks =
            (wireSize + m_sprayChunkSize - 1) / m_sprayChunkSize;
        uint64_t numChunks = std::max(minChunks, sizeBasedChunks);
        // Bound the chunk count by the wire size: the one-chunk-per-link
        // spray can otherwise produce trailing fragments whose offset is
        // past the wire end (wireSize - offset underflows). Same clamp as
        // the DATA spray path.
        numChunks = std::min(numChunks, std::max<uint64_t>(wireSize, 1));

        // Packet event count otherwise grows with every 128 KB spray unit. At
        // multi-GB sizes this dominates wall time. Coalesce only large
        // transfers and retain at least one event per physical path. Total
        // bytes remain unchanged, while queue and credit events become coarser.
        const uint64_t MAX_CHUNK_BYTES = 8ull * 1024 * 1024;
        const uint64_t COALESCE_THRESHOLD = 512ull * 1024 * 1024;
        uint64_t coalescedChunks =
            (wireSize + MAX_CHUNK_BYTES - 1) / MAX_CHUNK_BYTES;
        if (wireSize > COALESCE_THRESHOLD && coalescedChunks > minChunks)
        {
            uint64_t pathBalancedChunks =
                ((coalescedChunks + minChunks - 1) / minChunks) * minChunks;
            numChunks = std::max(minChunks, std::min(sizeBasedChunks, pathBalancedChunks));
        }
        uint64_t chunkSize = (wireSize + numChunks - 1) / numChunks;

        NS_LOG_INFO("Fragmenting collective " << effectiveSize << " effective bytes ("
                    << wireSize << " wire bytes) to dest " << destRank
                    << " across " << sendDevices.size() << " links in " << numChunks << " chunks");

        for (uint64_t i = 0; i < numChunks; i++)
        {
            uint64_t offset = i * chunkSize;
            if (offset >= wireSize)
            {
                continue; // trailing lane-pad fragment past the wire end
            }
            uint32_t thisChunkSize = static_cast<uint32_t>(
                std::min(chunkSize, wireSize - offset));
            // Partition effective bytes PROPORTIONALLY TO THE WIRE BYTES
            // ACTUALLY SENT. The old i*effectiveSize/numChunks split
            // reserved a share for every chunk index including the skipped
            // trailing lane-pad fragments, so those shares were never
            // delivered (a 32-byte-per-GPU 256B allgather on 18 links
            // delivered only 28 bytes and the collective never completed).
            // Anchoring the boundaries to the wire offsets makes the last
            // sent chunk absorb whatever the skipped fragments would have
            // carried and the shares sum to exactly effectiveSize.
            uint64_t wireSpanEnd = std::min(offset + chunkSize, wireSize);
            // 128-bit intermediates: effectiveSize * offset overflows uint64
            // before the division at multi-GB scales (a 6-GiB upload over 18
            // lanes advertised 9.3e9 effective bytes for 6.4e9 payload — the
            // wrapped products inflated the effective ledger and could trip
            // aggregation before the payload arrived; codex round 25,
            // finding 2). __int128 is the codebase's established wide type
            // (core/model/int64x64-128.h).
            uint64_t effectiveBegin = static_cast<uint64_t>(
                static_cast<__int128_t>(effectiveSize) * offset /
                std::max<uint64_t>(wireSize, 1));
            uint64_t effectiveEnd = static_cast<uint64_t>(
                static_cast<__int128_t>(effectiveSize) * wireSpanEnd /
                std::max<uint64_t>(wireSize, 1));
            uint32_t thisEffectiveSize =
                static_cast<uint32_t>(effectiveEnd - effectiveBegin);

            uint64_t seqKey = MakeSeqKey(destRank, 0, 0);
            uint32_t seqNum = m_nextSeqNum[seqKey]++;

            Ptr<Packet> chunkPacket = Create<Packet>(thisChunkSize);

            FabricHeader header;
            header.SetPacketType(type);
            header.SetFabricType(GetFabricTypeForDest(destRank));
            header.SetFlowId(0);
            header.SetSequenceNumber(seqNum);
            header.SetVirtualChannel(0);
            header.SetSourceRank(m_rank);
            header.SetDestRank(destRank);
            header.SetPayloadSize(thisChunkSize);
            header.SetSourceMac(m_address);
            header.SetDestMac(ResolveDestMac(destRank));
            header.SetEffectiveDataSize(thisEffectiveSize);

            // Admission through the virtual FlowControlGate (the documented
            // send-gate seam). creditBypass stays as-is: NVLS/SHARP packets
            // bypass credits — the switch SHARP engine has its own buffer.
            // The gate performs the HasCredits/ConsumeCredit pair for the
            // CREDIT policy, so a subclass override is no longer bypassed on
            // this path (codex round 29, finding 3).
            uint8_t vcId = 0;
            if (!FlowControlGate(vcId, seqNum, creditBypass))
            {
                NS_LOG_DEBUG("No credits for collective chunk " << i << " on VC " << static_cast<int>(vcId)
                             << ", queuing remaining chunks");
                // Queue remaining chunks for later sending when credits arrive
                // Don't add header here — TrySendQueuedPackets adds it when dequeuing
                SendQueueEntry entry;
                entry.packet = chunkPacket;
                entry.header = header;
                entry.vcId = vcId;
                entry.seqNum = seqNum;
                entry.deviceIndex = sendDevices[(m_globalSprayOffset + i) % sendDevices.size()];
                m_sendQueue.push_back(entry);

                // Queue remaining chunks too
                for (uint64_t j = i + 1; j < numChunks; j++)
                {
                    uint64_t remOffset = j * chunkSize;
                    if (remOffset >= wireSize)
                    {
                        continue; // trailing lane-pad fragment past the wire end
                    }
                    uint32_t remChunkSize = static_cast<uint32_t>(
                        std::min(chunkSize, wireSize - remOffset));
                    // Same wire-anchored proportional split as the main loop
                    // (skipped trailing fragments must not eat a share) and
                    // the same 128-bit intermediates (round 25, finding 2).
                    uint64_t remWireSpanEnd = std::min(remOffset + chunkSize, wireSize);
                    uint64_t remEffectiveBegin = static_cast<uint64_t>(
                        static_cast<__int128_t>(effectiveSize) * remOffset /
                        std::max<uint64_t>(wireSize, 1));
                    uint64_t remEffectiveEnd = static_cast<uint64_t>(
                        static_cast<__int128_t>(effectiveSize) * remWireSpanEnd /
                        std::max<uint64_t>(wireSize, 1));
                    uint32_t remEffectiveSize =
                        static_cast<uint32_t>(remEffectiveEnd - remEffectiveBegin);
                    uint64_t remSeqKey = MakeSeqKey(destRank, 0, 0);
                    uint32_t remSeqNum = m_nextSeqNum[remSeqKey]++;

                    Ptr<Packet> remPacket = Create<Packet>(remChunkSize);
                    FabricHeader remHeader;
                    remHeader.SetPacketType(type);
                    remHeader.SetFabricType(GetFabricTypeForDest(destRank));
                    remHeader.SetFlowId(0);
                    remHeader.SetSequenceNumber(remSeqNum);
                    remHeader.SetVirtualChannel(0);
                    remHeader.SetSourceRank(m_rank);
                    remHeader.SetDestRank(destRank);
                    remHeader.SetPayloadSize(remChunkSize);
                    remHeader.SetSourceMac(m_address);
                    remHeader.SetDestMac(ResolveDestMac(destRank));
                    remHeader.SetEffectiveDataSize(remEffectiveSize);

                    SendQueueEntry remEntry;
                    remEntry.packet = remPacket;
                    remEntry.header = remHeader;
                    remEntry.vcId = vcId;
                    remEntry.seqNum = remSeqNum;
                    remEntry.deviceIndex = sendDevices[(m_globalSprayOffset + j) % sendDevices.size()];
                    m_sendQueue.push_back(remEntry);
                }
                // Try to send any queued packets that might have credits
                TrySendQueuedPackets();
                break;
            }

            chunkPacket->AddHeader(header);

            uint32_t deviceIdx = sendDevices[(m_globalSprayOffset + i) % sendDevices.size()];
            Mac48Address destMac = ResolveDestMac(destRank);
            if (!SendPacketOnDevice(chunkPacket, deviceIdx, destMac))
            {
                // PFC-held: parked on the send queue by the chokepoint; do
                // not count a transmission (the resume drains it). KEEP
                // BUILDING the remaining fragments — each parks itself the
                // same way — instead of dropping them (codex round 27,
                // finding 1; same shape as SendBulkWireTransferSize's loop).
                continue;
            }

            m_txTrace(chunkPacket->GetSize(), seqNum);
            m_txPackets++;
            m_txBytes += chunkPacket->GetSize();
        }

        m_globalSprayOffset = static_cast<uint32_t>(
            (m_globalSprayOffset + numChunks) % sendDevices.size());
    }
    else
    {
        // Single packet mode (no spraying or small data) — uses credit queue
        uint64_t seqKey = MakeSeqKey(destRank, 0, 0);
        uint32_t seqNum = m_nextSeqNum[seqKey]++;

        uint32_t packetSize = static_cast<uint32_t>(wireSize);
        uint32_t packetEffectiveSize = static_cast<uint32_t>(effectiveSize);
        Ptr<Packet> packet = Create<Packet>(packetSize);

        FabricHeader header;
        header.SetPacketType(type);
        header.SetFabricType(GetFabricTypeForDest(destRank));
        header.SetFlowId(0);
        header.SetSequenceNumber(seqNum);
        header.SetVirtualChannel(0);
        header.SetSourceRank(m_rank);
        header.SetDestRank(destRank);
        header.SetPayloadSize(packetSize);
        header.SetEffectiveDataSize(packetEffectiveSize);
        header.SetSourceMac(m_address);
        header.SetDestMac(ResolveDestMac(destRank));

        if (creditBypass)
        {
            // NVLS/SHARP: send directly, bypassing the credit QUEUE — but
            // still through the virtual FlowControlGate (the documented
            // send-gate seam), so a subclass overriding the gate sees this
            // admission too (codex round 30, finding 2; creditBypass skips
            // the credit consume, not the gate dispatch).
            if (!FlowControlGate(0, seqNum, true))
            {
                // A gate denial PARKS the packet on the send queue — the
                // same recovery as the multi-fragment loops (codex round
                // 33, finding 2): the pre-fix early return discarded it
                // permanently (a temporary subclass gate denial of a
                // single-packet collective transmitted 0 packets and stored
                // 0 LLR entries; reopening the gate had nothing to drain).
                // The drain re-admits through the same virtual gate with
                // this entry's header-derived bypass (round 32, finding 2).
                NS_LOG_DEBUG("FlowControlGate denied collective single packet; parking");
                SendQueueEntry entry;
                entry.packet = packet;
                entry.header = header;
                entry.vcId = 0;
                entry.seqNum = seqNum;
                m_sendQueue.push_back(entry);
                return;
            }
            packet->AddHeader(header);
            uint32_t deviceIdx = m_devices.empty() ? 0 : (m_globalSprayOffset % m_devices.size());
            Mac48Address destMac = ResolveDestMac(destRank);
            const bool pfcHeld = !SendPacketOnDevice(packet, deviceIdx, destMac);
            if (!pfcHeld && !m_devices.empty())
            {
                m_globalSprayOffset = (m_globalSprayOffset + 1) % m_devices.size();
            }
            if (!pfcHeld)
            {
                m_txPackets++;
                m_txBytes += packetSize;
            }
        }
        else
        {
            SendQueueEntry entry;
            entry.packet = packet;
            entry.header = header;
            entry.vcId = 0;
            entry.seqNum = seqNum;

            m_sendQueue.push_back(entry);
            TrySendQueuedPackets();
        }
    }
}

void
FabricEndpoint::SendP2p(uint16_t destRank, const uint8_t* data, uint32_t size,
                         uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << destRank << size << flowId << vcId);

    uint64_t seqKey = MakeSeqKey(destRank, vcId, flowId);
    uint32_t seqNum = m_nextSeqNum[seqKey]++;

    Ptr<Packet> packet = Create<Packet>(data, size);

    FabricHeader header;
    header.SetPacketType(FabricPacketType::P2P);
    header.SetFabricType(GetFabricTypeForDest(destRank));
    header.SetFlowId(flowId);
    header.SetSequenceNumber(seqNum);
    header.SetVirtualChannel(vcId);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetPayloadSize(size);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));

    // Scale-out (remote dest) P2P bypasses the per-VC credit gate — the same
    // convention SendBulkWireTransferSize applies to So traffic. Credits are
    // returned by the RECEIVER only for intra-node sources
    // (OnPacketDelivered's IsLocalRank guard), so a credit-consuming remote
    // P2P would drain the local VC pool with no return path and stall once
    // vcCredits exhausts.
    // IsLocalRank alone decides (no m_localRankCount > 1 gate — one GPU per
    // node still has remote ranks; see GetProtocolModelForDest). The
    // admission still dispatches through the virtual FlowControlGate (the
    // documented send-gate seam) with the credit-bypass arm set — a subclass
    // overriding the gate sees remote P2P too (codex round 30, finding 2).
    if (!IsLocalRank(destRank))
    {
        if (!FlowControlGate(vcId, seqNum, true))
        {
            NS_LOG_DEBUG("FlowControlGate denied remote P2P on VC "
                         << static_cast<int>(vcId));
            SendQueueEntry entry;
            entry.packet = packet;
            entry.header = header;
            entry.vcId = vcId;
            entry.seqNum = seqNum;
            m_sendQueue.push_back(entry);
            TrySendQueuedPackets();
            return;
        }
        packet->AddHeader(header);
        uint32_t deviceIndex = ResolveDeviceIndex(destRank);
        SendPacketOnDevice(packet, deviceIndex, ResolveDestMac(destRank));
        return;
    }

    SendQueueEntry entry;
    entry.packet = packet;
    entry.header = header;
    entry.vcId = vcId;
    entry.seqNum = header.GetSequenceNumber();

    m_sendQueue.push_back(entry);
    TrySendQueuedPackets();
}

void
FabricEndpoint::SendMemoryRead(uint16_t destRank, uint64_t address, uint32_t size)
{
    NS_LOG_FUNCTION(this << destRank << address << size);

    uint64_t seqKey = MakeMemorySeqKey(destRank);
    uint32_t seqNum = m_nextSeqNum[seqKey]++;

    // Pack address and size into payload for memory read request
    uint8_t payload[12];
    std::memcpy(payload, &address, 8);
    std::memcpy(payload + 8, &size, 4);

    Ptr<Packet> packet = Create<Packet>(payload, 12);

    FabricHeader header;
    header.SetPacketType(FabricPacketType::MEMORY_READ);
    header.SetFabricType(GetFabricTypeForDest(destRank));
    header.SetFlowId(MEMORY_FLOW_ID);
    header.SetSequenceNumber(seqNum);
    header.SetVirtualChannel(0);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetPayloadSize(12);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));

    // Send-gate flow control: dispatches on m_flowControlPolicy. Under the
    // default CREDIT policy this is the existing per-VC credit check + consume;
    // WINDOW/RATE are seam-only and admit with a warning. Remote (So) dests
    // bypass the credit gate — the receiver returns credits only for local
    // sources (OnPacketDelivered's IsLocalRank guard), so a remote consume
    // would never come back and would stall the VC.
    uint8_t vcId = 0;
    if (!FlowControlGate(vcId, seqNum, !IsLocalRank(destRank)))
    {
        NS_LOG_DEBUG("FlowControlGate backpressure for MEMORY_READ on VC "
                     << static_cast<int>(vcId) << ", queuing packet");
        SendQueueEntry entry;
        entry.packet = packet;
        entry.header = header;
        entry.vcId = vcId;
        entry.seqNum = seqNum;
        m_sendQueue.push_back(entry);
        TrySendQueuedPackets();
        return;
    }

    packet->AddHeader(header);

    if (m_devices.empty())
    {
        NS_LOG_ERROR("No NetDevices configured");
        return;
    }

    uint32_t deviceIndex = ResolveDeviceIndex(destRank);
    SendPacketOnDevice(packet, deviceIndex, ResolveDestMac(destRank));
}

void
FabricEndpoint::SendMemoryWrite(uint16_t destRank, uint64_t address,
                                 const uint8_t* data, uint32_t size)
{
    NS_LOG_FUNCTION(this << destRank << address << size);

    uint64_t seqKey = MakeMemorySeqKey(destRank);
    uint32_t seqNum = m_nextSeqNum[seqKey]++;

    // Pack address prefix + data into payload
    uint32_t payloadSize = 8 + size;
    std::vector<uint8_t> payload(payloadSize);
    std::memcpy(payload.data(), &address, 8);
    std::memcpy(payload.data() + 8, data, size);

    Ptr<Packet> packet = Create<Packet>(payload.data(), payloadSize);

    FabricHeader header;
    header.SetPacketType(FabricPacketType::MEMORY_WRITE);
    header.SetFabricType(GetFabricTypeForDest(destRank));
    header.SetFlowId(MEMORY_FLOW_ID);
    header.SetSequenceNumber(seqNum);
    header.SetVirtualChannel(0);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetPayloadSize(payloadSize);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));

    // Admission through the virtual FlowControlGate (the documented
    // send-gate seam). Su (local-dest) traffic only — the receiver returns
    // credits solely for local sources (OnPacketDelivered's IsLocalRank
    // guard), so a remote memory write would drain the VC pool with no
    // return path. The gate performs the HasCredits/ConsumeCredit pair for
    // the CREDIT policy, so a subclass override is no longer bypassed on
    // this path (codex round 29, finding 3).
    uint8_t vcId = 0;
    if (!FlowControlGate(vcId, seqNum, !IsLocalRank(destRank)))
    {
        NS_LOG_DEBUG("No credits for MEMORY_WRITE on VC " << static_cast<int>(vcId)
                     << ", queuing packet");
        SendQueueEntry entry;
        entry.packet = packet;
        entry.header = header;
        entry.vcId = vcId;
        entry.seqNum = seqNum;
        m_sendQueue.push_back(entry);
        TrySendQueuedPackets();
        return;
    }

    packet->AddHeader(header);

    if (m_devices.empty())
    {
        NS_LOG_ERROR("No NetDevices configured");
        return;
    }

    uint32_t deviceIndex = ResolveDeviceIndex(destRank);
    SendPacketOnDevice(packet, deviceIndex, ResolveDestMac(destRank));
}

void
FabricEndpoint::SendMemoryResponse(uint16_t destRank, const uint8_t* data, uint32_t size)
{
    NS_LOG_FUNCTION(this << destRank << size);

    // Sequence responses from this endpoint's per-dest memory counter (the
    // isolated MakeMemorySeqKey namespace shared with SendMemoryRead/Write —
    // disjoint from DATA's counters, whose reorder tracking never sees
    // memory seqs). A hard-coded 0 gave every MEMORY_RESP the same LLR
    // retry key: with two reads in flight the second response's store entry
    // overwrote the first's packet copy, and its ACK purged the shared
    // entry — a lost first response was never retransmitted with its own
    // payload (its data vanished silently). Distinct sequence numbers also
    // let the receiver's CREDIT advance the GBN cumulative-ack window past 1.
    uint64_t seqKey = MakeMemorySeqKey(destRank);
    uint32_t seqNum = m_nextSeqNum[seqKey]++;

    Ptr<Packet> packet = Create<Packet>(data, size);

    FabricHeader header;
    header.SetPacketType(FabricPacketType::MEMORY_RESP);
    header.SetFabricType(GetFabricTypeForDest(destRank));
    header.SetFlowId(MEMORY_FLOW_ID);
    header.SetSequenceNumber(seqNum);
    header.SetVirtualChannel(0);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetPayloadSize(size);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));

    // Admission through the virtual FlowControlGate (see SendMemoryWrite;
    // Su/So split and gate rationale, codex round 29 finding 3).
    uint8_t vcId = 0;
    if (!FlowControlGate(vcId, seqNum, !IsLocalRank(destRank)))
    {
        NS_LOG_DEBUG("No credits for MEMORY_RESP on VC " << static_cast<int>(vcId)
                     << ", queuing packet");
        SendQueueEntry entry;
        entry.packet = packet;
        entry.header = header;
        entry.vcId = vcId;
        entry.seqNum = seqNum;
        m_sendQueue.push_back(entry);
        TrySendQueuedPackets();
        return;
    }

    packet->AddHeader(header);

    if (m_devices.empty())
    {
        NS_LOG_ERROR("No NetDevices configured");
        return;
    }

    uint32_t deviceIndex = ResolveDeviceIndex(destRank);
    SendPacketOnDevice(packet, deviceIndex, ResolveDestMac(destRank));
}

void
FabricEndpoint::SendMemorySemantic(uint16_t destRank, uint64_t address,
                                    const uint8_t* data, uint32_t size,
                                    MemoryAccessType accessType)
{
    NS_LOG_FUNCTION(this << destRank << address << size << static_cast<int>(accessType));

    if (accessType == MemoryAccessType::DMA_BULK)
    {
        // Backward compat: use existing SendMemoryWrite for bulk DMA
        if (data != nullptr)
        {
            SendMemoryWrite(destRank, address, data, size);
        }
        else
        {
            SendMemoryRead(destRank, address, size);
        }
        return;
    }

    // For SYNC_LOAD_STORE and ASYNC_URMA: schedule completion at latency
    uint64_t latencyNs = (accessType == MemoryAccessType::SYNC_LOAD_STORE)
                             ? m_syncMemLatencyNs
                             : m_asyncMemLatencyNs;

    // Send the memory request packet with the access type in the header.
    // data == nullptr is a READ (the API documents it so): build a
    // MEMORY_READ carrying address+size, not a MEMORY_WRITE padded with
    // zeros — the receiving MEMORY endpoint applies MEMORY_WRITE payloads
    // verbatim, so a zero-padded "read" would overwrite the target with 0s.
    const bool isRead = (data == nullptr);
    uint64_t seqKey = MakeMemorySeqKey(destRank);
    uint32_t seqNum = m_nextSeqNum[seqKey]++;

    uint32_t payloadSize = isRead ? 12 : 8 + size; // read: address + size
    std::vector<uint8_t> payload(payloadSize, 0);
    std::memcpy(payload.data(), &address, 8);
    if (isRead)
    {
        std::memcpy(payload.data() + 8, &size, 4);
    }
    else if (size > 0)
    {
        std::memcpy(payload.data() + 8, data, size);
    }

    Ptr<Packet> packet = Create<Packet>(payload.data(), payloadSize);

    FabricHeader header;
    header.SetPacketType(isRead ? FabricPacketType::MEMORY_READ
                                 : FabricPacketType::MEMORY_WRITE);
    header.SetFabricType(GetFabricTypeForDest(destRank));
    header.SetFlowId(MEMORY_FLOW_ID);
    header.SetSequenceNumber(seqNum);
    header.SetVirtualChannel(0);
    header.SetVirtualLane(0);
    header.SetMemoryAccessType(accessType);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetPayloadSize(payloadSize);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));

    // Admission through the virtual FlowControlGate (see SendMemoryWrite;
    // Su/So split and gate rationale, codex round 29 finding 3).
    uint8_t vcId = 0;
    if (!FlowControlGate(vcId, seqNum, !IsLocalRank(destRank)))
    {
        NS_LOG_DEBUG("No credits for MEMORY_SEMANTIC on VC " << static_cast<int>(vcId)
                     << ", queuing packet");
        SendQueueEntry entry;
        entry.packet = packet;
        entry.header = header;
        entry.vcId = vcId;
        entry.seqNum = seqNum;
        m_sendQueue.push_back(entry);
        TrySendQueuedPackets();
        // Still schedule completion — the packet will be sent when credits arrive
        // Tracked in m_deferredEgressEvents so DoDispose cancels it: the
        // bound member call targets raw `this` and fires into freed
        // memory when the endpoint is disposed and released before the
        // completion latency elapses (codex round 17, finding 2 — with a
        // 100-us sync latency and dispose+release at 1 us, valgrind
        // confirmed invalid reads in NotifyMemorySemanticComplete's
        // callback member access at the 100-us firing). Pruned before each
        // append — a gate-denied memory request never reaches
        // SendPacketOnDevice's entry-side prune, so without this the ledger
        // retained one expired EventId per completed request forever
        // (codex round 30, finding 3).
        PruneDeferredEgressEvents();
        m_deferredEgressEvents.push_back(
            Simulator::Schedule(NanoSeconds(latencyNs),
                                &FabricEndpoint::NotifyMemorySemanticComplete,
                                this, destRank, address, accessType));
        return;
    }

    packet->AddHeader(header);

    if (m_devices.empty())
    {
        NS_LOG_ERROR("No NetDevices configured");
        return;
    }

    uint32_t deviceIndex = ResolveDeviceIndex(destRank);
    SendPacketOnDevice(packet, deviceIndex, ResolveDestMac(destRank));

    // Schedule completion callback at the memory-semantic latency.
    // Tracked in m_deferredEgressEvents so DoDispose cancels it (same
    // rationale as the credit-blocked path above — codex round 17,
    // finding 2). Pruned before each append (codex round 30, finding 3).
    PruneDeferredEgressEvents();
    m_deferredEgressEvents.push_back(
        Simulator::Schedule(NanoSeconds(latencyNs),
                            &FabricEndpoint::NotifyMemorySemanticComplete,
                            this, destRank, address, accessType));
}

void
FabricEndpoint::SendDataInternal(uint16_t destRank, const uint8_t* data, uint32_t size,
                                  uint8_t protocolId, uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << destRank << size << static_cast<int>(protocolId) << flowId << vcId);

    // Check if this protocol has no overhead (SIMPLE, NONE, or vendor
    // single-protocol). Union gate, same semantics as the bulk path's
    // stampSoHeader (codex round 37, finding 1): efficiency alone is the
    // wrong discriminator for the So fabric — the RoCE model's WireEfficiency
    // is a header-accounting knob that is legitimately configured to 1.0
    // (the ns3-rdma alignment setting), and an efficiency-only gate sent
    // those non-bulk RoCE packets raw (no RocePayloadBuilder framing, no
    // SoHeader): the switch ECN-mark guard (protocol==SIMPLE + fabric ROCE
    // + SoHeader present) never fired, so congestion backoff stayed inert
    // at exactly the aligned configuration. Route through the builder when
    // the destination's model is RoCE OR the protocol has real framing
    // overhead (Su NCCL LL/LL128); plain SIMPLE/NONE/Ethernet traffic
    // (efficiency == 1.0, non-RoCE model) still takes the raw path.
    Ptr<ProtocolModel> destModel = GetProtocolModelForDest(destRank);
    double efficiency = destModel->GetEfficiency(protocolId);
    const bool destIsRoceModel = (destModel->GetInstanceTypeId()
                                  == RoceProtocolModel::GetTypeId());
    // NB: the builder check binds only the RoCE arm. The efficiency arm must
    // NOT require RocePayloadBuilder — Su NCCL LL/LL128 traffic frames with
    // NcclProtocolPayloadBuilder, and gating it on the RoCE builder broke
    // EgressDisposeCancelsDeferredSendsTest (delivered 3 instead of 1: no
    // per-flow pacing without the builder's chunked path).
    const bool useBuilder = (efficiency < 1.0)
        || (destIsRoceModel
            && GetPayloadBuilderForRank(destRank)->GetInstanceTypeId()
                == RocePayloadBuilder::GetTypeId());
    if (!useBuilder)
    {
        // No protocol overhead (or no RoCE builder mounted) - use regular
        // SendData. NB: a RoCE dest without --dcqcn never mounts the So
        // builder, so it takes this raw path exactly as before — the
        // builder detour only lights up the ECN/CNP chain when it is armed.
        SendData(destRank, data, size, flowId, vcId);
        return;
    }

    // For protocols with overhead (LL, LL128, etc.), use payload builder
    uint64_t chunkSize = GetProtocolModelForDest(destRank)->GetChunkSize(protocolId);

    NS_LOG_INFO("Sending " << size << " bytes with protocol "
               << static_cast<int>(protocolId) << " efficiency=" << efficiency
               << " chunk=" << chunkSize);

    // Split data into chunks and send each with protocol overhead
    uint32_t offset = 0;
    uint32_t remaining = size;
    uint32_t chunkIndex = 0;

    while (remaining > 0)
    {
        uint32_t thisChunkDataSize = std::min(static_cast<uint32_t>(chunkSize), remaining);
        const uint8_t* chunkData = data + offset;

        // Build protocol-aware chunks using the DESTINATION's payload
        // builder (GetPayloadBuilderForRank mirrors GetProtocolModelForDest's
        // Su/So split): a remote (So) dest frames with the So builder — the
        // receiver extracts with GetPayloadBuilderForRank(sourceRank), so an
        // Su-framed remote packet would be parsed as the wrong wire format
        // and deliver nothing.
        std::vector<Ptr<Packet>> chunks = GetPayloadBuilderForRank(destRank)->BuildChunks(
            chunkData, thisChunkDataSize, protocolId, chunkSize);

        // For protocols with overhead (LL/LL128), each sub-packet carries a
        // fixed amount of data (64B per LL line, 120B per LL128 chunk).
        // Track how much of thisChunkDataSize has been assigned so far.
        uint32_t dataAssigned = 0;

        for (auto& pkt : chunks)
        {
            uint32_t wireSize = pkt->GetSize();

            // Compute actual data bytes carried by this sub-packet.
            // For overhead protocols, use payload builder to extract the true
            // data size. The extraction buffer is sized to the sub-packet, not
            // a fixed 256 B: any vendor builder with larger units (e.g. 512 KiB
            // RoCE segments) would advertise only 256 B per segment and the
            // receiver would drop the rest (4096 B send delivered 256 B). The
            // result is capped by the data still unassigned in this chunk: a
            // fixed-width wire unit (LL line / LL128 chunk) that pads a short
            // tail otherwise advertises the full unit (64 B for a 50 B tail)
            // and over-counts the flow.
            uint32_t subPacketDataSize;
            if (efficiency < 1.0)
            {
                uint32_t dataStillNeeded = thisChunkDataSize - dataAssigned;
                std::vector<uint8_t> tmpBuf(pkt->GetSize());
                uint64_t extracted = GetPayloadBuilderForRank(destRank)->ExtractData(
                    pkt, protocolId, tmpBuf.data(), tmpBuf.size());
                subPacketDataSize = static_cast<uint32_t>(
                    std::min<uint64_t>(extracted, dataStillNeeded));
            }
            else
            {
                uint32_t dataStillNeeded = thisChunkDataSize - dataAssigned;
                uint64_t protoChunkSz = GetProtocolModelForDest(destRank)->GetChunkSize(protocolId);
                subPacketDataSize = std::min(static_cast<uint32_t>(protoChunkSz), dataStillNeeded);
            }
            dataAssigned += subPacketDataSize;

            uint64_t seqKey = MakeSeqKey(destRank, vcId, flowId);
            uint32_t seqNum = m_nextSeqNum[seqKey]++;

            FabricHeader header;
            header.SetPacketType(FabricPacketType::DATA);
            header.SetFabricType(GetFabricTypeForDest(destRank));
            header.SetFlowId(flowId);
            header.SetSequenceNumber(seqNum);
            header.SetVirtualChannel(vcId);
            header.SetSourceRank(m_rank);
            header.SetDestRank(destRank);
            header.SetPayloadSize(wireSize);
            header.SetSourceMac(m_address);
            header.SetDestMac(ResolveDestMac(destRank));
            header.SetProtocol(protocolId);
            header.SetEffectiveDataSize(subPacketDataSize);
            header.SetTtl(64);
            // These chunks came from the payload builder (BuildChunks framed
            // a vendor header inside the body) — stamp the wire flag the
            // receiver's ProcessDataPacket dispatch reads.
            header.SetProtocolFramed(true);

            // Admission through the virtual FlowControlGate (the documented
            // send-gate seam) — Su (local-dest) traffic only (see SendData
            // for the So bypass rationale; the receiver's return is
            // IsLocalRank-guarded, so a remote consume would never come
            // back). The gate performs the HasCredits/ConsumeCredit pair
            // for the CREDIT policy, so a subclass override is no longer
            // bypassed on this path (codex round 29, finding 3).
            if (!FlowControlGate(vcId, seqNum, !IsLocalRank(destRank)))
            {
                NS_LOG_DEBUG("No credits for DATA_INTERNAL chunk on VC " << static_cast<int>(vcId)
                             << ", queuing packet");
                SendQueueEntry entry;
                entry.packet = pkt;
                entry.header = header;
                entry.vcId = vcId;
                entry.seqNum = seqNum;
                m_sendQueue.push_back(entry);
                TrySendQueuedPackets();
                // Skip device selection for queued packets — TrySendQueuedPackets handles it
                chunkIndex++;
                continue;
            }

            pkt->AddHeader(header);

            std::vector<uint32_t> devices = GetPhysicalDevicesForDest(destRank);
            uint32_t deviceIdx;
            if (devices.size() > 1)
            {
                deviceIdx = devices[(m_globalSprayOffset + chunkIndex) % devices.size()];
            }
            else if (!devices.empty())
            {
                deviceIdx = devices[0];
            }
            else
            {
                deviceIdx = ResolveDeviceIndex(destRank);
            }

            Mac48Address destMac = ResolveDestMac(destRank);
            if (!SendPacketOnDevice(pkt, deviceIdx, destMac))
            {
                // PFC-held: parked on the send queue by the chokepoint; do
                // not count a transmission (the resume drains it). KEEP
                // BUILDING the remaining sub-packets — each parks itself
                // the same way — instead of dropping them (codex round 27,
                // finding 1; same shape as SendBulkWireTransferSize's loop).
                // The device-spray counter still advances so the parked
                // sub-packets keep their per-lane assignment.
                chunkIndex++;
                continue;
            }

            m_txTrace(pkt->GetSize(), seqNum);
            m_txPackets++;
            m_txBytes += pkt->GetSize();

            chunkIndex++;
        }

        offset += thisChunkDataSize;
        remaining -= thisChunkDataSize;
    }

    {
        std::vector<uint32_t> devices = GetPhysicalDevicesForDest(destRank);
        if (devices.size() > 1)
        {
            m_globalSprayOffset = (m_globalSprayOffset + chunkIndex) % devices.size();
        }
    }
}

} // namespace ns3
