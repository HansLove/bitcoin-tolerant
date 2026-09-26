// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/tolerant_reference_rate.h>

#include <coins.h>
#include <kernel/mempool_entry.h> // NewMempoolTransactionInfo
#include <logging.h>
#include <node/blockstorage.h>
#include <policy/policy.h>        // GetVirtualTransactionSize
#include <primitives/block.h>
#include <txmempool.h>
#include <undo.h>
#include <validation.h>

#include <cmath>

TolerantReferenceRateTracker::TolerantReferenceRateTracker(int64_t half_life_blocks, int64_t min_reliable_blocks)
    : m_half_life_blocks(half_life_blocks),
      m_decay_per_block(std::pow(0.5, 1.0 / static_cast<double>(half_life_blocks))),
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

const char* TolerantMempoolVerdict::Verdict() const
{
    if (!reference_reliable) return "warming-up";
    return pricing.hidden_subsidy ? "hidden-subsidy" : "honest";
}

TolerantChainMonitor::TolerantChainMonitor(ChainstateManager& chainman, const CTxMemPool& mempool,
                                           TolerantReferenceRateTracker& tracker)
    : m_chainman(chainman), m_mempool(mempool), m_tracker(tracker)
{
}

void TolerantChainMonitor::BlockConnected(const kernel::ChainstateRole& role, const std::shared_ptr<const CBlock>& block,
                                          const CBlockIndex* pindex)
{
    if (!block || !pindex) return;
    // Background (assumeutxo) validation replays historical blocks out of
    // order relative to the tip; only the chainstate that follows the tip
    // describes the current fee market.
    if (role.historical) return;

    CBlockUndo undo;
    if (!m_chainman.m_blockman.ReadBlockUndo(undo, *pindex)) {
        // No undo data (e.g. pruned, or the genesis block, which has no
        // spendable inputs and never carries fee-market data). Skip rather
        // than guess.
        return;
    }

    const TolerantBlockTotals totals = ComputeBlockFeeTotals(*block, undo);
    CFeeRate rate;
    int64_t observed;
    bool reliable;
    {
        LOCK(m_mutex);
        m_tracker.AddBlock(totals.total_fees, totals.total_vbytes);
        rate = m_tracker.GetReferenceRate(m_mempool.m_opts.min_relay_feerate);
        observed = m_tracker.BlocksObserved();
        reliable = m_tracker.IsReliable();
    }
    LogDebug(BCLog::TOLERANT, "[TolerantV2] block %s height=%d fees=%d vbytes=%d -> reference_rate=%s (blocks_observed=%d%s)\n",
             pindex->GetBlockHash().ToString(), pindex->nHeight, totals.total_fees, totals.total_vbytes,
             rate.ToString(FeeRateFormat::SAT_VB), observed, reliable ? "" : ", warming up");
}

void TolerantChainMonitor::TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t mempool_sequence)
{
    // Zero cost unless someone is going to read the result.
    if (!util::log::ShouldDebugLog(BCLog::TOLERANT)) return;

    const auto verdict = EvaluateTransaction(*tx.info.m_tx, tx.info.m_fee);
    if (!verdict) {
        LogDebug(BCLog::TOLERANT, "[TolerantV2] tx %s skipped: prevouts no longer available\n",
                 tx.info.m_tx->GetHash().ToString());
        return;
    }
    // A purely monetary transaction has economic == normal size by
    // construction (tested invariant); logging each one is pure noise.
    if (verdict->analysis.classification == TolerantDataClass::MONETARY) return;

    const auto& a = verdict->analysis;
    const auto& r = verdict->pricing;
    LogDebug(BCLog::TOLERANT, "[TolerantV2] tx %s class=%s vbytes=%d economic_vbytes=%d fee=%d "
             "normal_feerate=%s as_if_feerate=%s reference_rate=%s required_fee=%d verdict=%s\n",
             tx.info.m_tx->GetHash().ToString(), TolerantDataClassToString(a.classification),
             a.tx_vbytes, a.economic_vbytes, r.actual_fee,
             r.normal_feerate.ToString(FeeRateFormat::SAT_VB), r.as_if_feerate.ToString(FeeRateFormat::SAT_VB),
             verdict->reference_rate.ToString(FeeRateFormat::SAT_VB), r.required_fee, verdict->Verdict());
}

std::optional<TolerantMempoolVerdict> TolerantChainMonitor::EvaluateTransaction(const CTransaction& tx, CAmount fee) const
{
    TolerantMempoolVerdict v;
    {
        LOCK(m_mutex);
        v.reference_rate = m_tracker.GetReferenceRate(m_mempool.m_opts.min_relay_feerate);
        v.reference_reliable = m_tracker.IsReliable();
    }

    TolerantPremiums premiums;
    premiums.dust_relay_fee = m_mempool.m_opts.dust_relay_feerate;

    {
        LOCK2(::cs_main, m_mempool.cs);
        CCoinsViewMemPool mempool_view(&m_chainman.ActiveChainstate().CoinsTip(), m_mempool);
        CCoinsViewCache view(&mempool_view);
        for (const CTxIn& txin : tx.vin) {
            if (!view.HaveCoin(txin.prevout)) return std::nullopt;
        }
        v.analysis = AnalyzeTolerantTx(tx, view, premiums);
    }
    v.pricing = ComputeTolerantPricing(v.analysis, fee, v.reference_rate);
    return v;
}

TolerantReferenceSnapshot TolerantChainMonitor::GetSnapshot() const
{
    TolerantReferenceSnapshot snap;
    snap.floor = m_mempool.m_opts.min_relay_feerate;
    LOCK(m_mutex);
    snap.reference_rate = m_tracker.GetReferenceRate(snap.floor);
    snap.blocks_observed = m_tracker.BlocksObserved();
    snap.half_life_blocks = m_tracker.HalfLifeBlocks();
    snap.reliable = m_tracker.IsReliable();
    return snap;
}

CFeeRate TolerantChainMonitor::GetReferenceRate() const
{
    LOCK(m_mutex);
    return m_tracker.GetReferenceRate(m_mempool.m_opts.min_relay_feerate);
}

bool TolerantChainMonitor::IsReferenceReliable() const
{
    LOCK(m_mutex);
    return m_tracker.IsReliable();
}

int64_t TolerantChainMonitor::BlocksObserved() const
{
    LOCK(m_mutex);
    return m_tracker.BlocksObserved();
}
