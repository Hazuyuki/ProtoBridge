/*
 * SPDX-License-Identifier: GPL-2.0-only
 * protocol-config-runner.h
 *
 * The generic graph runner: binds a compiled ProtocolConfig graph to a set
 * of FabricEndpoints and drives the ProtocolTransactionExecutor. This is the
 * OTP->PEX handoff seam:
 *
 *   OTP -> PEX : the executor's ActionCallback fires SEND_DATA actions; the
 *                runner calls FabricEndpoint::SendBulkWireTransferSize (which
 *                packetizes, sprays, and applies FEC/credit/LLR per the bundle).
 *   PEX -> OTP : each endpoint's ReceiveCallback builds a ProtocolTransaction
 *                Event from the delivered packet and calls NotifyEvent, which
 *                completes matching delivery WAIT nodes and advances the graph.
 *
 * The runner also applies a ProtocolBundle (PEX stack: protocol, FEC, VCs +
 * credits, flow-control policy, LLR, bulk chunk size) to the endpoints -- a
 * convenience so a scratch program can go from config -> running sim without
 * replicating the per-endpoint setter dance.
 */

#ifndef PROTOCOL_CONFIG_RUNNER_H
#define PROTOCOL_CONFIG_RUNNER_H

#include "protocol-config.h"
#include "protocol-profile.h"
#include "protocol-transaction.h"
#include "fabric-endpoint.h"

#include "ns3/ptr.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Glue object that binds a config-compiled graph to endpoints + the
 *        executor and drives it. A plain class (not an ns3::Object): it owns
 *        an executor and registers callbacks, but is not itself instantiated
 *        via ObjectFactory.
 */
class ProtocolConfigRunner
{
  public:
    ProtocolConfigRunner();
    ~ProtocolConfigRunner();

    /**
     * @brief Apply a PEX bundle to every endpoint (protocol, payload builder,
     *        FEC, VCs + per-VC credits, flow-control policy, LLR, bulk chunk
     *        size). Safe to call after an existing inline setup to override the
     *        credit/VC/flow-control configuration with the profile's values.
     */
    static void ApplyBundle(const std::vector<Ptr<FabricEndpoint>>& endpoints,
                            const ProtocolBundle& bundle,
                            uint32_t bulkChunkSize = 8 * 1024 * 1024);

    /**
     * @brief Compile the config's [op] graph, wire the executor + receive
     *        callbacks, and prepare to Start(). Returns false on a compile
     *        error (with *error set).
     */
    bool Initialize(const std::vector<Ptr<FabricEndpoint>>& endpoints,
                    const ProtocolBundle& bundle,
                    const ProtocolConfig& config,
                    uint16_t numGpus,
                    uint64_t dataSize,
                    std::string* error = nullptr);

    /// Register the completion callback (fires once when the graph completes).
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb);

    /// Begin executing the graph. Records the start time for the duration.
    void Start();

    /// True after the completion callback has fired.
    bool IsComplete() const;

  private:
    void OnAction(const ProtocolTransactionAction& action);
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void OnComplete();

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    Ptr<ProtocolTransactionExecutor> m_executor;
    uint16_t m_baseFlowId{1};
    bool m_complete{false};
    int64_t m_startNs{0};
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // PROTOCOL_CONFIG_RUNNER_H
