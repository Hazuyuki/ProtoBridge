/*
 * SPDX-License-Identifier: GPL-2.0-only
 * protocol-profile.cc
 *
 * Implementation of the ProtocolProfile config loader. Pure composition:
 * it instantiates existing objects (ProtocolModel via ObjectFactory,
 * FecModel, CreditManager) and configures them from the file. No existing
 * class is modified.
 */

#include "protocol-profile.h"
#include "protocol-model.h"
#include "protocol-payload-builder.h"
#include "fec-model.h"
#include "credit-manager.h"
#include "ns3/object-factory.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/log.h"

#include <fstream>
#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ProtocolProfile");

// Reserved keys that Build() consumes directly (not forwarded as attributes).
static const char* RESERVED[] = {
    "protocolModel", "forceProtocol",
    "fecN", "fecK", "fecT",
    "fecEncodeLatencyNs", "fecDecodeLatencyNs",
    "vcCount", "vcCredits",
    "berIntraNodeElectrical", "berIntraRackElectrical", "berInterRackOptical",
    "flowControl", "llrEnabled", "llrMode",
    // Fabric hardware (consumed by the sim's topology builder, not protocol attrs).
    "bandwidthGbps", "latencyNs", "numLanes", "linksPerGpu",
    "sprayChunkSize", "switchVoqDepth", "switchArbIntervalNs"};
static bool
IsReserved(const std::string& k)
{
    for (const char* r : RESERVED)
    {
        if (k == r)
        {
            return true;
        }
    }
    return false;
}

ProtocolProfile::ProtocolProfile() = default;
ProtocolProfile::~ProtocolProfile() = default;

std::string
ProtocolProfile::Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}

bool
ProtocolProfile::Load(const std::string& path)
{
    m_kv.clear();
    std::ifstream f(path);
    if (!f.is_open())
    {
        NS_LOG_ERROR("ProtocolProfile: cannot open " << path);
        m_loaded = false;
        return false;
    }
    std::string line;
    while (std::getline(f, line))
    {
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';')
        {
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        std::string key = Trim(t.substr(0, eq));
        std::string val = t.substr(eq + 1);
        // Strip an inline `#` comment (the whole-line case is handled above).
        size_t hash = val.find('#');
        if (hash != std::string::npos)
        {
            val = val.substr(0, hash);
        }
        val = Trim(val);
        if (key.empty())
        {
            continue;
        }
        m_kv[key] = val;
    }
    m_loaded = !m_kv.empty();
    NS_LOG_INFO("ProtocolProfile: loaded " << m_kv.size() << " keys from " << path);
    return m_loaded;
}

bool
ProtocolProfile::IsLoaded() const
{
    return m_loaded;
}

void
ProtocolProfile::Set(const std::string& key, const std::string& value)
{
    m_kv[key] = value;
    m_loaded = true;
}

const std::unordered_map<std::string, std::string>&
ProtocolProfile::GetValues() const
{
    return m_kv;
}

std::string
ProtocolProfile::Get(const std::string& key, const std::string& fallback) const
{
    auto it = m_kv.find(key);
    return (it == m_kv.end()) ? fallback : it->second;
}

ProtocolBundle
ProtocolProfile::Build() const
{
    ProtocolBundle b;

    // --- Protocol model ---------------------------------------------------
    std::string tid = Get("protocolModel", "ns3::NcclProtocolModel");
    ObjectFactory factory(tid);
    Ptr<ProtocolModel> proto = factory.Create<ProtocolModel>();
    if (!proto)
    {
        NS_LOG_WARN("ProtocolProfile: could not create " << tid
                                                        << "; falling back to NcclProtocolModel");
        factory.SetTypeId("ns3::NcclProtocolModel");
        proto = factory.Create<ProtocolModel>();
    }
    // Apply every non-reserved key as a String-typed attribute. ns-3 parses
    // the string to the attribute's real type, so this works for Uinteger,
    // Double, etc. across any vendor without a case list.
    for (const auto& kv : m_kv)
    {
        if (IsReserved(kv.first))
        {
            continue;
        }
        proto->SetAttributeFailSafe(kv.first, StringValue(kv.second));
    }
    // Force protocol id (0 = auto-select by size).
    std::string fp = Get("forceProtocol", "0");
    if (fp != "0")
    {
        uint8_t id = static_cast<uint8_t>(std::stoul(fp));
        proto->SetForceProtocolId(id);
    }
    b.protocol = proto;

    // --- Payload builder (vendor-specific, paired with the protocol) -----
    // Derive the builder type from the *effective* protocol TypeId (robust to
    // the NCCL fallback above), mirroring the main sim's type map so the
    // bundle is genuinely ready (protocol + builder).
    std::string effTid = proto->GetInstanceTypeId().GetName();
    std::string pbType;
    if (effTid == "ns3::NcclProtocolModel")
    {
        pbType = "ns3::NcclProtocolPayloadBuilder";
    }
    else if (effTid == "ns3::McclProtocolModel")
    {
        pbType = "ns3::McclPayloadBuilder";
    }
    else if (effTid == "ns3::UbProtocolModel")
    {
        pbType = "ns3::UbPayloadBuilder";
    }
    if (!pbType.empty())
    {
        ObjectFactory pbFactory(pbType);
        b.payloadBuilder = pbFactory.Create<ProtocolPayloadBuilder>();
    }

    // --- FEC model --------------------------------------------------------
    Ptr<FecModel> fec = CreateObject<FecModel>();
    uint32_t n = std::stoul(Get("fecN", "544"));
    uint32_t k = std::stoul(Get("fecK", "514"));
    uint32_t t = std::stoul(Get("fecT", "15"));
    fec->SetFecParams(n, k, t);
    uint64_t encNs = std::stoul(Get("fecEncodeLatencyNs", "50"));
    uint64_t decNs = std::stoul(Get("fecDecodeLatencyNs", "80"));
    fec->SetEncodeLatency(NanoSeconds(encNs));
    fec->SetDecodeLatency(NanoSeconds(decNs));
    b.fec = fec;

    // --- Credit manager ---------------------------------------------------
    Ptr<CreditManager> cm = CreateObject<CreditManager>();
    uint32_t vcCount = std::stoul(Get("vcCount", "4"));
    uint32_t vcCredits = std::stoul(Get("vcCredits", "64"));
    for (uint32_t v = 0; v < vcCount; ++v)
    {
        cm->InitializeVc(static_cast<uint8_t>(v), vcCredits);
    }
    b.credits = cm;

    // --- Link BER ---------------------------------------------------------
    b.berIntraNodeElectrical = std::stod(Get("berIntraNodeElectrical", "1e-15"));
    b.berIntraRackElectrical = std::stod(Get("berIntraRackElectrical", "1e-13"));
    b.berInterRackOptical = std::stod(Get("berInterRackOptical", "1e-9"));

    // --- Flow control policy + LLR (applied to each endpoint by caller) --
    b.flowControl = FlowControlPolicyFromString(Get("flowControl", "credit"));
    b.llrEnabled = (Get("llrEnabled", "0") == "1" || Get("llrEnabled", "0") == "true");
    b.llrMode = Get("llrMode", "gobackn");

    NS_LOG_INFO("ProtocolProfile: built bundle proto=" << proto->GetVendorName()
              << " fec=" << n << "/" << k << "/" << t
              << " vcs=" << vcCount << "x" << vcCredits
              << " flowControl=" << FlowControlPolicyName(b.flowControl)
              << " llrEnabled=" << b.llrEnabled << " llrMode=" << b.llrMode
              << " berOpt=" << b.berInterRackOptical);
    return b;
}

} // namespace ns3
