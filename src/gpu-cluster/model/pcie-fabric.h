/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PCIE_FABRIC_H
#define PCIE_FABRIC_H

#include "ns3/header.h"
#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/traced-callback.h"

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace ns3
{

enum class PcieTlpType : uint8_t
{
    READ = 0x20,      ///< 4-DW Memory Read
    WRITE = 0x60,     ///< 4-DW Memory Write
    COMPLETION = 0x4a ///< 3-DW Completion with Data
};

enum class PcieTraffic : uint8_t
{
    PAYLOAD,
    WQE,
    DOORBELL,
    CQ,
    D2H,
    H2D,
    COUNT
};
const char* PcieTrafficName(PcieTraffic traffic);
const char* PcieTlpName(PcieTlpType type);

/** Gen3--5, non-FLIT, VC0 performance model. Credits count headers / 16-B data units.
 * These defaults describe an example link, not a calibrated GPU or NIC.
 */
struct PcieConfig
{
    uint32_t generation{4}, lanes{16};
    std::string topology{"switch"}; ///< switch: P2P below shared uplink; root: via RC
    uint32_t mps{256}, mrrs{512}, rcb{128}, tags{32};
    uint32_t maxReadBytes{16384}, completionBufferBytes{32768}, queuePackets{64};
    // PH, PD, NPH, NPD, CplH, CplD. Data credit units are 16 bytes.
    std::array<uint32_t, 6> credits{{32, 512, 32, 32, 32, 512}};
    uint32_t replayBufferBytes{65536}, ackEvery{8};
    Time linkLatency{NanoSeconds(10)}, switchLatency{NanoSeconds(20)};
    Time rootLatency{NanoSeconds(50)};
    Time creditUpdate{NanoSeconds(50)}, ackDelay{NanoSeconds(50)};
    bool relaxedOrdering{false};
    void Validate() const;
    bool operator==(const PcieConfig& other) const;
};

/** The supported subset of the PCIe TLP header, including byte enables.
 * Payload padding, link framing, sequence and LCRC overhead are charged separately.
 */
class PcieTlpHeader : public Header
{
  public:
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    void Print(std::ostream& os) const override;
    uint32_t PayloadBytesOnWire() const;

    PcieTlpType type{PcieTlpType::READ};
    uint64_t address{0};
    uint32_t bytes{0};
    uint16_t requester{0}, completer{0}, byteCount{0};
    uint8_t tag{0};
    bool relaxed{false};
};

struct PcieStats
{
    uint64_t reads{0}, writes{0}, readRequests{0}, writeTlps{0}, completionTlps{0};
    uint64_t tlpHops{0}, wireBytes{0}, flowUpdates{0}, linkAcks{0};
    uint64_t creditStalls{0}, queueStalls{0}, tagStalls{0}, orderingStalls{0}, replayStalls{0};
    uint64_t peakOutstandingReads{0}, peakOutstandingReadBytes{0};
    uint64_t peakQueuePackets{0}, peakReplayBytes{0}, lastCompletionNs{0};
    std::array<uint64_t, static_cast<size_t>(PcieTraffic::COUNT)> trafficBytes{};
};

struct PcieTraceEvent
{
    Time time;
    std::string event;
    uint16_t from{0}, to{0};
    uint64_t transaction{0}, address{0};
    PcieTlpType type{PcieTlpType::READ};
    PcieTraffic traffic{PcieTraffic::PAYLOAD};
    uint32_t bytes{0}, wireBytes{0};
    uint8_t tag{0};
    std::array<uint32_t, 6> credits{};
};

/** Packet execution on a PCIe fabric, with address-routed requests and ID-routed
 * completions. Read/Write callbacks indicate delivery to endpoint memory. Link
 * ACKs and credit updates are separate packets/events. No errors or drops occur.
 * Memory values remain owned by the caller (e.g. RdmaOperationEngine).
 */
class PcieFabric : public Object
{
  public:
    using Ready = std::function<void()>;
    using MemoryService = std::function<Time(uint32_t)>;
    using Observer = std::function<void(const PcieTraceEvent&)>;
    static TypeId GetTypeId();
    PcieFabric();
    ~PcieFabric() override;
    void Configure(const PcieConfig& config);
    const PcieConfig& GetConfig() const;
    uint16_t AddEndpoint(const std::string& name,
                         MemoryService read = {},
                         MemoryService write = {});
    uint16_t AddRootComplex(const std::string& name,
                            MemoryService read = {},
                            MemoryService write = {});
    uint16_t AddSwitch(const std::string& name);
    void AddLink(uint16_t a, uint16_t b);
    /// Each endpoint owns a 1-TiB address window, assigned during construction.
    uint64_t GetAddress(uint16_t endpoint, uint64_t offset = 0) const;
    uint64_t Read(uint16_t requester,
                  uint16_t target,
                  uint64_t bytes,
                  Ready ready,
                  PcieTraffic traffic = PcieTraffic::PAYLOAD,
                  uint64_t offset = 0,
                  bool relaxed = false);
    uint64_t Write(uint16_t requester,
                   uint16_t target,
                   uint64_t bytes,
                   Ready ready,
                   PcieTraffic traffic = PcieTraffic::PAYLOAD,
                   uint64_t offset = 0,
                   bool relaxed = false);
    const PcieStats& GetStats() const;
    void SetObserver(Observer observer);
    bool HasPending() const;

  protected:
    void DoDispose() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
    TracedCallback<Ptr<const Packet>, uint16_t, uint16_t> m_tlpTx, m_tlpRx;
};
} // namespace ns3
#endif
