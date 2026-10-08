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

#include <algorithm>
#include <cmath>
#include <vector>

TolerantReferenceRateTracker::TolerantReferenceRateTracker(int64_t half_life_blocks, int64_t min_reliable_blocks)
    : m_half_life_blocks(half_life_blocks),
      m_decay_per_block(std::pow(0.5, 1.0 / static_cast<double>(half_life_blocks))),
      m_window(static_cast<size_t>(std::max<int64_t>(MAX_HALF_LIFE_BLOCKS, 10 * half_life_blocks))),
      m_min_reliable_blocks(min_reliable_blocks)
{
}

bool TolerantReferenceRateTracker::AddBlock(int height, CAmount total_fees, int64_t total_vbytes)
{
    if (!m_history.empty() && height <= m_history.back().height) return false;
    m_history.push_back({height, total_fees, total_vbytes});
    while (m_history.size() > m_window) m_history.pop_front();
    Recompute();
    return true;
}

void TolerantReferenceRateTracker::AddBlock(CAmount total_fees, int64_t total_vbytes)
{
    AddBlock(m_history.empty() ? 0 : m_history.back().height + 1, total_fees, total_vbytes);
}

void TolerantReferenceRateTracker::RemoveBlocksFrom(int height)
{
    bool removed{false};
    while (!m_history.empty() && m_history.back().height >= height) {
        m_history.pop_back();
        removed = true;
    }
    if (removed) Recompute();
}

std::optional<int> TolerantReferenceRateTracker::TipHeight() const
{
    if (m_history.empty()) return std::nullopt;
    return m_history.back().height;
}

void TolerantReferenceRateTracker::Recompute()
{
    // Oldest first: decay everything accumulated so far by one block of age,
    // then add the next block at full weight.
    m_weighted_fees = 0.0;
    m_weighted_vbytes = 0.0;
    for (const Entry& e : m_history) {
        m_weighted_fees = m_weighted_fees * m_decay_per_block + static_cast<double>(e.fees);
        m_weighted_vbytes = m_weighted_vbytes * m_decay_per_block + static_cast<double>(e.vbytes);
    }
}

CFeeRate TolerantReferenceRateTracker::GetReferenceRate(const CFeeRate& floor) const
{
    if (m_history.empty() || m_weighted_vbytes <= 0.0) return floor;

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
                                           TolerantReferenceRateTracker& tracker, TolerantPricingMode mode)
    : m_chainman(chainman), m_mempool(mempool), m_mode(mode), m_tracker(tracker)
{
}

void TolerantChainMonitor::RecordTemplate(const TolerantTemplateStats& stats)
{
    LOCK(m_mutex);
    m_last_template = stats;
}

std::optional<TolerantTemplateStats> TolerantChainMonitor::LastTemplate() const
{
    LOCK(m_mutex);
    return m_last_template;
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
        // A duplicate (already backfilled at startup) is ignored.
        if (!m_tracker.AddBlock(pindex->nHeight, totals.total_fees, totals.total_vbytes)) return;
        rate = m_tracker.GetReferenceRate(m_mempool.m_opts.min_relay_feerate);
        observed = m_tracker.BlocksObserved();
        reliable = m_tracker.IsReliable();
    }
    LogDebug(BCLog::TOLERANT, "[TolerantV2] block %s height=%d fees=%d vbytes=%d -> reference_rate=%s (blocks_observed=%d%s)\n",
             pindex->GetBlockHash().ToString(), pindex->nHeight, totals.total_fees, totals.total_vbytes,
             rate.ToString(FeeRateFormat::SAT_VB), observed, reliable ? "" : ", warming up");
}

void TolerantChainMonitor::BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex)
{
    if (!pindex) return;
    CFeeRate rate;
    {
        LOCK(m_mutex);
        m_tracker.RemoveBlocksFrom(pindex->nHeight);
        rate = m_tracker.GetReferenceRate(m_mempool.m_opts.min_relay_feerate);
    }
    LogDebug(BCLog::TOLERANT, "[TolerantV2] block %s height=%d disconnected -> reference_rate=%s\n",
             pindex->GetBlockHash().ToString(), pindex->nHeight, rate.ToString(FeeRateFormat::SAT_VB));
}

int TolerantChainMonitor::Backfill(int depth)
{
    AssertLockHeld(::cs_main);
    const Chainstate& chainstate = m_chainman.ActiveChainstate();
    const CBlockIndex* tip = chainstate.m_chain.Tip();
    if (!tip || depth <= 0) return 0;

    // Oldest first, so the tracker receives heights in ascending order.
    std::vector<const CBlockIndex*> indexes;
    for (const CBlockIndex* index = tip; index && static_cast<int>(indexes.size()) < depth; index = index->pprev) {
        indexes.push_back(index);
    }
    std::reverse(indexes.begin(), indexes.end());

    int loaded{0};
    for (const CBlockIndex* index : indexes) {
        if (index->nHeight == 0) continue; // genesis: no spendable inputs, no undo data
        CBlock block;
        CBlockUndo undo;
        if (!m_chainman.m_blockman.ReadBlock(block, *index) || !m_chainman.m_blockman.ReadBlockUndo(undo, *index)) {
            // Pruned: older data is gone. Keep what we have; it is still the
            // most recent, contiguous history.
            continue;
        }
        const TolerantBlockTotals totals = ComputeBlockFeeTotals(block, undo);
        LOCK(m_mutex);
        if (m_tracker.AddBlock(index->nHeight, totals.total_fees, totals.total_vbytes)) ++loaded;
    }
    return loaded;
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
