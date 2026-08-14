/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Fabric Endpoint Send Path Implementation
 */

#include "fabric-endpoint.h"

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
        uint32_t chunkSize = (size + numChunks - 1) / numChunks;

        NS_LOG_INFO("Fragmenting " << size << " bytes to dest " << destRank
                    << " across " << sendDevices.size() << " links in " << numChunks << " chunks"
                    << " starting at offset " << m_globalSprayOffset);

        for (uint32_t i = 0; i < numChunks; i++)
        {
            uint32_t offset = i * chunkSize;
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

            // Check and consume credit before sending (credit-based flow control)
            if (m_creditManager && !m_creditManager->HasCredits(vcId))
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
                m_sendQueue.push(entry);

                // Queue remaining chunks
                for (uint32_t j = i + 1; j < numChunks; j++)
                {
                    uint32_t remOffset = j * chunkSize;
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

            // Select device using round-robin with global offset
            uint32_t deviceIdx = sendDevices[(m_globalSprayOffset + i) % sendDevices.size()];
            Mac48Address destMac = ResolveDestMac(destRank);
            SendPacketOnDevice(chunkPacket, deviceIdx, destMac);

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

        m_sendQueue.push(entry);
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
                                const uint8_t* data, uint32_t size)
{
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
            uint32_t thisChunkSize = static_cast<uint32_t>(
                std::min(chunkSize, wireSize - offset));
            uint64_t effectiveBegin = i * effectiveSize / numChunks;
            uint64_t effectiveEnd = (i + 1) * effectiveSize / numChunks;
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

            // Check and consume credit before sending (credit-based flow control)
            // NVLS/SHARP packets bypass credits — switch SHARP engine has own buffer
            uint8_t vcId = 0;
            if (!creditBypass && m_creditManager && !m_creditManager->HasCredits(vcId))
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
                m_sendQueue.push(entry);

                // Queue remaining chunks too
                for (uint64_t j = i + 1; j < numChunks; j++)
                {
                    uint64_t remOffset = j * chunkSize;
                    uint32_t remChunkSize = static_cast<uint32_t>(
                        std::min(chunkSize, wireSize - remOffset));
                    uint64_t remEffectiveBegin = j * effectiveSize / numChunks;
                    uint64_t remEffectiveEnd = (j + 1) * effectiveSize / numChunks;
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
                    m_sendQueue.push(remEntry);
                }
                // Try to send any queued packets that might have credits
                TrySendQueuedPackets();
                break;
            }
            if (!creditBypass && m_creditManager)
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
            // NVLS/SHARP: send directly, bypassing credit queue
            packet->AddHeader(header);
            uint32_t deviceIdx = m_devices.empty() ? 0 : (m_globalSprayOffset % m_devices.size());
            Mac48Address destMac = ResolveDestMac(destRank);
            SendPacketOnDevice(packet, deviceIdx, destMac);
            if (!m_devices.empty())
            {
                m_globalSprayOffset = (m_globalSprayOffset + 1) % m_devices.size();
            }
            m_txPackets++;
            m_txBytes += packetSize;
        }
        else
        {
            SendQueueEntry entry;
            entry.packet = packet;
            entry.header = header;
            entry.vcId = 0;
            entry.seqNum = seqNum;

            m_sendQueue.push(entry);
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

    SendQueueEntry entry;
    entry.packet = packet;
    entry.header = header;
    entry.vcId = vcId;
    entry.seqNum = header.GetSequenceNumber();

    m_sendQueue.push(entry);
    TrySendQueuedPackets();
}

