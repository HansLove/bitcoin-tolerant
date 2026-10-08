// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/tolerant_template.h>

#include <logging.h>
#include <node/tolerant_reference_rate.h>
#include <policy/policy.h> // GetVirtualTransactionSize
#include <primitives/transaction.h>
#include <sync.h>
#include <validation.h>

std::optional<TolerantPricingMode> ParseTolerantPricingMode(std::string_view mode)
{
    if (mode == "observe") return TolerantPricingMode::OBSERVE;
    if (mode == "market") return TolerantPricingMode::MARKET;
    return std::nullopt;
}

const char* TolerantPricingModeToString(TolerantPricingMode mode)
{
    switch (mode) {
    case TolerantPricingMode::OBSERVE: return "observe";
    case TolerantPricingMode::MARKET:  return "market";
    }
    return "unknown";
}

TolerantChunkVerdict EvaluateTolerantChunk(const std::vector<std::pair<TolerantTxAnalysis, CAmount>>& txs,
                                           const CFeeRate& reference_rate)
{
    TolerantChunkVerdict v;
    for (const auto& [analysis, fee] : txs) {
        v.fee += fee;
        v.vbytes += analysis.tx_vbytes;
        v.economic_vbytes += analysis.economic_vbytes;
    }
    // A chunk is one unit to the miner: a child that pays for its parent
    // (CPFP) pays for both, so the chunk is priced as a whole.
    v.uses_discount = v.economic_vbytes > v.vbytes;
    v.as_if_feerate = CFeeRate(v.fee, static_cast<int32_t>(std::max<int64_t>(1, v.economic_vbytes)));
    v.hidden_subsidy = v.uses_discount && v.as_if_feerate < reference_rate;
    return v;
}

TolerantTemplatePricer::TolerantTemplatePricer(TolerantChainMonitor& monitor, Chainstate& chainstate,
                                               const CTxMemPool& mempool, int height)
    : m_monitor(monitor),
      m_mempool(mempool),
      m_mempool_view(&chainstate.CoinsTip(), mempool),
      m_view(&m_mempool_view)
{
    AssertLockHeld(::cs_main);
    AssertLockHeld(mempool.cs);
    const TolerantReferenceSnapshot snap = monitor.GetSnapshot();
    m_stats.height = height;
    m_stats.mode = monitor.Mode();
    m_stats.reference_reliable = snap.reliable;
    m_stats.reference_rate = snap.reference_rate;
    m_stats.applied = m_stats.mode == TolerantPricingMode::MARKET && snap.reliable;
    m_premiums.dust_relay_fee = mempool.m_opts.dust_relay_feerate;
}

TolerantTemplatePricer::~TolerantTemplatePricer()
{
    if (m_stats.chunks_flagged > 0) {
        LogDebug(BCLog::TOLERANT, "[TolerantV2] template height=%d mode=%s reference_rate=%s: %d hidden-subsidy chunks (%d txs, %d sat in fees) %s\n",
                 m_stats.height, TolerantPricingModeToString(m_stats.mode),
                 m_stats.reference_rate.ToString(FeeRateFormat::SAT_VB),
                 m_stats.chunks_flagged, m_stats.txs_flagged, m_stats.fees_flagged,
                 m_stats.applied ? "excluded" : "would be excluded in market mode");
    }
    m_monitor.RecordTemplate(m_stats);
}

bool TolerantTemplatePricer::AllowChunk(const std::vector<CTxMemPoolEntry::CTxMemPoolEntryRef>& chunk)
{
    AssertLockHeld(::cs_main);
    AssertLockHeld(m_mempool.cs);

    std::vector<std::pair<TolerantTxAnalysis, CAmount>> priced;
    priced.reserve(chunk.size());
    for (const auto& ref : chunk) {
        const CTxMemPoolEntry& entry = ref.get();
        const CTransaction& tx = entry.GetTx();
        TolerantTxAnalysis analysis;
        if (tx.HasWitness()) {
            analysis = AnalyzeTolerantTx(tx, m_view, m_premiums);
        } else {
            // No witness, no discount: economic size is real size.
            analysis.tx_vbytes = analysis.economic_vbytes = GetVirtualTransactionSize(tx);
        }
        // Modified fee: a prioritisetransaction delta is a payment the miner
        // received out of band, and it counts.
        priced.emplace_back(std::move(analysis), entry.GetModifiedFee());
    }

    const TolerantChunkVerdict v = EvaluateTolerantChunk(priced, m_stats.reference_rate);
    if (!v.hidden_subsidy) return true;

    ++m_stats.chunks_flagged;
    m_stats.txs_flagged += static_cast<int>(chunk.size());
    m_stats.fees_flagged += v.fee;
    LogDebug(BCLog::TOLERANT, "[TolerantV2] template height=%d chunk first_tx=%s txs=%d vbytes=%d economic_vbytes=%d fee=%d as_if_feerate=%s < reference_rate=%s -> %s\n",
             m_stats.height, chunk.front().get().GetTx().GetHash().ToString(), chunk.size(),
             v.vbytes, v.economic_vbytes, v.fee, v.as_if_feerate.ToString(FeeRateFormat::SAT_VB),
             m_stats.reference_rate.ToString(FeeRateFormat::SAT_VB),
             m_stats.applied ? "excluded" : "would be excluded");
    return !m_stats.applied;
}
