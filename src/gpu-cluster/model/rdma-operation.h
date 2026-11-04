/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RDMA_OPERATION_H
#define RDMA_OPERATION_H

#include "ns3/nstime.h"
#include "ns3/tag.h"

#include <cstdint>
#include <functional>
#include <string>

namespace ns3
{
enum class RdmaOpcode : uint8_t
{
    WRITE,
    READ,
    SEND,
    WRITE_IMM,
    SEND_IMM,
    FETCH_ADD,
    COMPARE_SWAP,
    PUT_SIGNAL,
    SIGNAL
};
enum class RdmaSignalOp : uint8_t
{
    SET,
    ADD
};
enum class RdmaComparison : uint8_t
{
    EQ,
    NE,
    GT,
    GE,
    LT,
    LE
};
std::string RdmaOpcodeName(RdmaOpcode opcode);
RdmaOpcode ParseRdmaOpcode(const std::string& name);
bool RdmaHasSourcePayload(RdmaOpcode opcode);
bool RdmaUsesReadAtomicCredit(RdmaOpcode opcode);
bool RdmaNeedsReceive(RdmaOpcode opcode);

/** Addresses are simulated registered-memory addresses, not process pointers. */
struct RdmaRequest
{
    RdmaOpcode opcode{RdmaOpcode::WRITE};
    uint16_t peer{0};
    uint64_t bytes{0};
    uint64_t localAddress{0}, remoteAddress{0};
    uint32_t localKey{1}, remoteKey{1};
    uint64_t operand{1}, compare{0};
    uint32_t immediate{0};
    uint64_t signalAddress{0}, signalValue{1};
    RdmaSignalOp signalOp{RdmaSignalOp::SET};
    uint16_t flowId{1};
    uint8_t vc{0};
    uint32_t producer{UINT32_MAX};
};

struct RdmaCompletion
{
    uint64_t requestId{0};
    RdmaOpcode opcode{RdmaOpcode::WRITE};
    bool success{true};
    uint64_t bytes{0}, value{0}; ///< Atomic operations return the old value.
};

struct RdmaReceiveCompletion
{
    uint64_t receiveId{0};
    uint64_t requestId{0}, offset{0};
    uint16_t peer{0};
    uint32_t qp{0};
    RdmaOpcode opcode{RdmaOpcode::SEND};
    uint64_t bytes{0};
    bool success{true}, hasImmediate{false};
    uint32_t immediate{0};
    Time observed;
};

struct RdmaOperationStats
{
    uint64_t readRequests{0}, readResponseBytes{0}, atomicExecutions{0};
    uint64_t rnrRetries{0}, rnrExhausted{0}, receiveCompletions{0}, signalUpdates{0};
    uint64_t responsePeak{0}, activeResponses{0}, postedReceives{0};
    uint64_t wireDataBytes{0}, wireControlBytes{0};
};

enum class RdmaPhase : uint8_t
{
    DATA,
    REQUEST,
    READ_RESPONSE,
    ATOMIC_RESPONSE,
    RNR,
    COMPLETE
};

/** Simulation metadata. Request bodies and immediate bytes have separate wire costs. */
class RdmaOperationTag : public Tag
{
  public:
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer buffer) const override;
    void Deserialize(TagBuffer buffer) override;
    void Print(std::ostream& os) const override;
    RdmaOpcode opcode{RdmaOpcode::WRITE};
    RdmaPhase phase{RdmaPhase::DATA};
    uint16_t owner{0};
    uint64_t workId{0}, requestId{0}, bytes{0}, offset{0}, fragmentOffset{0};
    uint64_t localAddress{0}, remoteAddress{0}, signalAddress{0}, signalValue{1};
    uint64_t operand{0}, compare{0};
    uint32_t localKey{1}, remoteKey{1}, immediate{0}, attempt{0};
    RdmaSignalOp signalOp{RdmaSignalOp::SET};
    bool zeroPayload{true}, success{true};
};
} // namespace ns3
#endif
