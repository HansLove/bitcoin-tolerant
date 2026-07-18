// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/tolerant_pricing.h>

#include <consensus/consensus.h>   // WITNESS_SCALE_FACTOR
#include <consensus/validation.h>  // GetTransactionWeight
#include <policy/policy.h>         // DatacarrierBytes, CalculateExtraTxWeight, IsDust, GetVirtualTransactionSize
#include <primitives/transaction.h>
#include <script/script.h>

#include <algorithm>
#include <cmath>

const char* TolerantDataClassToString(TolerantDataClass c)
{
    switch (c) {
    case TolerantDataClass::MONETARY:         return "MONETARY";
    case TolerantDataClass::SMALL_COMMITMENT: return "SMALL_COMMITMENT";
    case TolerantDataClass::OP_RETURN_DATA:   return "OP_RETURN_DATA";
    case TolerantDataClass::WITNESS_DATA:     return "WITNESS_DATA";
    case TolerantDataClass::TAPROOT_DATA:     return "TAPROOT_DATA";
    case TolerantDataClass::UTXO_BLOAT:       return "UTXO_BLOAT";
    case TolerantDataClass::MIXED:            return "MIXED";
    }
    return "UNKNOWN";
}

namespace {
//! Classify by dominant space usage. Descriptive only; never rejects.
//! Where witness vs taproot cannot be told apart safely, we return WITNESS_DATA
//! and never guess in a way that would raise a premium (see design §5).
TolerantDataClass ClassifyTx(int64_t op_return_bytes, int64_t witness_bytes,
                             int64_t dust_outputs, const TolerantPremiums& premiums)
{
    const int64_t total_data = op_return_bytes + witness_bytes;

    if (total_data == 0) {
        if (dust_outputs >= premiums.utxo_bloat_dust_count) return TolerantDataClass::UTXO_BLOAT;
        return TolerantDataClass::MONETARY;
    }
    if (total_data <= premiums.small_commitment_bytes) {
        return TolerantDataClass::SMALL_COMMITMENT;
    }
    const bool op_return_significant = op_return_bytes > premiums.small_commitment_bytes;
    const bool witness_significant = witness_bytes > premiums.small_commitment_bytes;
    if (op_return_significant && witness_significant) return TolerantDataClass::MIXED;
    if (witness_bytes >= op_return_bytes) return TolerantDataClass::WITNESS_DATA;
    return TolerantDataClass::OP_RETURN_DATA;
}
} // namespace

TolerantTxAnalysis AnalyzeTolerantTx(const CTransaction& tx, const CCoinsViewCache& view,
                                     const TolerantPremiums& premiums)
{
    TolerantTxAnalysis a;

    a.tx_weight = GetTransactionWeight(tx);
    a.tx_vbytes = GetVirtualTransactionSize(tx);

    // Reuse the node's existing datacarrier accounting rather than re-parsing.
    // .first  = output-side (OP_RETURN / provably-unspendable) datacarrier bytes.
    // .second = witness / script-path / inscription datacarrier bytes.
    const auto dcb = DatacarrierBytes(tx, view);
    a.op_return_bytes = static_cast<int64_t>(dcb.first);
    a.witness_bytes = static_cast<int64_t>(dcb.second);

    // Output-side descriptive counts (for scoring; output_count is never priced).
    a.output_count = static_cast<int64_t>(tx.vout.size());
    for (const CTxOut& txout : tx.vout) {
        if (!txout.scriptPubKey.empty() && txout.scriptPubKey[0] == OP_RETURN) {
            ++a.op_return_count;
        }
        if (IsDust(txout, premiums.dust_relay_fee)) {
            ++a.dust_outputs;
        }
    }

    a.classification = ClassifyTx(a.op_return_bytes, a.witness_bytes, a.dust_outputs, premiums);

    // Multipliers. Monetary/small-commitment carry no data bytes, so the premium
    // multiplies a zero base and never touches monetary transactions (§1 invariant).
    a.permanence_multiplier = premiums.permanence;
    a.externality_multiplier = premiums.externality;
    a.data_premium_multiplier = premiums.permanence * premiums.externality;

    // Economic size: convert the honest data footprint to weight using the same
    // machinery as -datacarriercost, expressed as weight-units per data byte.
    // Default premium (1.0) => WITNESS_SCALE_FACTOR (4 WU/B = 1 vB/B), i.e. data
    // pays full price with the witness discount removed, and OP_RETURN bytes
    // (already 1 vB/B) are unchanged. Quantized to whole WU, matching the
    // existing g_weight_per_data_byte fixed-point granularity.
    const double wpdb_d = static_cast<double>(WITNESS_SCALE_FACTOR) * a.data_premium_multiplier;
    const unsigned int weight_per_data_byte = static_cast<unsigned int>(
        std::max<int64_t>(1, std::llround(wpdb_d)));
    const int32_t extra_weight = CalculateExtraTxWeight(tx, view, weight_per_data_byte);
    const int64_t economic_weight = a.tx_weight + static_cast<int64_t>(extra_weight);
    a.economic_vbytes = GetVirtualTransactionSize(economic_weight, /*nSigOpCost=*/0, /*bytes_per_sigop=*/0);

    // Honest data footprint (the premium base): every data byte at 1 vB/B times
    // the premium. base = economic - data holds by construction, so a monetary
    // tx has data_vbytes == 0 and base_monetary_vbytes == economic_vbytes.
    a.data_vbytes = std::llround(static_cast<double>(a.op_return_bytes + a.witness_bytes) *
                                 a.data_premium_multiplier);
    a.base_monetary_vbytes = std::max<int64_t>(0, a.economic_vbytes - a.data_vbytes);

    // Observability heuristic: arbitrary-data density (data bytes per vbyte).
    a.data_score = a.tx_vbytes > 0
        ? static_cast<double>(a.op_return_bytes + a.witness_bytes) / static_cast<double>(a.tx_vbytes)
        : 0.0;

    return a;
}

TolerantPricingResult ComputeTolerantPricing(const TolerantTxAnalysis& analysis,
                                             CAmount actual_fee,
                                             const CFeeRate& reference_rate)
{
    TolerantPricingResult r;
    r.actual_fee = actual_fee;
    r.economic_vbytes = analysis.economic_vbytes;

    const uint32_t econ = static_cast<uint32_t>(std::max<int64_t>(1, analysis.economic_vbytes));
    const uint32_t normal = static_cast<uint32_t>(std::max<int64_t>(1, analysis.tx_vbytes));

    r.required_fee = reference_rate.GetFee(econ);
    r.normal_feerate = CFeeRate(actual_fee, normal);
    r.as_if_feerate = CFeeRate(actual_fee, econ);

    // A hidden subsidy is paying below what recent block space actually cost, on
    // the transaction's true (economic) footprint. Because reference_rate itself
    // falls toward the floor in a quiet market, this does not fire on ordinary
    // low-fee transactions during genuine slack periods (design §2, §8).
    r.hidden_subsidy = r.as_if_feerate < reference_rate;
    r.eligible_for_template = actual_fee >= r.required_fee;

    if (analysis.classification == TolerantDataClass::MONETARY) {
        r.reason = "monetary; economic == normal size";
    } else if (r.eligible_for_template) {
        r.reason = "pays honest economic rate";
    } else {
        r.reason = "insufficient as-if feerate for economic size";
    }
    return r;
}
