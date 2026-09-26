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
#include <policy/feerate.h>
#include <validationinterface.h>

#include <cstdint>
#include <memory>

class CBlock;
class CBlockIndex;
class CBlockUndo;
namespace node {
class BlockManager;
} // namespace node

//! Default for -tolerantv2. Off by default: with this unset, the node's
//! behavior is byte-for-byte identical to plain Bitcoin Core.
static constexpr bool DEFAULT_TOLERANT_V2{false};

class TolerantReferenceRateTracker
{
public:
    static constexpr int64_t DEFAULT_HALF_LIFE_BLOCKS{6};
    //! Below this many observed blocks, the rate is not yet reliable enough
    //! for market-priced enforcement (design §2's bootstrap guard).
    static constexpr int64_t DEFAULT_MIN_RELIABLE_BLOCKS{6};

    explicit TolerantReferenceRateTracker(int64_t half_life_blocks = DEFAULT_HALF_LIFE_BLOCKS,
                                          int64_t min_reliable_blocks = DEFAULT_MIN_RELIABLE_BLOCKS);

    //! Feed one newly-connected block's realized totals into the tracker.
    void AddBlock(CAmount total_fees, int64_t total_vbytes);

    //! Number of blocks observed since construction (does not decay).
    int64_t BlocksObserved() const { return m_blocks_observed; }

    //! False until at least `min_reliable_blocks` have been observed --
    //! market-priced enforcement must not act on the rate before this.
    bool IsReliable() const { return m_blocks_observed >= m_min_reliable_blocks; }

    //! The current reference rate, floored at `floor` (typically minrelayfee).
    //! Returns `floor` unconditionally if no blocks have been observed yet.
    CFeeRate GetReferenceRate(const CFeeRate& floor) const;

private:
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

/** Bridges real connected blocks into a TolerantReferenceRateTracker.
 *
 *  BlockDisconnected is intentionally NOT overridden (a no-op): exponential
 *  decay cannot be undone exactly without storing full block history, and
 *  Phase 1 does not enforce anything on the reference rate -- nothing reads
 *  it to reject a transaction yet. A short reorg therefore causes only a
 *  small, transient, economically negligible skew, not a correctness bug.
 *  Revisit this if/when a later phase enforces on the rate; see
 *  doc/tolerant-v2-pricing.md. */
class TolerantChainMonitor : public CValidationInterface
{
public:
    TolerantChainMonitor(node::BlockManager& blockman, TolerantReferenceRateTracker& tracker);

    void BlockConnected(ChainstateRole role, const std::shared_ptr<const CBlock>& block,
                        const CBlockIndex* pindex) override;

private:
    node::BlockManager& m_blockman;
    TolerantReferenceRateTracker& m_tracker;
};

#endif // BITCOIN_NODE_TOLERANT_REFERENCE_RATE_H
