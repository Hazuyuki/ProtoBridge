/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 */

#include "credit-manager.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("CreditManager");

NS_OBJECT_ENSURE_REGISTERED(CreditManager);

TypeId
CreditManager::GetTypeId()
{
    static TypeId tid = TypeId("ns3::CreditManager")
                            .SetParent<Object>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<CreditManager>()
                            .AddTraceSource("CreditChange",
                                            "Credit count changed for a VC",
                                            MakeTraceSourceAccessor(&CreditManager::m_creditChangeTrace),
                                            "ns3::CreditManager::CreditChangeTracedCallback");
    return tid;
}

CreditManager::CreditManager()
    : m_maxVcs(8)  // Default to 8 VCs
{
    NS_LOG_FUNCTION(this);
}

CreditManager::~CreditManager()
{
    NS_LOG_FUNCTION(this);
}

void
CreditManager::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_vcCredits.clear();
    Object::DoDispose();
}

void
CreditManager::InitializeVc(uint8_t vcId, uint32_t numCredits)
{
    NS_LOG_FUNCTION(this << static_cast<int>(vcId) << numCredits);

    VcCreditInfo info;
    info.availableCredits = numCredits;
    info.totalCredits = numCredits;
    info.pendingCredits = 0;

    m_vcCredits[vcId] = info;
}

bool
CreditManager::HasCredits(uint8_t vcId) const
{
    auto it = m_vcCredits.find(vcId);
    if (it == m_vcCredits.end())
    {
        return false;
    }
    return it->second.availableCredits > 0;
}

uint32_t
CreditManager::GetAvailableCredits(uint8_t vcId) const
{
    auto it = m_vcCredits.find(vcId);
    if (it == m_vcCredits.end())
    {
        return 0;
    }
    return it->second.availableCredits;
}

bool
CreditManager::ConsumeCredit(uint8_t vcId, uint32_t seqNum)
{
    NS_LOG_FUNCTION(this << static_cast<int>(vcId) << seqNum);

    auto it = m_vcCredits.find(vcId);
    if (it == m_vcCredits.end())
    {
        NS_LOG_WARN("VC " << static_cast<int>(vcId) << " not initialized");
        return false;
    }

    if (it->second.availableCredits == 0)
    {
        NS_LOG_DEBUG("No credits available for VC " << static_cast<int>(vcId));
        return false;
    }

    NS_ASSERT_MSG(it->second.availableCredits > 0,
                  "ConsumeCredit called with zero available credits for VC " << static_cast<int>(vcId));
    uint32_t oldCredits = it->second.availableCredits;
    it->second.availableCredits--;
    it->second.pendingCredits++;

    NS_ASSERT_MSG(it->second.availableCredits + it->second.pendingCredits == it->second.totalCredits,
                  "Credit invariant violated for VC " << static_cast<int>(vcId)
                  << ": available=" << it->second.availableCredits
                  << " pending=" << it->second.pendingCredits
                  << " total=" << it->second.totalCredits);
    NS_ASSERT_MSG(it->second.pendingCredits <= it->second.totalCredits,
                  "Pending credits exceed total for VC " << static_cast<int>(vcId)
                  << ": pending=" << it->second.pendingCredits
                  << " total=" << it->second.totalCredits);

    m_creditChangeTrace(vcId, oldCredits, it->second.availableCredits);

    NS_LOG_DEBUG("Consumed credit for VC " << static_cast<int>(vcId)
                 << ", remaining: " << it->second.availableCredits);
    return true;
}

void
CreditManager::ReturnCredits(uint8_t vcId, uint32_t numCredits)
{
    NS_LOG_FUNCTION(this << static_cast<int>(vcId) << numCredits);

    auto it = m_vcCredits.find(vcId);
    if (it == m_vcCredits.end())
    {
        NS_LOG_WARN("VC " << static_cast<int>(vcId) << " not initialized");
        return;
    }

    NS_ASSERT_MSG(numCredits > 0,
                  "ReturnCredits called with zero credits for VC " << static_cast<int>(vcId));

    uint32_t oldCredits = it->second.availableCredits;
    it->second.availableCredits += numCredits;

    // Cap at total credits
    if (it->second.availableCredits > it->second.totalCredits)
    {
        it->second.availableCredits = it->second.totalCredits;
    }

    if (it->second.pendingCredits >= numCredits)
    {
        it->second.pendingCredits -= numCredits;
    }
    else
    {
        it->second.pendingCredits = 0;
    }

    NS_ASSERT_MSG(it->second.availableCredits + it->second.pendingCredits == it->second.totalCredits,
                  "Credit invariant violated after ReturnCredits for VC " << static_cast<int>(vcId)
                  << ": available=" << it->second.availableCredits
                  << " pending=" << it->second.pendingCredits
                  << " total=" << it->second.totalCredits);
    NS_ASSERT_MSG(it->second.pendingCredits <= it->second.totalCredits,
                  "Pending credits exceed total after ReturnCredits for VC " << static_cast<int>(vcId)
                  << ": pending=" << it->second.pendingCredits
                  << " total=" << it->second.totalCredits);

    m_creditChangeTrace(vcId, oldCredits, it->second.availableCredits);

    NS_LOG_DEBUG("Returned " << numCredits << " credits for VC " << static_cast<int>(vcId)
                 << ", now available: " << it->second.availableCredits);

    // Notify callback if we now have credits available
    if (oldCredits == 0 && it->second.availableCredits > 0 && !m_creditAvailableCallback.IsNull())
    {
        m_creditAvailableCallback(vcId);
    }
}

void
CreditManager::SetTotalCredits(uint8_t vcId, uint32_t credits)
{
    NS_LOG_FUNCTION(this << static_cast<int>(vcId) << credits);

    auto it = m_vcCredits.find(vcId);
    if (it == m_vcCredits.end())
    {
        InitializeVc(vcId, credits);
        return;
    }

    int32_t diff = static_cast<int32_t>(credits) - static_cast<int32_t>(it->second.totalCredits);
    it->second.totalCredits = credits;
    it->second.availableCredits = static_cast<uint32_t>(
        static_cast<int32_t>(it->second.availableCredits) + diff);

    if (it->second.availableCredits > it->second.totalCredits)
    {
        it->second.availableCredits = it->second.totalCredits;
    }
}

uint32_t
CreditManager::GetNumVcs() const
{
    return static_cast<uint32_t>(m_vcCredits.size());
}

void
CreditManager::SetCreditAvailableCallback(CreditAvailableCallback cb)
{
    m_creditAvailableCallback = cb;
}

} // namespace ns3
