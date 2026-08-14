/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Credit Manager for VC-isolated flow control
 */

#ifndef CREDIT_MANAGER_H
#define CREDIT_MANAGER_H

#include "ns3/object.h"
#include "ns3/callback.h"
#include "ns3/traced-callback.h"

#include <unordered_map>
#include <cstdint>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Credit information for a single virtual channel
 */
struct VcCreditInfo
{
    uint32_t availableCredits;      ///< Current available credits
    uint32_t totalCredits;          ///< Total credits allocated
    uint32_t pendingCredits;        ///< Credits pending acknowledgment
};

/**
 * @ingroup gpu-cluster
 * @brief Manages credit-based flow control for GPU endpoints
 *
 * This class implements per-VC credit management:
 * - Tracks available credits per virtual channel
 * - Handles credit allocation and return
 * - Supports credit-based backpressure
 */
class CreditManager : public Object
{
  public:
    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId();

    CreditManager();
    ~CreditManager() override;

    // Delete copy constructor and assignment operator
    CreditManager(const CreditManager&) = delete;
    CreditManager& operator=(const CreditManager&) = delete;

    /**
     * @brief Initialize credits for a virtual channel
     * @param vcId Virtual channel ID
     * @param numCredits Initial number of credits
     */
    void InitializeVc(uint8_t vcId, uint32_t numCredits);

    /**
     * @brief Check if a VC has available credits
     * @param vcId Virtual channel ID
     * @return true if credits available
     */
    bool HasCredits(uint8_t vcId) const;

    /**
     * @brief Get available credits for a VC
     * @param vcId Virtual channel ID
     * @return number of available credits
     */
    uint32_t GetAvailableCredits(uint8_t vcId) const;

    /**
     * @brief Consume a credit from a VC
     * @param vcId Virtual channel ID
     * @param seqNum Sequence number of the packet being sent
     * @return true if credit was consumed successfully
     */
    bool ConsumeCredit(uint8_t vcId, uint32_t seqNum);

    /**
     * @brief Return credits (received from remote endpoint)
     * @param vcId Virtual channel ID
     * @param numCredits Number of credits to return
     */
    void ReturnCredits(uint8_t vcId, uint32_t numCredits);

    /**
     * @brief Set the total credits for a VC
     * @param vcId Virtual channel ID
     * @param credits Total credits
     */
    void SetTotalCredits(uint8_t vcId, uint32_t credits);

    /**
     * @brief Get the number of initialized VCs
     * @return number of VCs
     */
    uint32_t GetNumVcs() const;

    /**
     * @brief Callback type for credit availability notification
     */
    typedef Callback<void, uint8_t> CreditAvailableCallback;

    /**
     * @brief Set callback for credit availability notification
     * @param cb Callback to invoke when credits become available
     */
    void SetCreditAvailableCallback(CreditAvailableCallback cb);

    /**
     * @brief Traced callback for credit changes
     */
    typedef TracedCallback<uint8_t, uint32_t, uint32_t> CreditChangeTracedCallback;

  private:
    void DoDispose() override;

    std::unordered_map<uint8_t, VcCreditInfo> m_vcCredits; ///< Per-VC credit info
    CreditAvailableCallback m_creditAvailableCallback;     ///< Callback for credit availability
    uint8_t m_maxVcs;                                      ///< Maximum number of VCs

    // Traced sources
    TracedCallback<uint8_t, uint32_t, uint32_t> m_creditChangeTrace; ///< VC, old, new credits
};

} // namespace ns3

#endif /* CREDIT_MANAGER_H */
