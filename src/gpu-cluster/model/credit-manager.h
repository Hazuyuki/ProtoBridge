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
     * @brief Get the currently installed credit-availability callback.
     *
     * Identity-comparison support for the ownership-checked detach: an
     * owner detaches only if the callback still equals the one IT
     * installed (a later re-binding by another owner is untouched).
     */
    CreditAvailableCallback GetCreditAvailableCallback() const;

    /**
     * @brief Detach the credit-available callback from its endpoint owner.
     *
     * Called by FabricEndpoint::DoDispose: the manager can outlive the
     * endpoint when an external holder keeps the (attribute-supplied)
     * reference, and its m_creditAvailableCallback then still points at
     * the freed endpoint — a ReturnCredits after endpoint disposal fires
     * OnCreditAvailable into freed memory (codex round 18, finding 1).
     *
     * OWNERSHIP-CHECKED (codex round 19, findings 1 and 7): only the
     * callback THIS owner installed is cleared. Replacing the endpoint's
     * CreditManager attribute re-binds the OLD manager's callback to
     * nothing while the NEW manager keeps its own binding — a blind null
     * would drop the new owner's notifications (and swapping the
     * attribute without re-initializing left the old manager's callback
     * bound to the endpoint, so disposal detached the WRONG manager).
     * \param owner The callback the detaching owner installed; detached
     *              only on an identity match (a null callback detaches
     *              unconditionally for plain cleanup callers).
     */
    void DetachFromEndpoint(CreditAvailableCallback owner = CreditAvailableCallback());

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
