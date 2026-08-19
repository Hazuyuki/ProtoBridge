/*
 * SPDX-License-Identifier: GPL-2.0-only
 * protocol-config.cc
 *
 * The OTP compiler. Parses a `.cfg` and compiles its `[op]` stencil into a
 * ProtocolTransactionGraph. Each declared transfer is expanded through the
 * vendor ProtocolModel::AddTransaction (which emits the ACTION + delivery
 * WAIT nodes, packetization, wire size, protocol id). The config therefore
 * declares transfers + a dependency DAG + delays -- never raw nodes.
 */

#include "protocol-config.h"
#include "protocol-model.h"
#include "fabric-header.h"
#include "ns3/log.h"
#include "ns3/nstime.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("ProtocolConfig");

namespace {

std::string
Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}

// Split "lo .. hi" into (lo, hi) expressions. Returns false if no "..".
bool
SplitRange(const std::string& s, std::string& lo, std::string& hi)
{
    size_t dot = s.find("..");
    if (dot == std::string::npos)
    {
        return false;
    }
    lo = Trim(s.substr(0, dot));
    hi = Trim(s.substr(dot + 2));
    return !lo.empty() && !hi.empty();
}

// ---- minimal recursive-descent integer expression evaluator ----------------
//
// Grammar (left-associative, integer arithmetic):
//   expr  = term (('+' | '-') term)*
//   term  = factor (('*' | '/' | 'mod') factor)*
//   factor = number | ident | '(' expr ')'
//
// `mod` is a keyword operator (same precedence as * /). Division and modulo
// by zero are errors. Subtraction underflow saturates to 0.

class EvalEngine
{
  public:
    EvalEngine(const std::unordered_map<std::string, uint64_t>& vars, std::string* err)
        : m_vars(vars), m_err(err) {}

    bool Run(const std::string& expr, uint64_t& out)
    {
        m_s = expr;
        m_pos = 0;
        SkipWs();
        if (!ParseExpr(out)) return false;
        SkipWs();
        if (m_pos != m_s.size())
        {
            return Fail("trailing characters in expression: '" + expr + "'");
        }
        return true;
    }

  private:
    const std::unordered_map<std::string, uint64_t>& m_vars;
    std::string* m_err;
    std::string m_s;
    size_t m_pos{0};

    bool Fail(const std::string& msg)
    {
        if (m_err) *m_err = msg;
        return false;
    }

    void SkipWs()
    {
        while (m_pos < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_pos])))
        {
            ++m_pos;
        }
    }

    bool PeekWord(const std::string& kw)
    {
        if (m_pos + kw.size() > m_s.size()) return false;
        if (m_s.compare(m_pos, kw.size(), kw) != 0) return false;
        char after = (m_pos + kw.size() < m_s.size()) ? m_s[m_pos + kw.size()] : '\0';
        return !std::isalnum(static_cast<unsigned char>(after)) && after != '_';
    }

    bool ConsumeWord(const std::string& kw)
    {
        if (!PeekWord(kw)) return false;
        m_pos += kw.size();
        return true;
    }

    bool ParseExpr(uint64_t& out)
    {
        if (!ParseTerm(out)) return false;
        for (;;)
        {
            SkipWs();
            if (m_pos < m_s.size() && (m_s[m_pos] == '+' || m_s[m_pos] == '-'))
            {
                char op = m_s[m_pos++];
                uint64_t rhs;
                if (!ParseTerm(rhs)) return false;
                out = (op == '+') ? out + rhs
                                 : (rhs > out ? 0 : out - rhs);
            }
            else break;
        }
        return true;
    }

    bool ParseTerm(uint64_t& out)
    {
        if (!ParseFactor(out)) return false;
        for (;;)
        {
            SkipWs();
            if (m_pos < m_s.size() && (m_s[m_pos] == '*' || m_s[m_pos] == '/'))
            {
                char op = m_s[m_pos++];
                uint64_t rhs;
                if (!ParseFactor(rhs)) return false;
                if (op == '*') out = out * rhs;
                else { if (rhs == 0) return Fail("division by zero"); out = out / rhs; }
            }
            else if (ConsumeWord("mod"))
            {
                uint64_t rhs;
                if (!ParseFactor(rhs)) return false;
                if (rhs == 0) return Fail("modulo by zero");
                out = out % rhs;
            }
            else break;
        }
        return true;
    }

    bool ParseFactor(uint64_t& out)
    {
        SkipWs();
        if (m_pos >= m_s.size()) return Fail("unexpected end of expression");
        char c = m_s[m_pos];
        if (c == '(')
        {
            ++m_pos;
            if (!ParseExpr(out)) return false;
            SkipWs();
            if (m_pos >= m_s.size() || m_s[m_pos] != ')') return Fail("missing ')'");
            ++m_pos;
            return true;
        }
        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            size_t start = m_pos;
            while (m_pos < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_pos])))
            {
                ++m_pos;
            }
            try { out = std::stoull(m_s.substr(start, m_pos - start)); }
            catch (...) { return Fail("invalid number"); }
            return true;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            size_t start = m_pos;
            while (m_pos < m_s.size() &&
                   (std::isalnum(static_cast<unsigned char>(m_s[m_pos])) || m_s[m_pos] == '_'))
            {
                ++m_pos;
            }
            std::string name = m_s.substr(start, m_pos - start);
            auto it = m_vars.find(name);
            if (it == m_vars.end())
            {
                return Fail("unknown symbol: '" + name + "'");
            }
            out = it->second;
            return true;
        }
        return Fail(std::string("unexpected character '") + c + "'");
    }
};

