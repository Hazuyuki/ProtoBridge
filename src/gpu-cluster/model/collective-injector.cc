/*
 * collective-injector.cc
 */

#include "collective-injector.h"
#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("CollectiveInjector");

NS_OBJECT_ENSURE_REGISTERED(CollectiveInjector);

std::string CollectiveInjector::g_stepTraceFile;
std::ofstream CollectiveInjector::g_stepTraceStream;

TypeId
CollectiveInjector::GetTypeId()
{
    static TypeId tid = TypeId("ns3::CollectiveInjector")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster");
    return tid;
}

CollectiveInjector::CollectiveInjector()
{
}

CollectiveInjector::~CollectiveInjector()
{
}

void
CollectiveInjector::SetStepTraceFile(const std::string& path)
{
    g_stepTraceFile = path;
    if (g_stepTraceStream.is_open())
    {
        g_stepTraceStream.close();
    }
    if (!path.empty())
    {
        g_stepTraceStream.open(path, std::ios::out | std::ios::trunc);
        g_stepTraceStream << "gpu,step,collectiveStartNs,completeNs,sinceStartNs" << std::endl;
    }
}

void
CollectiveInjector::LogStepComplete(uint16_t gpu, uint32_t step,
                                    uint64_t startNs, uint64_t completeNs)
{
    if (g_stepTraceStream.is_open())
    {
        g_stepTraceStream << gpu << "," << step << ","
                          << startNs << "," << completeNs << ","
                          << (completeNs - startNs) << std::endl;
        // sinceStartNs = completeNs - collectiveStartNs (cumulative, NOT per-step delay)
    }
}

} // namespace ns3
