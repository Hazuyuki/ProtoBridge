/*
 * protocol-profile-demo.cc
 *
 * Demonstrates the ProtocolProfile config interface: load one profile
 * file, get back a ready-to-use {ProtocolModel, FecModel, CreditManager,
 * BER} bundle, and print its configuration. This scratch program does
 * not run a simulation -- it shows the "make the protocol ready to use"
 * step.
 *
 *   ./ns3 run "protocol-profile-demo --profile=configs/protocol_profiles/h200-ll128.profile"
 */

#include "ns3/core-module.h"
#include "ns3/gpu-cluster-module.h"
#include "ns3/protocol-profile.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("ProtocolProfileDemo");

int
main(int argc, char* argv[])
{
    std::string profilePath = "configs/protocol_profiles/h200-ll128.profile";
    CommandLine cmd;
    cmd.AddValue("profile", "Path to a .profile file", profilePath);
    cmd.Parse(argc, argv);

    ProtocolProfile prof;
    if (!prof.Load(profilePath))
    {
        NS_ABORT_MSG("could not load profile: " << profilePath);
    }

    ProtocolBundle b = prof.Build();

    // The bundle is now ready: hand b.protocol / b.fec / b.credits to your
    // FabricEndpoint + NetDevice, and b.ber* to LinkDegradationModel.
    std::cout << "=== ProtocolProfile bundle ready ===\n";
    std::cout << "  profile        : " << profilePath << "\n";
    std::cout << "  vendor         : " << b.protocol->GetVendorName() << "\n";
    std::cout << "  forceProtocol  : " << static_cast<int>(b.protocol->GetForceProtocolId())
              << (b.protocol->GetForceProtocolId() == 0 ? " (auto)" : "") << "\n";
    std::cout << "  FEC (N/K/T)    : " << b.fec->GetN() << "/" << b.fec->GetK() << "/"
              << b.fec->GetT() << "  rate=" << b.fec->GetCodeRate() << "\n";
    std::cout << "  FEC encode/dec : " << b.fec->GetEncodeLatency().GetNanoSeconds() << "/"
              << b.fec->GetDecodeLatency().GetNanoSeconds() << " ns\n";
    std::cout << "  VCs (count x cr): " << b.credits->GetNumVcs() << " VCs initialized\n";
    std::cout << "  BER node/rack/opt: " << b.berIntraNodeElectrical << " / "
              << b.berIntraRackElectrical << " / " << b.berInterRackOptical << "\n";
    std::cout << "  flowControl    : " << FlowControlPolicyName(b.flowControl) << "\n";
    std::cout << "  LLR            : " << (b.llrEnabled ? "on" : "off")
              << "  mode=" << (b.llrMode.empty() ? "gobackn" : b.llrMode) << "\n";
    std::cout << "=== ready to run ===" << std::endl;

    return 0;
}