// Map a kind name to (ProtocolTransactionKind, FabricPacketType).
bool
ParseKind(const std::string& s, ProtocolTransactionKind& kind, FabricPacketType& ptype)
{
    if (s == "DATA" || s == "SEND_DATA") { kind = ProtocolTransactionKind::DATA_TRANSFER; ptype = FabricPacketType::DATA; return true; }
    if (s == "P2P")        { kind = ProtocolTransactionKind::P2P_TRANSFER; ptype = FabricPacketType::DATA; return true; }
    if (s == "MEMORY_READ"){ kind = ProtocolTransactionKind::MEMORY_READ; ptype = FabricPacketType::MEMORY_READ; return true; }
    if (s == "MEMORY_WRITE"){ kind = ProtocolTransactionKind::MEMORY_WRITE; ptype = FabricPacketType::MEMORY_WRITE; return true; }
    if (s == "COLLECTIVE") { kind = ProtocolTransactionKind::COLLECTIVE_OFFLOAD; ptype = FabricPacketType::ALLREDUCE; return true; }
    return false;
}

} // anonymous namespace

ProtocolConfig::ProtocolConfig() = default;
ProtocolConfig::~ProtocolConfig() = default;

void
ProtocolConfig::Clear()
{
    m_profilePath.clear();
    m_stack.clear();
    m_param.clear();
    m_dims.clear();
    m_transfer.clear();
    m_startup = "auto";
    m_perStepDelay = "0";
    m_complete = "all";
    m_collective.clear();
    m_algorithm.clear();
    m_topology.clear();
    m_currentSection.clear();
}

