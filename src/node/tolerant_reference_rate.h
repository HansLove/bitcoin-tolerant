// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Bitcoin Tolerant V2 -- the recent-block reference rate (design §2).
//
//   reference_rate = total_fees(recent blocks) / total_vbytes(recent blocks)
//
// Recency weighting: a block's contribution decays exponentially with age,
// halving every `half_life_blocks` (default 6, ~1 hour). This is the fixed,
// static curve confirmed for V2 -- NOT an adaptive controller: the half-life
// itself never changes based on observed volatility. It is one honest,
// visible constant, exactly like the flat 6-block average it replaces, just
// smoother: recent blocks dominate, a single anomalous block is diluted
// rather than swinging the rate outright, and old data fades out instead of
// falling off a hard cliff.
//
// Why no explicit window buffer: true exponential decay is representable as
// a running accumulator (weighted_fees, weighted_vbytes) updated by a single
// multiply-and-add per block -- it never needs to store history. After
// roughly 2016 blocks (Bitcoin's own difficulty-retarget period -- chosen as
// the *outer bound* this decay is compared against, not a reset point; see
// doc/tolerant-v2-pricing.md §2) a block's weight is ~0.5^336, i.e.
// indistinguishable from zero. The 2016-block "ceiling" is a property of the
// half-life, not a second parameter to configure.
//
// The rate is sourced from CONFIRMED blocks (via undo data), never from the
// local mempool. This is deliberate: a mempool-derived measurement inherits
// whatever the node's own relay policy already filtered out, silently
// under-reporting real scarcity (the "filtered-lens problem" -- design §2).
// Reading confirmed blocks means the reference is a fact about the network,
// not a fact about this node's local policy.

#ifndef BITCOIN_NODE_TOLERANT_REFERENCE_RATE_H
#define BITCOIN_NODE_TOLERANT_REFERENCE_RATE_H

#include <consensus/amount.h>
#include <kernel/chain.h> // ChainstateRole
#include <kernel/cs_main.h>
#include <node/tolerant_pricing.h>
#include <policy/feerate.h>
#include <sync.h>
#include <validationinterface.h>

#include <cstdint>
#include <memory>
#include <optional>

class CBlock;
class CBlockIndex;
class CBlockUndo;
class CTxMemPool;
class ChainstateManager;
struct NewMempoolTransactionInfo;

//! Default for -tolerantv2. Off by default: with this unset, the node's
//! behavior is byte-for-byte identical to plain Bitcoin Core.
static constexpr bool DEFAULT_TOLERANT_V2{false};

class TolerantReferenceRateTracker
{
public:
    static constexpr int64_t DEFAULT_HALF_LIFE_BLOCKS{6};
    //! Upper bound for the half-life: Bitcoin's difficulty-retarget period,
    //! the memory ceiling the decay model is designed around.
    static constexpr int64_t MAX_HALF_LIFE_BLOCKS{2016};
    //! Below this many observed blocks, the rate is not yet reliable enough
    //! for market-priced enforcement (design §2's bootstrap guard).
    static constexpr int64_t DEFAULT_MIN_RELIABLE_BLOCKS{6};

    explicit TolerantReferenceRateTracker(int64_t half_life_blocks = DEFAULT_HALF_LIFE_BLOCKS,
                                          int64_t min_reliable_blocks = DEFAULT_MIN_RELIABLE_BLOCKS);

    //! Feed one newly-connected block's realized totals into the tracker.
    void AddBlock(CAmount total_fees, int64_t total_vbytes);

    int64_t HalfLifeBlocks() const { return m_half_life_blocks; }

    //! Number of blocks observed since construction (does not decay).
    int64_t BlocksObserved() const { return m_blocks_observed; }

    //! False until at least `min_reliable_blocks` have been observed --
    //! market-priced enforcement must not act on the rate before this.
    bool IsReliable() const { return m_blocks_observed >= m_min_reliable_blocks; }

    //! The current reference rate, floored at `floor` (typically minrelayfee).
    //! Returns `floor` unconditionally if no blocks have been observed yet.
    CFeeRate GetReferenceRate(const CFeeRate& floor) const;

private:
    int64_t m_half_life_blocks;
    double m_decay_per_block; //!< 0.5^(1/half_life_blocks), in (0, 1).
    double m_weighted_fees{0.0};
    double m_weighted_vbytes{0.0};
    int64_t m_blocks_observed{0};
    int64_t m_min_reliable_blocks;
};

