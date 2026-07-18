// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Bitcoin Tolerant V2 — Honest blockspace pricing (Phase 1: observe-only core).
//
// This module contains PURE analysis and pricing functions. It never mutates
// global state, never touches consensus validation, and never rejects a
// transaction on its own. It answers one question about a transaction:
//
//   Is it paying the real economic cost of the block space it consumes?
//
// See doc/tolerant-v2-pricing.md for the full design. Guiding invariants:
//   - A purely monetary transaction has economic_vbytes == tx_vbytes, so its
//     as-if feerate equals its normal feerate (no premium on monetary bytes).
//   - Data is priced, never filtered by content. Classification only selects a
//     permanence multiplier; no class is ever rejected for being its class.
//   - economic_vbytes reuses the existing CalculateExtraTxWeight() accounting,
//     so V2 with default (1.0) premiums is byte-for-byte consistent with the
//     node's configured -datacarriercost base.

#ifndef BITCOIN_NODE_TOLERANT_PRICING_H
#define BITCOIN_NODE_TOLERANT_PRICING_H

#include <consensus/amount.h>
#include <policy/feerate.h>
#include <policy/policy.h> // for DUST_RELAY_TX_FEE

#include <cstdint>
#include <string>

class CCoinsViewCache;
class CTransaction;

/** Descriptive classification of a transaction's dominant space usage.
 *  Descriptive only: it selects a permanence multiplier, nothing else. */
enum class TolerantDataClass {
    MONETARY,          //!< No detectable arbitrary data.
    SMALL_COMMITMENT,  //!< Data present, at or below the small-commitment threshold.
    OP_RETURN_DATA,    //!< Output-side (provably unspendable) payload dominates.
    WITNESS_DATA,      //!< Input/witness/script-path payload dominates.
    TAPROOT_DATA,      //!< Taproot script-path payload, where safely detectable.
    UTXO_BLOAT,        //!< Dust-output dominated; burdens active state.
    MIXED,             //!< Multiple data sources above the small-commitment threshold.
};

const char* TolerantDataClassToString(TolerantDataClass c);

/** Operator-configured inputs to the analysis. All default to the neutral,
 *  no-extra-premium position, so defaults reproduce current node behavior. */
struct TolerantPremiums {
    //! Multiplier for the durability of the burden. Default 1.0 (fair byte price).
    double permanence{1.0};
    //! Operator judgement for network externalities. Default 1.0 (no opinion).
    //! Never inferred or auto-tuned; see doc/tolerant-v2-pricing.md §4.
    double externality{1.0};
    //! Data at or below this many bytes is a SMALL_COMMITMENT (no premium).
    int64_t small_commitment_bytes{40};
    //! Number of dust outputs at or above which a tx is flagged UTXO_BLOAT.
    int64_t utxo_bloat_dust_count{2};
    //! Dust threshold reference, used to count dust outputs.
    CFeeRate dust_relay_fee{DUST_RELAY_TX_FEE};
};

/** Result of analysing a single transaction. Reporting-only; see the module
 *  comment for the base_monetary_vbytes + data_vbytes == economic_vbytes shape. */
struct TolerantTxAnalysis {
    TolerantDataClass classification{TolerantDataClass::MONETARY};
    int64_t tx_vbytes{0};
    int64_t tx_weight{0};
    int64_t economic_vbytes{0};      //!< base + data priced at the honest rate.
    int64_t base_monetary_vbytes{0}; //!< Never premium-multiplied.
    int64_t data_vbytes{0};          //!< The premium base (honest data footprint).
    int64_t op_return_bytes{0};      //!< Output-side datacarrier bytes.
    int64_t op_return_count{0};
    int64_t witness_bytes{0};        //!< Witness/script-path datacarrier bytes.
    int64_t taproot_payload_bytes{0};
    int64_t output_count{0};         //!< Scored only — never priced.
    int64_t dust_outputs{0};         //!< The only priceable UTXO-bloat signal.
    double data_score{0.0};          //!< Observability heuristic: data density.
    double permanence_multiplier{1.0};
    double externality_multiplier{1.0};
    double data_premium_multiplier{1.0};
};

/** Result of pricing an analysed transaction against a reference rate. */
struct TolerantPricingResult {
    CAmount actual_fee{0};
    CAmount required_fee{0};
    CFeeRate normal_feerate{};
    CFeeRate as_if_feerate{};
    int64_t economic_vbytes{0};
    bool eligible_for_template{true}; //!< Defaults open, not closed.
    bool hidden_subsidy{false};       //!< as_if_feerate < reference_rate.
    std::string reason;
};

/** Analyse a transaction: classify it and compute its honest economic size.
 *  Pure: reads only `tx`, `view` (for input prevouts) and `premiums`. */
TolerantTxAnalysis AnalyzeTolerantTx(const CTransaction& tx, const CCoinsViewCache& view,
                                     const TolerantPremiums& premiums = {});

/** Price an analysed transaction against a reference blockspace rate (§2).
 *  `reference_rate` is the realized average feerate of recent blocks, supplied
 *  by the caller; this function does not read chain state. Pure. */
TolerantPricingResult ComputeTolerantPricing(const TolerantTxAnalysis& analysis,
                                             CAmount actual_fee,
                                             const CFeeRate& reference_rate);

#endif // BITCOIN_NODE_TOLERANT_PRICING_H