bool
ProtocolConfig::Load(const std::string& path, std::string* error)
{
    Clear();
    std::ifstream f(path);
    if (!f.is_open())
    {
        if (error) *error = "cannot open config: " + path;
        return false;
    }
    std::string line;
    size_t lineNo = 0;
    while (std::getline(f, line))
    {
        ++lineNo;
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']')
        {
            m_currentSection = Trim(t.substr(1, t.size() - 2));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos)
        {
            if (error) *error = "line " + std::to_string(lineNo) + ": expected key = value";
            return false;
        }
        std::string key = Trim(t.substr(0, eq));
        std::string val = t.substr(eq + 1);
        size_t hash = val.find('#');
        if (hash != std::string::npos) val = val.substr(0, hash);
        val = Trim(val);
        if (key.empty()) continue;

        if (m_currentSection == "stack")
        {
            if (key == "profile") m_profilePath = val;
            else m_stack[key] = val;
        }
        else if (m_currentSection == "op")
        {
            if (key.rfind("param.", 0) == 0)
            {
                m_param.emplace_back(key.substr(6), val);
            }
            else if (key.rfind("replicate.", 0) == 0)
            {
                Dim d;
                d.var = key.substr(10);
                if (!SplitRange(val, d.loExpr, d.hiExpr))
                {
                    if (error) *error = "line " + std::to_string(lineNo)
                        + ": replicate." + d.var + " must be 'lo .. hi'";
                    return false;
                }
                m_dims.push_back(d);
            }
            else if (key.rfind("transfer.", 0) == 0)
            {
                m_transfer[key] = val;
            }
            else if (key == "startup") m_startup = val;
            else if (key == "per_step_delay") m_perStepDelay = val;
            else if (key == "complete") m_complete = val;
            else if (key == "collective") m_collective = val; // delegate to a calibrated injector
            else if (key == "algorithm")  m_algorithm = val;  // paired with collective
            else if (key == "topology") m_topology = val;     // fabric the op runs on
            else
            {
                if (error) *error = "line " + std::to_string(lineNo) + ": unknown op key '" + key + "'";
                return false;
            }
        }
        // Sections other than [stack]/[op] are ignored.
    }

    // A `[op] collective =` / `algorithm =` op delegates to a calibrated inline
    // injector and needs no transfer stencil; otherwise the [op] stencil must
    // declare >=1 transfer. The two axes must be declared together.
    const bool hasAxes = !m_collective.empty() || !m_algorithm.empty();
    if (hasAxes && (m_collective.empty() || m_algorithm.empty()))
    {
        if (error) *error = "[op] collective= and algorithm= must both be set (or neither)";
        return false;
    }
    if (!hasAxes && m_transfer.empty())
    {
        if (error) *error = "config has no transfer.* entries in [op] and no collective=/algorithm=";
        return false;
    }
    return true;
}

const std::string&
ProtocolConfig::GetProfilePath() const
{
    return m_profilePath;
}

const std::unordered_map<std::string, std::string>&
ProtocolConfig::GetStackValues() const
{
    return m_stack;
}

const std::string&
ProtocolConfig::GetCollective() const
{
    return m_collective;
}

const std::string&
ProtocolConfig::GetAlgorithm() const
{
    return m_algorithm;
}

const std::string&
ProtocolConfig::GetTopology() const
{
    return m_topology;
}

bool
ProtocolConfig::Eval(const std::string& expr,
                    const std::unordered_map<std::string, uint64_t>& vars,
                    uint64_t& out, std::string* error) const
{
    EvalEngine e(vars, error);
    return e.Run(expr, out);
}

