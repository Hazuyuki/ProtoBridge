/* SPDX-License-Identifier: GPL-2.0-only */
#include "rdma-operation.h"

#include "ns3/abort.h"

namespace ns3
{
NS_OBJECT_ENSURE_REGISTERED(RdmaOperationTag);

std::string
RdmaOpcodeName(RdmaOpcode opcode)
{
    switch (opcode)
    {
    case RdmaOpcode::WRITE:
        return "write";
    case RdmaOpcode::READ:
        return "read";
    case RdmaOpcode::SEND:
        return "send";
    case RdmaOpcode::WRITE_IMM:
        return "write_imm";
    case RdmaOpcode::SEND_IMM:
        return "send_imm";
    case RdmaOpcode::FETCH_ADD:
        return "fetch_add";
    case RdmaOpcode::COMPARE_SWAP:
        return "compare_swap";
    case RdmaOpcode::PUT_SIGNAL:
        return "put_signal";
    case RdmaOpcode::SIGNAL:
        return "signal";
    }
    NS_ABORT_MSG("Unknown RDMA opcode");
}

RdmaOpcode
ParseRdmaOpcode(const std::string& name)
{
    for (auto opcode : {RdmaOpcode::WRITE,
                        RdmaOpcode::READ,
                        RdmaOpcode::SEND,
                        RdmaOpcode::WRITE_IMM,
                        RdmaOpcode::SEND_IMM,
                        RdmaOpcode::FETCH_ADD,
                        RdmaOpcode::COMPARE_SWAP,
                        RdmaOpcode::PUT_SIGNAL,
                        RdmaOpcode::SIGNAL})
    {
        if (name == RdmaOpcodeName(opcode))
        {
            return opcode;
        }
    }
    NS_ABORT_MSG("Unknown rdmaOperation: " << name);
}

bool
RdmaHasSourcePayload(RdmaOpcode opcode)
{
    return opcode == RdmaOpcode::WRITE || opcode == RdmaOpcode::SEND ||
           opcode == RdmaOpcode::WRITE_IMM || opcode == RdmaOpcode::SEND_IMM ||
           opcode == RdmaOpcode::PUT_SIGNAL;
}

bool
RdmaUsesReadAtomicCredit(RdmaOpcode opcode)
{
    return opcode == RdmaOpcode::READ || opcode == RdmaOpcode::FETCH_ADD ||
           opcode == RdmaOpcode::COMPARE_SWAP || opcode == RdmaOpcode::SIGNAL;
}

bool
RdmaNeedsReceive(RdmaOpcode opcode)
{
    return opcode == RdmaOpcode::SEND || opcode == RdmaOpcode::SEND_IMM ||
           opcode == RdmaOpcode::WRITE_IMM;
}

TypeId
RdmaOperationTag::GetTypeId()
{
    static TypeId tid = TypeId("ns3::RdmaOperationTag")
                            .SetParent<Tag>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<RdmaOperationTag>();
    return tid;
}

TypeId
RdmaOperationTag::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t
RdmaOperationTag::GetSerializedSize() const
{
    return 111;
}

void
RdmaOperationTag::Serialize(TagBuffer b) const
{
    b.WriteU8(static_cast<uint8_t>(opcode));
    b.WriteU8(static_cast<uint8_t>(phase));
    b.WriteU16(owner);
    for (auto v : {workId,
                   requestId,
                   bytes,
                   offset,
                   fragmentOffset,
                   localAddress,
                   remoteAddress,
                   signalAddress,
                   signalValue,
                   operand,
                   compare})
    {
        b.WriteU64(v);
    }
    for (auto v : {localKey, remoteKey, immediate, attempt})
    {
        b.WriteU32(v);
    }
    b.WriteU8(static_cast<uint8_t>(signalOp));
    b.WriteU8(zeroPayload);
    b.WriteU8(success);
}

void
RdmaOperationTag::Deserialize(TagBuffer b)
{
    opcode = static_cast<RdmaOpcode>(b.ReadU8());
    phase = static_cast<RdmaPhase>(b.ReadU8());
    owner = b.ReadU16();
    for (auto p : {&workId,
                   &requestId,
                   &bytes,
                   &offset,
                   &fragmentOffset,
                   &localAddress,
                   &remoteAddress,
                   &signalAddress,
                   &signalValue,
                   &operand,
                   &compare})
    {
        *p = b.ReadU64();
    }
    for (auto p : {&localKey, &remoteKey, &immediate, &attempt})
    {
        *p = b.ReadU32();
    }
    signalOp = static_cast<RdmaSignalOp>(b.ReadU8());
    zeroPayload = b.ReadU8();
    success = b.ReadU8();
}

void
RdmaOperationTag::Print(std::ostream& os) const
{
    os << RdmaOpcodeName(opcode) << " owner=" << owner << " work=" << workId;
}
} // namespace ns3
