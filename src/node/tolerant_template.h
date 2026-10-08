// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Bitcoin Tolerant V2, Phase 2: honest pricing in local block templates.
//
// Bitcoin Core builds templates from cluster-mempool chunks, ordered by
// feerate on their *discounted* size. A chunk carrying witness data therefore
// ranks higher than the block space it takes would justify. This module
// checks each chunk Core is about to include:
//
//   uses_discount = economic_vbytes > vbytes        (witness data present)
//   hidden_subsidy = uses_discount && fee / economic_vbytes < reference_rate
//
// In MARKET mode a hidden-subsidy chunk is skipped (with the rest of its
// cluster, exactly as Core skips a chunk that doesn't fit). In OBSERVE mode
// nothing changes; the would-be exclusions are counted and logged.
//
// Never touched: payments and OP_RETURN (they pay full weight), chunks that
// pay the reference rate on their true size, and anything while the
// reference rate is still warming up. Only local templates are affected:
// externally mined blocks are validated and accepted exactly as in Core.
// See doc/tolerant-v2-pricing.md, Phase 2.

#ifndef BITCOIN_NODE_TOLERANT_TEMPLATE_H
#define BITCOIN_NODE_TOLERANT_TEMPLATE_H

#include <coins.h>
#include <consensus/amount.h>
#include <kernel/mempool_entry.h>
#include <node/tolerant_pricing.h>
#include <policy/feerate.h>
#include <txmempool.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

class Chainstate;
class TolerantChainMonitor;

enum class TolerantPricingMode {
    OBSERVE, //!< Templates unchanged; would-be exclusions are counted and logged.
    MARKET,  //!< Hidden-subsidy chunks are left out of local templates.
};
static constexpr TolerantPricingMode DEFAULT_TOLERANT_PRICING_MODE{TolerantPricingMode::OBSERVE};

std::optional<TolerantPricingMode> ParseTolerantPricingMode(std::string_view mode);
const char* TolerantPricingModeToString(TolerantPricingMode mode);

/** What one template build did. */
struct TolerantTemplateStats {
    int height{0};
    TolerantPricingMode mode{DEFAULT_TOLERANT_PRICING_MODE};
    bool reference_reliable{false};
    CFeeRate reference_rate;
    int chunks_flagged{0};   //!< Hidden-subsidy chunks seen.
    int txs_flagged{0};
    CAmount fees_flagged{0}; //!< Their fees: what excluding them gives up.
    bool applied{false};     //!< True if the flagged chunks were excluded.
};

/** Pricing for one honest-chunk evaluation. Pure. */
struct TolerantChunkVerdict {
    CAmount fee{0};
    int64_t vbytes{0};
    int64_t economic_vbytes{0};
    CFeeRate as_if_feerate;
    bool uses_discount{false};
    bool hidden_subsidy{false};
};

/** Price a chunk as one unit: its transactions' analyses and (modified)
 *  fees. Pure; no chain state. */
TolerantChunkVerdict EvaluateTolerantChunk(const std::vector<std::pair<TolerantTxAnalysis, CAmount>>& txs,
                                           const CFeeRate& reference_rate);

/** Lives for one BlockAssembler::addChunks() call. Built and used with
 *  cs_main and the mempool lock held (as addChunks already holds them);
 *  reports its stats to the monitor when destroyed. */
class TolerantTemplatePricer
{
public:
    TolerantTemplatePricer(TolerantChainMonitor& monitor, Chainstate& chainstate,
                           const CTxMemPool& mempool, int height);
    ~TolerantTemplatePricer();

    TolerantTemplatePricer(const TolerantTemplatePricer&) = delete;
    TolerantTemplatePricer& operator=(const TolerantTemplatePricer&) = delete;

    //! Whether Core may include this chunk. False only in MARKET mode, with a
    //! reliable reference rate, for a hidden-subsidy chunk.
    bool AllowChunk(const std::vector<CTxMemPoolEntry::CTxMemPoolEntryRef>& chunk);

private:
    TolerantChainMonitor& m_monitor;
    const CTxMemPool& m_mempool;
    CCoinsViewMemPool m_mempool_view;
    CCoinsViewCache m_view;
    TolerantPremiums m_premiums;
    TolerantTemplateStats m_stats;
};

#endif // BITCOIN_NODE_TOLERANT_TEMPLATE_H
