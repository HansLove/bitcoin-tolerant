// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/tolerant_reference_rate.h>

#include <node/blockstorage.h>
#include <policy/policy.h>       // GetVirtualTransactionSize
#include <primitives/block.h>
#include <undo.h>

#include <cmath>

TolerantReferenceRateTracker::TolerantReferenceRateTracker(int64_t half_life_blocks, int64_t min_reliable_blocks)
    : m_decay_per_block(std::pow(0.5, 1.0 / static_cast<double>(half_life_blocks))),
      m_min_reliable_blocks(min_reliable_blocks)
{
}

void TolerantReferenceRateTracker::AddBlock(CAmount total_fees, int64_t total_vbytes)
{
    // Decay everything accumulated so far by one block's worth of age, then
    // add the new block at full (undecayed) weight.
    m_weighted_fees = m_weighted_fees * m_decay_per_block + static_cast<double>(total_fees);
    m_weighted_vbytes = m_weighted_vbytes * m_decay_per_block + static_cast<double>(total_vbytes);
    ++m_blocks_observed;
}

CFeeRate TolerantReferenceRateTracker::GetReferenceRate(const CFeeRate& floor) const
{
    if (m_blocks_observed == 0 || m_weighted_vbytes <= 0.0) return floor;

    const double rate_sat_per_vb = m_weighted_fees / m_weighted_vbytes;
    const double rate_sat_per_kvb = rate_sat_per_vb * 1000.0;
    if (rate_sat_per_kvb <= 0.0) return floor;

    const CFeeRate observed{static_cast<CAmount>(std::llround(rate_sat_per_kvb))};
    return observed > floor ? observed : floor;
}

TolerantBlockTotals ComputeBlockFeeTotals(const CBlock& block, const CBlockUndo& undo)
{
    TolerantBlockTotals totals;

    for (const auto& tx : block.vtx) {
        totals.total_vbytes += GetVirtualTransactionSize(*tx);
    }

    // block.vtx[0] is the coinbase (no fee); undo.vtxundo[i] corresponds to
    // block.vtx[i + 1]. Iterate only as far as both are defined -- defensive
    // against a size mismatch rather than crashing a validation-interface
    // callback on malformed/unexpected input.
    const size_t non_coinbase = block.vtx.empty() ? 0 : block.vtx.size() - 1;
    const size_t n = std::min(non_coinbase, undo.vtxundo.size());
    for (size_t i = 0; i < n; ++i) {
        const CTransaction& tx = *block.vtx[i + 1];
        const CTxUndo& tx_undo = undo.vtxundo[i];

        CAmount value_in{0};
        const size_t inputs = std::min(tx.vin.size(), tx_undo.vprevout.size());
        for (size_t j = 0; j < inputs; ++j) {
            value_in += tx_undo.vprevout[j].out.nValue;
        }
        CAmount value_out{0};
        for (const CTxOut& out : tx.vout) {
            value_out += out.nValue;
        }
        totals.total_fees += (value_in - value_out);
    }

    return totals;
}

TolerantChainMonitor::TolerantChainMonitor(node::BlockManager& blockman, TolerantReferenceRateTracker& tracker)
    : m_blockman(blockman), m_tracker(tracker)
{
}

void TolerantChainMonitor::BlockConnected(const kernel::ChainstateRole& role, const std::shared_ptr<const CBlock>& block,
                                          const CBlockIndex* pindex)
{
    if (!block || !pindex) return;
    // Only the background (assumeutxo) validation chainstate and the normal
    // chainstate call this; either way it reflects a real connected block on
    // some chain tip. We price whatever we're told was connected.
    (void)role;

    CBlockUndo undo;
    if (!m_blockman.ReadBlockUndo(undo, *pindex)) {
        // Undo data unavailable (e.g. pruned, or genesis block has none to
        // read in the first place -- genesis has no spendable inputs and
        // its coinbase is unspendable by consensus, so it never contributes
        // real fee-market data regardless). Skip rather than guess.
        return;
    }

    const TolerantBlockTotals totals = ComputeBlockFeeTotals(*block, undo);
    m_tracker.AddBlock(totals.total_fees, totals.total_vbytes);
}
