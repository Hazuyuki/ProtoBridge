/* SPDX-License-Identifier: GPL-2.0-only */
#include "rdma-submission-model.h"

#include "ns3/abort.h"
#include "ns3/simulator.h"

#include <algorithm>

namespace ns3
{
namespace
{
Time
AddTime(Time a, Time b)
{
    return b > Time::Max() - a ? Time::Max() : a + b;
}
} // namespace

RdmaSubmissionModel::Ready
RdmaSubmissionModel::Guard(Ready ready)
{
    return [alive = m_alive, ready = std::move(ready)] {
        if (*alive && ready)
        {
            ready();
        }
    };
}

void
RdmaSubmissionModel::EnsurePcie()
{
    if (m_pcie || m_config.pcieModel != "tlp")
    {
        return;
    }
    NS_ABORT_MSG_IF(m_disposed, "Cannot attach PCIe to a disposed runtime");
    if (!m_hostMemory->pcie)
    {
        auto fabric = CreateObject<PcieFabric>();
        fabric->Configure(m_config.pcie);
        auto memory = m_hostMemory->pcieMemory;
        auto read = [memory, rate = m_config.hostReadGbps](uint32_t bytes) {
            return Reserve(bytes, rate, memory->readFree, Simulator::Now()) - Simulator::Now();
        };
        auto write = [memory, rate = m_config.hostWriteGbps](uint32_t bytes) {
            return Reserve(bytes, rate, memory->writeFree, Simulator::Now()) - Simulator::Now();
        };
        m_hostMemory->pcieHost = fabric->AddRootComplex("root-complex/host-dram", read, write);
        m_hostMemory->pcieSwitch = m_hostMemory->pcieHost;
        if (m_config.pcie.topology == "switch")
        {
            m_hostMemory->pcieSwitch = fabric->AddSwitch("pcie-switch");
            fabric->AddLink(m_hostMemory->pcieHost, m_hostMemory->pcieSwitch);
        }
        m_hostMemory->pcie = fabric;
        m_hostMemory->pcieHostReadGbps = m_config.hostReadGbps;
        m_hostMemory->pcieHostWriteGbps = m_config.hostWriteGbps;
    }
    m_pcie = m_hostMemory->pcie;
    NS_ABORT_MSG_IF(!(m_pcie->GetConfig() == m_config.pcie) ||
                        m_hostMemory->pcieHostReadGbps != m_config.hostReadGbps ||
                        m_hostMemory->pcieHostWriteGbps != m_config.hostWriteGbps,
                    "RDMA runtimes sharing a host must use the same PCIe configuration");
    auto memory = m_pcieGpuMemory;
    auto read = [memory, rate = m_config.hbmReadGbps](uint32_t bytes) {
        return Reserve(bytes, rate, memory->readFree, Simulator::Now()) - Simulator::Now();
    };
    auto write = [memory, rate = m_config.hbmWriteGbps](uint32_t bytes) {
        return Reserve(bytes, rate, memory->writeFree, Simulator::Now()) - Simulator::Now();
    };
    m_pcieHost = m_hostMemory->pcieHost;
    m_pcieGpu = m_pcie->AddEndpoint("gpu-hbm", read, write);
    m_pcieNic = m_pcie->AddEndpoint("nic");
    m_pcie->AddLink(m_hostMemory->pcieSwitch, m_pcieGpu);
    m_pcie->AddLink(m_hostMemory->pcieSwitch, m_pcieNic);
}

Ptr<PcieFabric>
RdmaSubmissionModel::GetPcieFabric()
{
    if (m_disposed)
    {
        return nullptr;
    }
    EnsurePcie();
    return m_pcie;
}

void
RdmaSubmissionModel::SourceRead(uint64_t bytes, Ready ready, uint64_t address)
{
    if (m_config.pcieModel != "tlp")
    {
        Schedule(ReserveSourceRead(bytes), std::move(ready));
        return;
    }
    EnsurePcie();
    bool gpu = m_config.dataPath == "gpudirect";
    if (gpu)
    {
        m_stats.hbmReadBytes += bytes;
    }
    else
    {
        m_stats.hostReadBytes += bytes;
    }
    m_pcie->Read(
        m_pcieNic,
        gpu ? m_pcieGpu : m_pcieHost,
        bytes,
        Guard([this, ready = std::move(ready)] { Schedule(m_config.dmaReadLatency, ready); }),
        PcieTraffic::PAYLOAD,
        address % 4096);
}

void
RdmaSubmissionModel::DestinationWrite(uint64_t bytes, Ready ready, uint64_t address)
{
    if (m_config.pcieModel != "tlp")
    {
        Schedule(ReserveDestinationWrite(bytes), std::move(ready));
        return;
    }
    EnsurePcie();
    bool gpu = m_config.dataPath == "gpudirect";
    if (gpu)
    {
        m_stats.hbmWriteBytes += bytes;
    }
    else
    {
        m_stats.hostWriteBytes += bytes;
    }
    m_pcie->Write(
        m_pcieNic,
        gpu ? m_pcieGpu : m_pcieHost,
        bytes,
        Guard([this, ready = std::move(ready)] { Schedule(m_config.dmaWriteLatency, ready); }),
        PcieTraffic::PAYLOAD,
        address % 4096);
}

uint64_t
RdmaSubmissionModel::TakeSourceDmaOffset(uint64_t workId, uint64_t bytes)
{
    auto& work = m_work.at(workId);
    NS_ABORT_MSG_IF(bytes > work.time.bytes - work.sourceDmaBytes,
                    "DMA exceeds its RDMA work request");
    uint64_t offset = work.sourceDmaBytes;
    work.sourceDmaBytes += bytes;
    return offset + (m_config.dataPath == "staged" ? 0 : work.time.offset);
}

void
RdmaSubmissionModel::PcieCopy(bool toGpu, uint64_t bytes, Ready ready)
{
    EnsurePcie();
    Time now = Simulator::Now();
    Time engine = toGpu ? Reserve(bytes, m_config.h2dGbps, m_h2dFree, now)
                        : Reserve(bytes, m_config.d2hGbps, m_d2hFree, now);
    Time latency = toGpu ? m_config.h2dLatency : m_config.d2hLatency;
    auto complete = Guard([this, engine, latency, ready = std::move(ready)] {
        Schedule(AddTime(std::max(engine, Simulator::Now()), latency) - Simulator::Now(), ready);
    });
    if (toGpu)
    {
        m_stats.hostReadBytes += bytes;
        m_stats.hbmWriteBytes += bytes;
        m_stats.destinationH2dBytes += bytes;
        m_pcie->Read(m_pcieGpu, m_pcieHost, bytes, std::move(complete), PcieTraffic::H2D);
    }
    else
    {
        m_stats.hbmReadBytes += bytes;
        m_stats.hostWriteBytes += bytes;
        m_stats.sourceD2hBytes += bytes;
        m_pcie->Write(m_pcieGpu, m_pcieHost, bytes, std::move(complete), PcieTraffic::D2H);
    }
}

void
RdmaSubmissionModel::SourceD2h(uint64_t bytes, Ready ready)
{
    if (m_config.pcieModel == "tlp")
    {
        PcieCopy(false, bytes, std::move(ready));
    }
    else
    {
        Schedule(ReserveSourceD2h(bytes), std::move(ready));
    }
}

void
RdmaSubmissionModel::DestinationH2d(uint64_t bytes, Ready ready)
{
    if (m_config.pcieModel == "tlp")
    {
        PcieCopy(true, bytes, std::move(ready));
    }
    else
    {
        Schedule(ReserveDestinationH2d(bytes), std::move(ready));
    }
}

void
RdmaSubmissionModel::WriteCompletion(Ready ready)
{
    Time poll = m_config.initiator == "cpu" ? m_config.cpuPoll : m_config.gpuPoll;
    if (m_config.pcieModel != "tlp")
    {
        Schedule(AddTime(m_config.cqWrite, poll), std::move(ready));
        return;
    }
    EnsurePcie();
    Schedule(m_config.cqWrite, [this, poll, ready = std::move(ready)] {
        if (m_config.initiator == "cpu")
        {
            m_stats.hostWriteBytes += m_config.cqeBytes;
        }
        else
        {
            m_stats.hbmWriteBytes += m_config.cqeBytes;
        }
        m_pcie->Write(m_pcieNic,
                      m_config.initiator == "cpu" ? m_pcieHost : m_pcieGpu,
                      m_config.cqeBytes,
                      Guard([this, poll, ready] { Schedule(poll, ready); }),
                      PcieTraffic::CQ);
    });
}

void
RdmaSubmissionModel::FetchPublished(const QpKey& key, const std::vector<uint64_t>& ids)
{
    for (auto id : ids)
    {
        m_work.at(id).time.doorbell = Simulator::Now();
        if (m_config.initiator == "cpu")
        {
            m_stats.hostReadBytes += m_config.wqeBytes;
        }
        else
        {
            m_stats.hbmReadBytes += m_config.wqeBytes;
        }
        m_pcie->Read(m_pcieNic,
                     m_config.initiator == "cpu" ? m_pcieHost : m_pcieGpu,
                     m_config.wqeBytes,
                     Guard([this, key, id] {
                         auto& qp = m_qps.at(key);
                         qp.fetchFree =
                             AddTime(std::max(qp.fetchFree, Simulator::Now()), m_config.wqeFetch);
                         Schedule(qp.fetchFree - Simulator::Now(), [this, key, id] {
                             m_pcieReady.insert(id);
                             PumpPcieFetch(key);
                         });
                     }),
                     PcieTraffic::WQE);
    }
}

void
RdmaSubmissionModel::PumpPcieFetch(const QpKey& key)
{
    auto& queue = m_pcieFetched[key];
    while (!queue.empty() && m_pcieReady.erase(queue.front()))
    {
        uint64_t id = queue.front();
        queue.pop_front();
        ExecuteWork(id);
        if (m_disposed)
        {
            return;
        }
    }
}
} // namespace ns3
