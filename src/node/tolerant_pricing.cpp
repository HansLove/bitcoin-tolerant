// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/tolerant_pricing.h>

#include <consensus/consensus.h>   // WITNESS_SCALE_FACTOR
#include <consensus/validation.h>  // GetTransactionWeight
#include <node/tolerant_datacarrier.h>
#include <policy/policy.h>         // IsDust, GetVirtualTransactionSize
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
//! Witness and taproot envelopes are not currently distinguished (both are
//! witness-location data, priced identically); see TolerantDataClass::TAPROOT_DATA.
TolerantDataClass ClassifyTx(int64_t op_return_bytes, int64_t witness_like_bytes,
                             int64_t dust_outputs, const TolerantPremiums& premiums)
{
    const int64_t total_data = op_return_bytes + witness_like_bytes;

    if (total_data == 0) {
        if (dust_outputs >= premiums.utxo_bloat_dust_count) return TolerantDataClass::UTXO_BLOAT;
        return TolerantDataClass::MONETARY;
    }
    if (total_data <= premiums.small_commitment_bytes) {
        return TolerantDataClass::SMALL_COMMITMENT;
    }
    const bool op_return_significant = op_return_bytes > premiums.small_commitment_bytes;
    const bool witness_significant = witness_like_bytes > premiums.small_commitment_bytes;
    if (op_return_significant && witness_significant) return TolerantDataClass::MIXED;
    if (witness_like_bytes >= op_return_bytes) return TolerantDataClass::WITNESS_DATA;
    return TolerantDataClass::OP_RETURN_DATA;
}
} // namespace

TolerantTxAnalysis AnalyzeTolerantTx(const CTransaction& tx, const CCoinsViewCache& view,
                                     const TolerantPremiums& premiums)
{
    TolerantTxAnalysis a;

    a.tx_weight = GetTransactionWeight(tx);
    a.tx_vbytes = GetVirtualTransactionSize(tx);

    const TolerantDatacarrierBytes dcb = ComputeTxDatacarrierBytes(tx, view);
    a.op_return_bytes = static_cast<int64_t>(dcb.op_return_bytes);
    // "witness_bytes" here means "data that benefits from today's witness
    // discount" -- the rare base_envelope case (a legacy, non-witness input
    // carrying an envelope pattern) already pays full price, so it is folded
    // into the reporting bucket alongside op_return rather than witness_bytes.
    a.witness_bytes = static_cast<int64_t>(dcb.witness_envelope_bytes);
    const int64_t already_full_price_data =
        static_cast<int64_t>(dcb.op_return_bytes + dcb.base_envelope_bytes);

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

    a.classification = ClassifyTx(already_full_price_data, a.witness_bytes, a.dust_outputs, premiums);

    // Multipliers. Monetary/small-commitment transactions carry no data
    // bytes, so the premium multiplies a zero base and never touches
    // monetary transactions (§1 invariant).
    a.permanence_multiplier = premiums.permanence;
    a.externality_multiplier = premiums.externality;
    a.data_premium_multiplier = premiums.permanence * premiums.externality;

    // Economic size: charge the honest per-byte rate to each data byte,
    // relative to the consensus weight it already pays today.
    //   - Witness-location data pays 1 WU/byte today (the segwit discount).
    //   - Base-location data (outputs, legacy scriptSig) already pays 4 WU/byte.
    // At the default premium (1.0) the target rate is exactly 4 WU/byte, so
    // witness data gains +3 WU/byte (discount removed) and base data gains
    // nothing (it was never discounted -- no double charge, design §6).
    const double target_wu_per_byte = static_cast<double>(WITNESS_SCALE_FACTOR) * a.data_premium_multiplier;
    int64_t extra_weight{0};
    if (target_wu_per_byte > 1.0) {
        extra_weight += std::llround(static_cast<double>(dcb.witness_envelope_bytes) * (target_wu_per_byte - 1.0));
    }
    if (target_wu_per_byte > static_cast<double>(WITNESS_SCALE_FACTOR)) {
        extra_weight += std::llround(static_cast<double>(already_full_price_data) *
                                     (target_wu_per_byte - WITNESS_SCALE_FACTOR));
    }
    const int64_t economic_weight = a.tx_weight + extra_weight;
    a.economic_vbytes = GetVirtualTransactionSize(economic_weight, /*nSigOpCost=*/0, /*bytes_per_sigop=*/0);

    // Honest data footprint (the premium base, in vbytes): every data byte at
    // 1 vB/B times the premium. base = economic - data holds by construction,
    // so a monetary tx has data_vbytes == 0 and base == economic_vbytes.
    const int64_t total_data_bytes = already_full_price_data + a.witness_bytes;
    a.data_vbytes = std::llround(static_cast<double>(total_data_bytes) * a.data_premium_multiplier);
    a.base_monetary_vbytes = std::max<int64_t>(0, a.economic_vbytes - a.data_vbytes);

    // Observability heuristic: arbitrary-data density (data bytes per vbyte).
    a.data_score = a.tx_vbytes > 0
        ? static_cast<double>(total_data_bytes) / static_cast<double>(a.tx_vbytes)
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