void
FabricEndpoint::SendMemoryRead(uint16_t destRank, uint64_t address, uint32_t size)
{
    NS_LOG_FUNCTION(this << destRank << address << size);

    uint64_t seqKey = MakeSeqKey(destRank, 0, 0);
    uint32_t seqNum = m_nextSeqNum[seqKey]++;

    // Pack address and size into payload for memory read request
    uint8_t payload[12];
    std::memcpy(payload, &address, 8);
    std::memcpy(payload + 8, &size, 4);

    Ptr<Packet> packet = Create<Packet>(payload, 12);

    FabricHeader header;
    header.SetPacketType(FabricPacketType::MEMORY_READ);
    header.SetFabricType(GetFabricTypeForDest(destRank));
    header.SetFlowId(0);
    header.SetSequenceNumber(seqNum);
    header.SetVirtualChannel(0);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetPayloadSize(12);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));

    // Send-gate flow control: dispatches on m_flowControlPolicy. Under the
    // default CREDIT policy this is the existing per-VC credit check + consume;
    // WINDOW/RATE are seam-only and admit with a warning.
    uint8_t vcId = 0;
    if (!FlowControlGate(vcId, seqNum, false))
    {
        NS_LOG_DEBUG("FlowControlGate backpressure for MEMORY_READ on VC "
                     << static_cast<int>(vcId) << ", queuing packet");
        SendQueueEntry entry;
        entry.packet = packet;
        entry.header = header;
        entry.vcId = vcId;
        entry.seqNum = seqNum;
        m_sendQueue.push(entry);
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

    uint64_t seqKey = MakeSeqKey(destRank, 0, 0);
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
    header.SetFlowId(0);
    header.SetSequenceNumber(seqNum);
    header.SetVirtualChannel(0);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetPayloadSize(payloadSize);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));

    // Credit-based flow control: check and consume credits before sending
    uint8_t vcId = 0;
    if (m_creditManager)
    {
        if (!m_creditManager->HasCredits(vcId))
        {
            NS_LOG_DEBUG("No credits for MEMORY_WRITE on VC " << static_cast<int>(vcId)
                         << ", queuing packet");
            SendQueueEntry entry;
            entry.packet = packet;
            entry.header = header;
            entry.vcId = vcId;
            entry.seqNum = seqNum;
            m_sendQueue.push(entry);
            TrySendQueuedPackets();
            return;
        }
        m_creditManager->ConsumeCredit(vcId, seqNum);
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

    Ptr<Packet> packet = Create<Packet>(data, size);

    FabricHeader header;
    header.SetPacketType(FabricPacketType::MEMORY_RESP);
    header.SetFabricType(GetFabricTypeForDest(destRank));
    header.SetFlowId(0);
    header.SetSequenceNumber(0);
    header.SetVirtualChannel(0);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetPayloadSize(size);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));

    // Credit-based flow control for MEMORY_RESP
    uint8_t vcId = 0;
    if (m_creditManager)
    {
        if (!m_creditManager->HasCredits(vcId))
        {
            NS_LOG_DEBUG("No credits for MEMORY_RESP on VC " << static_cast<int>(vcId)
                         << ", queuing packet");
            SendQueueEntry entry;
            entry.packet = packet;
            entry.header = header;
            entry.vcId = vcId;
            entry.seqNum = 0;
            m_sendQueue.push(entry);
            TrySendQueuedPackets();
            return;
        }
        m_creditManager->ConsumeCredit(vcId, 0);
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

    // Send the memory request packet with the access type in the header
    uint64_t seqKey = MakeSeqKey(destRank, 0, 0);
    uint32_t seqNum = m_nextSeqNum[seqKey]++;

    uint32_t payloadSize = 8 + size; // address + data
    std::vector<uint8_t> payload(payloadSize, 0);
    std::memcpy(payload.data(), &address, 8);
    if (data != nullptr && size > 0)
    {
        std::memcpy(payload.data() + 8, data, size);
    }

    Ptr<Packet> packet = Create<Packet>(payload.data(), payloadSize);

    FabricHeader header;
    header.SetPacketType(FabricPacketType::MEMORY_WRITE);
    header.SetFabricType(GetFabricTypeForDest(destRank));
    header.SetFlowId(0);
    header.SetSequenceNumber(seqNum);
    header.SetVirtualChannel(0);
    header.SetVirtualLane(0);
    header.SetMemoryAccessType(accessType);
    header.SetSourceRank(m_rank);
    header.SetDestRank(destRank);
    header.SetPayloadSize(payloadSize);
    header.SetSourceMac(m_address);
    header.SetDestMac(ResolveDestMac(destRank));

    // Credit-based flow control for memory semantic packets
    uint8_t vcId = 0;
    if (m_creditManager)
    {
        if (!m_creditManager->HasCredits(vcId))
        {
            NS_LOG_DEBUG("No credits for MEMORY_SEMANTIC on VC " << static_cast<int>(vcId)
                         << ", queuing packet");
            SendQueueEntry entry;
            entry.packet = packet;
            entry.header = header;
            entry.vcId = vcId;
            entry.seqNum = seqNum;
            m_sendQueue.push(entry);
            TrySendQueuedPackets();
            // Still schedule completion — the packet will be sent when credits arrive
            Simulator::Schedule(NanoSeconds(latencyNs),
                                &FabricEndpoint::NotifyMemorySemanticComplete,
                                this, destRank, address, accessType);
            return;
        }
        m_creditManager->ConsumeCredit(vcId, seqNum);
    }

    packet->AddHeader(header);

    if (m_devices.empty())
    {
        NS_LOG_ERROR("No NetDevices configured");
        return;
    }

    uint32_t deviceIndex = ResolveDeviceIndex(destRank);
    SendPacketOnDevice(packet, deviceIndex, ResolveDestMac(destRank));

    // Schedule completion callback at the memory-semantic latency
    Simulator::Schedule(NanoSeconds(latencyNs),
                        &FabricEndpoint::NotifyMemorySemanticComplete,
                        this, destRank, address, accessType);
}