/** A connected block's realized totals, as paid by the fee market. */
struct TolerantBlockTotals {
    CAmount total_fees{0};   //!< Sum of (inputs - outputs) over non-coinbase txs.
    int64_t total_vbytes{0}; //!< Sum of vsize over EVERY tx, including coinbase --
                             //!< this is the block-space budget the fees were paid
                             //!< against, matching design §2's denominator.
};

/** Compute a connected block's realized (fees, vbytes) totals from the block
 *  and its undo data. Pure: no chain-state access, no disk I/O -- the caller
 *  supplies `undo` (e.g. via node::BlockManager::ReadBlockUndo). `undo` must
 *  align with `block` (one CTxUndo per non-coinbase transaction, in order) --
 *  true for any undo data read for a block that is actually connected. */
TolerantBlockTotals ComputeBlockFeeTotals(const CBlock& block, const CBlockUndo& undo);

/** A consistent, single-lock view of the reference rate's state. */
struct TolerantReferenceSnapshot {
    CFeeRate reference_rate;
    CFeeRate floor;
    int64_t blocks_observed{0};
    int64_t half_life_blocks{0};
    bool reliable{false};
};

/** Result of evaluating one transaction against the live reference rate. */
struct TolerantMempoolVerdict {
    TolerantTxAnalysis analysis;
    TolerantPricingResult pricing;
    CFeeRate reference_rate;
    //! False while the tracker is still warming up (fewer than
    //! min_reliable_blocks observed): the pricing numbers are computed, but
    //! no subsidy verdict should be drawn from them yet.
    bool reference_reliable{false};
    //! "honest", "hidden-subsidy" or "warming-up".
    const char* Verdict() const;
};

/** Bitcoin Tolerant V2's live bridge into the node (observe-only).
 *
 *  - BlockConnected: reads the block's undo data and feeds its realized
 *    (fees, vbytes) into the reference-rate tracker.
 *  - TransactionAddedToMempool: when -debug=tolerant is on, analyses and
 *    prices the new transaction against the current reference rate and logs
 *    the verdict. When the category is off it returns immediately -- no
 *    locks, no analysis -- so enabling -tolerantv2 alone costs nothing per
 *    transaction.
 *
 *  Nothing here rejects, excludes or reorders anything. It only measures.
 *
 *  BlockDisconnected is intentionally NOT overridden: exponential decay
 *  cannot be undone exactly without storing full block history, and since
 *  nothing enforces on the reference rate yet, a short reorg causes only a
 *  small, transient, economically negligible skew. Revisit if a later phase
 *  ever enforces on the rate; see doc/tolerant-v2-pricing.md.
 *
 *  Thread safety: the tracker is only touched under m_mutex, so future
 *  readers (e.g. an RPC) must go through this class, not the tracker. */
class TolerantChainMonitor : public CValidationInterface
{
public:
    TolerantChainMonitor(ChainstateManager& chainman, const CTxMemPool& mempool,
                         TolerantReferenceRateTracker& tracker);

    void BlockConnected(ChainstateRole role, const std::shared_ptr<const CBlock>& block,
                        const CBlockIndex* pindex) override;
    void TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t mempool_sequence) override;

    //! Analyse and price `tx` (paying `fee`) against the current reference
    //! rate, resolving prevouts from the chain tip and the mempool. Returns
    //! nullopt if any input's prevout cannot be found (e.g. already spent by
    //! a block that confirmed the tx in the meantime) -- we never guess.
    //! Acquires cs_main and the mempool lock; call without holding either.
    std::optional<TolerantMempoolVerdict> EvaluateTransaction(const CTransaction& tx, CAmount fee) const
        EXCLUSIVE_LOCKS_REQUIRED(!::cs_main, !m_mutex);

    TolerantReferenceSnapshot GetSnapshot() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    CFeeRate GetReferenceRate() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool IsReferenceReliable() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    int64_t BlocksObserved() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    ChainstateManager& m_chainman;
    const CTxMemPool& m_mempool;
    mutable Mutex m_mutex;
    TolerantReferenceRateTracker& m_tracker GUARDED_BY(m_mutex);
};

#endif // BITCOIN_NODE_TOLERANT_REFERENCE_RATE_H