ProtocolTransactionNodeId
ProtocolConfig::Compile(ProtocolTransactionGraph& graph,
                        Ptr<ProtocolModel> proto,
                        uint16_t numGpus,
                        uint64_t dataSize,
                        uint16_t baseFlowId,
                        std::string* error) const
{
    if (!proto)
    {
        if (error) *error = "null protocol model";
        return PROTOCOL_TRANSACTION_INVALID_NODE;
    }

    graph.Clear();

    // --- 1. base symbols + params (in declaration order) -------------------
    std::unordered_map<std::string, uint64_t> vars;
    vars["numGpus"] = numGpus;
    vars["dataSize"] = dataSize;

    for (const auto& p : m_param)
    {
        uint64_t v;
        if (!Eval(p.second, vars, v, error))
        {
            if (error) *error = "param." + p.first + " = '" + p.second + "': " + *error;
            return PROTOCOL_TRANSACTION_INVALID_NODE;
        }
        vars[p.first] = v;
    }

    // --- 2. resolve replicate dim bounds ------------------------------------
    struct ResolvedDim { std::string var; uint64_t lo; uint64_t hi; };
    std::vector<ResolvedDim> rdims;
    for (const auto& d : m_dims)
    {
        ResolvedDim r;
        r.var = d.var;
        if (!Eval(d.loExpr, vars, r.lo, error))
        { if (error) *error = "replicate." + d.var + " lo: " + *error; return PROTOCOL_TRANSACTION_INVALID_NODE; }
        if (!Eval(d.hiExpr, vars, r.hi, error))
        { if (error) *error = "replicate." + d.var + " hi: " + *error; return PROTOCOL_TRANSACTION_INVALID_NODE; }
        if (r.hi < r.lo)
        { if (error) *error = "replicate." + d.var + " hi < lo"; return PROTOCOL_TRANSACTION_INVALID_NODE; }
        rdims.push_back(r);
    }

    // --- 3. enumerate instances (cartesian product) -------------------------
    std::vector<std::unordered_map<std::string, uint64_t>> instances;
    std::unordered_map<std::string, uint64_t> binding;
    std::function<void(size_t)> expand = [&](size_t idx)
    {
        if (idx >= rdims.size())
        {
            instances.push_back(binding);
            return;
        }
        for (uint64_t v = rdims[idx].lo; v <= rdims[idx].hi; ++v)
        {
            binding[rdims[idx].var] = v;
            expand(idx + 1);
        }
    };
    expand(0);
    if (instances.empty())
    {
        instances.push_back(binding);
    }

    // --- 4. collect transfer stencils, ordered by index --------------------
    std::map<uint32_t, std::unordered_map<std::string, std::string>> transfers;
    for (const auto& kv : m_transfer)
    {
        std::string rest = kv.first.substr(9); // drop "transfer."
        size_t dot = rest.find('.');
        if (dot == std::string::npos) continue;
        uint32_t idx = static_cast<uint32_t>(std::stoul(rest.substr(0, dot)));
        transfers[idx][rest.substr(dot + 1)] = kv.second;
    }
    if (transfers.empty())
    {
        if (error) *error = "no transfer stencils";
        return PROTOCOL_TRANSACTION_INVALID_NODE;
    }

    // --- 5. resolve startup + per_step_delay -------------------------------
    // startup "auto" -> proto startup for the total data size at numGpus gpus
    // (mirrors RingAllReduce::Initialize).
    uint64_t startupNs = 0;
    bool hasStartup = false;
    if (m_startup == "auto")
    {
        uint8_t startupProto = proto->GetProtocolId(dataSize);
        startupNs = proto->GetStartupDelayNs(startupProto, numGpus);
        hasStartup = startupNs > 0;
    }
    else
    {
        uint64_t v;
        if (Eval(m_startup, vars, v, error))
        {
            startupNs = v;
            hasStartup = startupNs > 0;
        }
    }
    uint64_t perStepNs = 0;
    {
        uint64_t v;
        if (Eval(m_perStepDelay, vars, v, error)) perStepNs = v;
    }

    // --- 6. emit the graph -------------------------------------------------
    // Chain groups = first replicate dim value. Within a group, iterate
    // instances in row-major order; within an instance, iterate transfers by
    // index. Insert per_step_delay before each (instance,transfer) pair
    // except the very first in the group (mirrors RingAllReduce's inter-step
    // delay with no trailing delay). Seed each group with a startup delay.
    const std::string firstDim = rdims.empty() ? std::string() : rdims[0].var;

    std::vector<size_t> order(instances.size());
    for (size_t i = 0; i < instances.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [&](size_t a, size_t b)
        {
            if (!firstDim.empty())
            {
                uint64_t ga = instances[a].count(firstDim) ? instances[a].at(firstDim) : 0;
                uint64_t gb = instances[b].count(firstDim) ? instances[b].at(firstDim) : 0;
                if (ga != gb) return ga < gb;
            }
            return a < b;
        });

    auto groupKeyOf = [&](const std::unordered_map<std::string, uint64_t>& b) -> uint64_t
    {
        return (!firstDim.empty() && b.count(firstDim)) ? b.at(firstDim) : 0;
    };

    std::map<uint64_t, ProtocolTransactionNodeId> groupDep; // last node in chain
    std::map<uint64_t, bool> groupSeeded;
    std::map<uint64_t, uint64_t> groupPairCount;
    std::vector<ProtocolTransactionNodeId> finals;
    uint32_t stageCounter = 0;

    for (size_t ord = 0; ord < order.size(); ++ord)
    {
        const auto& inst = instances[order[ord]];
        uint64_t gkey = groupKeyOf(inst);

        // Per-instance symbol table: global params + this instance's loop vars.
        std::unordered_map<std::string, uint64_t> instVars = vars;
        for (const auto& b : inst)
        {
            instVars[b.first] = b.second;
        }

        if (!groupSeeded[gkey])
        {
            if (hasStartup)
            {
                groupDep[gkey] = graph.AddDelay(NanoSeconds(startupNs), {}, "startup");
            }
            groupSeeded[gkey] = true;
        }

        for (const auto& kv : transfers)
        {
            uint32_t tidx = kv.first;
            const auto& stencil = kv.second;

            // Inter-pair delay (before this pair, except the first in the group).
            if (groupPairCount[gkey] > 0 && perStepNs > 0)
            {
                groupDep[gkey] = graph.AddDelay(
                    NanoSeconds(perStepNs), {groupDep[gkey]}, "step-delay");
            }
            groupPairCount[gkey] += 1;

            std::string kindStr = stencil.count("kind") ? stencil.at("kind") : "DATA";
            ProtocolTransactionKind kind;
            FabricPacketType ptype;
            if (!ParseKind(kindStr, kind, ptype))
            {
                if (error) *error = "transfer." + std::to_string(tidx)
                    + ".kind = '" + kindStr + "': unknown kind";
                return PROTOCOL_TRANSACTION_INVALID_NODE;
            }

            auto evalField = [&](const std::string& field, uint64_t def,
                                 uint64_t& out) -> bool
            {
                auto it = stencil.find(field);
                if (it == stencil.end()) { out = def; return true; }
                std::string e;
                if (!Eval(it->second, instVars, out, &e))
                {
                    if (error) *error = "transfer." + std::to_string(tidx)
                        + "." + field + " = '" + it->second + "': " + e;
                    return false;
                }
                return true;
            };

            uint64_t src = 0, dst = 0, bytes = 0, vc = 0, response = 0;
            if (!evalField("src", 0, src)) return PROTOCOL_TRANSACTION_INVALID_NODE;
            if (!evalField("dst", 0, dst)) return PROTOCOL_TRANSACTION_INVALID_NODE;
            if (!evalField("bytes", 0, bytes)) return PROTOCOL_TRANSACTION_INVALID_NODE;
            if (!evalField("vc", 0, vc)) return PROTOCOL_TRANSACTION_INVALID_NODE;
            if (!evalField("response", 0, response)) return PROTOCOL_TRANSACTION_INVALID_NODE;

            ProtocolTransactionRequest req;
            req.kind = kind;
            req.packetType = ptype;
            req.sourceRank = static_cast<uint16_t>(src);
            req.destinationRank = static_cast<uint16_t>(dst);
            req.virtualChannel = static_cast<uint8_t>(vc);
            req.effectiveBytes = bytes;
            req.responseBytes = response;
            req.stageId = stageCounter;
            req.flowId = static_cast<uint16_t>(baseFlowId + stageCounter);
            req.hasProtocolId = false; // auto: protocol derived from effectiveBytes
            req.label = "t" + std::to_string(tidx) + "-g" + std::to_string(src)
                      + "-s" + std::to_string(stageCounter);
            ++stageCounter;

            std::vector<ProtocolTransactionNodeId> deps;
            auto it = groupDep.find(gkey);
            if (it != groupDep.end() && it->second != PROTOCOL_TRANSACTION_INVALID_NODE)
            {
                deps.push_back(it->second);
            }
            ProtocolTransactionNodeId wait = proto->AddTransaction(graph, req, deps);
            groupDep[gkey] = wait;
        }
    }

    for (const auto& kv : groupDep)
    {
        if (kv.second != PROTOCOL_TRANSACTION_INVALID_NODE)
        {
            finals.push_back(kv.second);
        }
    }
    if (finals.empty())
    {
        if (error) *error = "compiled graph has no terminal nodes";
        return PROTOCOL_TRANSACTION_INVALID_NODE;
    }

    ProtocolTransactionDependency depKind = ProtocolTransactionDependency::ALL;
    if (m_complete == "any") depKind = ProtocolTransactionDependency::ANY;
    ProtocolTransactionNodeId completeNode = graph.AddCompletion(finals, depKind, "complete");

    std::string verr;
    if (!graph.Validate(&verr))
    {
        if (error) *error = "compiled graph invalid: " + verr;
        return PROTOCOL_TRANSACTION_INVALID_NODE;
    }
    return completeNode;
}

} // namespace ns3