void
FabricEndpoint::SendDataInternal(uint16_t destRank, const uint8_t* data, uint32_t size,
                                  uint8_t protocolId, uint16_t flowId, uint8_t vcId)
{
    NS_LOG_FUNCTION(this << destRank << size << static_cast<int>(protocolId) << flowId << vcId);

    // Check if this protocol has no overhead (SIMPLE, NONE, or vendor single-protocol)
    double efficiency = m_protocolModel->GetEfficiency(protocolId);
    if (efficiency >= 1.0)
    {
        // No protocol overhead - use regular SendData
        SendData(destRank, data, size, flowId, vcId);
        return;
    }

    // For protocols with overhead (LL, LL128, etc.), use payload builder
    uint64_t chunkSize = m_protocolModel->GetChunkSize(protocolId);

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

        // Build protocol-aware chunks using vendor-specific payload builder
        std::vector<Ptr<Packet>> chunks = m_payloadBuilder->BuildChunks(
            chunkData, thisChunkDataSize, protocolId, chunkSize);

        // For protocols with overhead (LL/LL128), each sub-packet carries a
        // fixed amount of data (64B per LL line, 120B per LL128 chunk).
        // Track how much of thisChunkDataSize has been assigned so far.
        uint32_t dataAssigned = 0;

        for (auto& pkt : chunks)
        {
            uint32_t wireSize = pkt->GetSize();

            // Compute actual data bytes carried by this sub-packet.
            // For overhead protocols, use payload builder to extract the
            // true data size. For SIMPLE (efficiency >= 1), each sub-packet
            // carries min(chunkSize, remaining) data.
            uint32_t subPacketDataSize;
            if (efficiency < 1.0)
            {
                // LL/LL128: extract actual data from the built sub-packet
                uint8_t tmpBuf[256];
                uint64_t extracted = m_payloadBuilder->ExtractData(
                    pkt, protocolId, tmpBuf, sizeof(tmpBuf));
                subPacketDataSize = static_cast<uint32_t>(extracted);
            }
            else
            {
                uint32_t dataStillNeeded = thisChunkDataSize - dataAssigned;
                uint64_t protoChunkSz = m_protocolModel->GetChunkSize(protocolId);
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

            // Credit-based flow control: check and consume credits before sending
            if (m_creditManager && !m_creditManager->HasCredits(vcId))
            {
                NS_LOG_DEBUG("No credits for DATA_INTERNAL chunk on VC " << static_cast<int>(vcId)
                             << ", queuing packet");
                SendQueueEntry entry;
                entry.packet = pkt;
                entry.header = header;
                entry.vcId = vcId;
                entry.seqNum = seqNum;
                m_sendQueue.push(entry);
                TrySendQueuedPackets();
                // Skip device selection for queued packets — TrySendQueuedPackets handles it
                chunkIndex++;
                continue;
            }
            if (m_creditManager)
            {
                m_creditManager->ConsumeCredit(vcId, seqNum);
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
            SendPacketOnDevice(pkt, deviceIdx, destMac);

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
