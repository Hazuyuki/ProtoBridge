/*
 * SPDX-License-Identifier: GPL-2.0-only
 * protocol-profile.h
 *
 * Config-driven protocol bundle builder. Reads a simple `key = value`
 * profile file and produces a ready-to-use {ProtocolModel, FecModel,
 * CreditManager, BER} bundle. This is a pure interface on top of existing
 * objects -- it creates and configures them, it does not modify any
 * existing class. A user (or scratch program) hands the bundle to its
 * FabricEndpoint / NetDevice and is ready to run.
 *
 * Reserved keys (drive the bundle directly):
 *   protocolModel           TypeId string, e.g. "ns3::NcclProtocolModel"
 *   forceProtocol           0=auto, else a protocol id
 *   fecN / fecK / fecT      FEC codeword params (RS-style)
 *   fecEncodeLatencyNs      per-codeword encode latency
 *   fecDecodeLatencyNs      per-codeword decode latency
 *   vcCount                 number of virtual channels to initialize
 *   vcCredits               credits per VC
 *   berIntraNodeElectrical  BER for intra-node electrical links
 *   berIntraRackElectrical  BER for intra-rack electrical links
 *   berInterRackOptical     BER for inter-rack optical links
 *   flowControl             credit | window | rate (send-gate policy)
 *   llrEnabled              0/1, enable link-level retry on the endpoints
 *   llrMode                 gobackn | sack (GBN cumulative vs SACK selective)
 *   bandwidthGbps           per-link effective bandwidth (Gbps) -- fabric hw
 *   latencyNs               per-link latency (ns) -- fabric hw
 *   numLanes                 physical lanes per logical link -- fabric hw
 *   linksPerGpu             links per GPU to the switch -- fabric hw
 *   sprayChunkSize          spray chunk size (bytes) -- fabric hw
 *   switchVoqDepth          NVSwitch VOQ depth -- fabric hw
 *   switchArbIntervalNs     NVSwitch arbitration interval (ns) -- fabric hw
 *
 * The fabric-hw keys are consumed by the simulator's topology builder (not
 * protocol attributes); they let a `.cfg` run reproduce a calibrated fabric
 * without per-run CLI flags. Any other key is applied to the ProtocolModel
 * object as a String-typed attribute via SetAttributeFailSafe, so
 * vendor-specific attributes (StartupDelayLL, LlThreshold, ...) work
 * generically without a per-vendor case list.
 */

#ifndef PROTOCOL_PROFILE_H
#define PROTOCOL_PROFILE_H

#include "ns3/core-module.h"
#include "flow-control-policy.h"
#include <string>
#include <unordered_map>

namespace ns3
{

class ProtocolModel;
class ProtocolPayloadBuilder;
class FecModel;
class CreditManager;

/**
 * @ingroup gpu-cluster
 * @brief A fully-configured protocol stack ready to plug into a sim.
 *
 * The ProtocolModel is created from the profile's TypeId and has its
 * attributes set; the FecModel is parameterized; the CreditManager has
 * its VCs initialized. The three BER values are exposed for the link
 * layer (LinkDegradationModel) to consume. The flowControl policy and
 * LLR (enable + mode) fields are applied to each FabricEndpoint's send
 * gate / LLR manager by the caller.
 */
struct ProtocolBundle
{
    Ptr<ProtocolModel> protocol;       ///< configured protocol model (ready)
    Ptr<ProtocolPayloadBuilder> payloadBuilder; ///< vendor payload builder (ready)
    Ptr<FecModel> fec;                 ///< parameterized FEC model (ready)
    Ptr<CreditManager> credits;        ///< VC-initialized credit manager (ready)
    double berIntraNodeElectrical = 0.0;
    double berIntraRackElectrical = 0.0;
    double berInterRackOptical = 0.0;
    FlowControlPolicy flowControl = FlowControlPolicy::CREDIT; ///< send-gate policy
    bool llrEnabled = false;           ///< enable link-level retry
    std::string llrMode;              ///< "gobackn" | "sack" (applied to LlrManager)
};

/**
 * @ingroup gpu-cluster
 * @brief Loads a `key = value` profile file and builds a ProtocolBundle.
 *
 * Usage:
 * @code
 *   ProtocolProfile prof;
 *   if (!prof.Load("h200-ll128.profile")) { NS_ABORT_MSG("bad profile"); }
 *   ProtocolBundle b = prof.Build();
 *   // b.protocol, b.fec, b.credits are ready to use.
 * @endcode
 */
class ProtocolProfile
{
  public:
    ProtocolProfile();
    ~ProtocolProfile();

    /**
     * @brief Parse a profile file. Lines beginning with '#' and blank
     *        lines are ignored. Each remaining line is `key = value`.
     * @return true on success (file readable and non-empty).
     */
    bool Load(const std::string& path);

    /// True after a successful Load().
    bool IsLoaded() const;

    /**
     * @brief Build the ready-to-use bundle. Must be called after Load().
     *        If no profile is loaded, returns a bundle built from defaults.
     */
    ProtocolBundle Build() const;

    /// Raw access to the parsed key/value map (for debugging/override).
    const std::unordered_map<std::string, std::string>& GetValues() const;

    /// Set or override a single key programmatically (before Build()).
    void Set(const std::string& key, const std::string& value);

    /// Read one key with a fallback. Public so the simulator can source
    /// profile values (PEX + fabric hardware) into its CLI variables when a
    /// `.cfg` op delegates to a calibrated inline injector (`collective =`
    /// + `algorithm =`).
    std::string Get(const std::string& key, const std::string& fallback) const;

  private:
    static std::string Trim(const std::string& s);

    std::unordered_map<std::string, std::string> m_kv;
    bool m_loaded = false;
};

} // namespace ns3

#endif // PROTOCOL_PROFILE_H
