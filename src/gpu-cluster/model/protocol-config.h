/*
 * SPDX-License-Identifier: GPL-2.0-only
 * protocol-config.h
 *
 * Config-driven protocol stack. This is the OTP (operation-to-packet)
 * compiler: it reads a small `.cfg` file that declares (a) a PEX protocol
 * stack by reference to a `ProtocolProfile`, and (b) an operation as a
 * replicated stencil of logical transfers, then compiles the stencil into a
 * `ProtocolTransactionGraph` by expanding each transfer through
 * `ProtocolModel::AddTransaction` (which emits the vendor ACTION + delivery
 * WAIT nodes). The caller hands the compiled graph to a generic executor
 * (see ProtocolConfigRunner); the PEX bundle comes from ProtocolProfile.
 *
 * The config therefore declares transfers + a dependency DAG + delays -- it
 * never writes raw graph nodes, because the vendor ProtocolModel owns the
 * ACTION/WAIT expansion (packetization, wire size, protocol id). This is the
 * "define a protocol in tens of lines of config" seam from the paper.
 *
 * File format (INI-style, `key = value`, `#` comments, `[section]` headers):
 *
 *   [stack]
 *   profile = configs/protocol_profiles/h200-ll128.profile
 *
 *   [op]
 *   param.N        = numGpus                       # symbolic params (in order)
 *   param.segment  = dataSize / N
 *   param.steps    = 2 * (N - 1)
 *
 *   replicate.gpu  = 0 .. N-1                      # one instance per (gpu,step)
 *   replicate.step = 0 .. steps-1
 *
 *   startup        = auto                          # auto = protocol startup
 *   per_step_delay = 0                             # ns between a gpu's steps
 *
 *   transfer.kind  = DATA                          # DATA|P2P|MEMORY_READ|...
 *   transfer.src   = gpu
 *   transfer.dst   = (gpu + 1) mod N
 *   transfer.bytes = segment
 *   transfer.vc    = 0
 *
 *   complete       = all
 *
 * Symbolic expressions support integer literals, identifiers (loop vars,
 * params, `numGpus`, `dataSize`), `+ - * / mod` and parentheses. Integer
 * arithmetic, evaluated at compile time.
 */

#ifndef PROTOCOL_CONFIG_H
#define PROTOCOL_CONFIG_H

#include "protocol-transaction.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ns3
{

class ProtocolModel;

/**
 * @ingroup gpu-cluster
 * @brief The OTP compiler: parses a `.cfg` and compiles its `[op]` stencil
 *        into a ProtocolTransactionGraph via a vendor ProtocolModel.
 *
 * The PEX bundle (protocol/FEC/credits/BER) is NOT built here -- the caller
 * loads the `[stack]` profile path via GetProfilePath() and builds a
 * ProtocolBundle with ProtocolProfile. This class only owns the OTP side.
 */
class ProtocolConfig
{
  public:
    ProtocolConfig();
    ~ProtocolConfig();

    /**
     * @brief Parse a `.cfg` file. Returns false (and sets *error) on a parse
     *        or semantic error. After success, GetProfilePath() is usable.
     */
    bool Load(const std::string& path, std::string* error = nullptr);

    /// Path from the `[stack] profile = ...` line (empty if none declared).
    const std::string& GetProfilePath() const;

    /// Stack-section key/value overrides beyond `profile` (for the caller).
    const std::unordered_map<std::string, std::string>& GetStackValues() const;

    /**
     * @brief Compile the `[op]` stencil into `graph`.
     *
     * Expands the replicate domain, evaluates each transfer stencil per
     * instance, and emits vendor ACTION+WAIT nodes through
     * `proto->AddTransaction`. Chains instances grouped by the first
     * replicate variable (the chain axis) via `per_step_delay`, seeds each
     * chain with a `startup` delay, and adds a terminal completion node.
     *
     * @param proto  the vendor protocol model (drives packetization/wire size).
     * @param numGpus  GPU count (resolves the `numGpus` symbol).
     * @param dataSize total operation bytes (resolves `dataSize`).
     * @param baseFlowId flow ids are assigned baseFlowId + stageId (matches
     *                   the runner's receive-side stageId derivation).
     * @return the terminal completion node id, or PROTOCOL_TRANSACTION_INVALID_NODE
     *         on error (with *error set).
     */
    ProtocolTransactionNodeId Compile(ProtocolTransactionGraph& graph,
                                      Ptr<ProtocolModel> proto,
                                      uint16_t numGpus,
                                      uint64_t dataSize,
                                      uint16_t baseFlowId,
                                      std::string* error = nullptr) const;

  private:
    struct Dim
    {
        std::string var;
        std::string loExpr;
        std::string hiExpr;
    };

    bool ParseLine(const std::string& rawLine, std::string* error);
    void Clear();

    // Integer expression evaluator (recursive descent).
    bool Eval(const std::string& expr,
              const std::unordered_map<std::string, uint64_t>& vars,
              uint64_t& out,
              std::string* error) const;

    std::string m_profilePath;
    std::unordered_map<std::string, std::string> m_stack;       // [stack] keys
    std::vector<std::pair<std::string, std::string>> m_param;   // param.<name> (ordered)
    std::vector<Dim> m_dims;                                     // replicate.<var>
    std::unordered_map<std::string, std::string> m_transfer;     // transfer.<field>
    std::string m_startup{"auto"};
    std::string m_perStepDelay{"0"};
    std::string m_complete{"all"};
    std::string m_currentSection;
};

} // namespace ns3

#endif // PROTOCOL_CONFIG_H
