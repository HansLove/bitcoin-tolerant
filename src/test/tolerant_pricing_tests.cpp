// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Unit tests for the Bitcoin Tolerant V2 honest-pricing core.
// See doc/tolerant-v2-pricing.md. These tests pin the invariants that keep
// honest pricing from decaying back into content filtering.

#include <node/tolerant_pricing.h>

#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>

#include <vector>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(tolerant_pricing_tests, BasicTestingSetup)

namespace {

const CScript P2WPKH_SCRIPT = CScript() << OP_0 << std::vector<unsigned char>(20, 0x02);
constexpr CAmount ABOVE_DUST{100000};
constexpr CAmount DUST_VALUE{1};

//! Fund `spk` with a synthetic coin and return an input spending it.
CTxIn FundInput(CCoinsViewCache& coins, const CScript& spk, uint32_t n = 0)
{
    CMutableTransaction funding;
    funding.vin.resize(1);
    funding.vout.resize(n + 1);
    for (uint32_t i = 0; i <= n; ++i) {
        funding.vout[i].nValue = 10 * COIN;
        funding.vout[i].scriptPubKey = spk;
    }
    const CTransaction funding_tx{funding};
    AddCoins(coins, funding_tx, 0);
    return CTxIn(COutPoint(funding_tx.GetHash(), n));
}

//! A taproot (witness v1) output script: OP_1 <32-byte program>.
CScript TaprootSpk()
{
    return CScript() << OP_1 << std::vector<unsigned char>(WITNESS_V1_TAPROOT_SIZE, 0x03);
}

//! An inscription-style tapscript: OP_FALSE OP_IF <payload> OP_ENDIF.
//! The envelope never executes, but the bytes are stored forever.
CScript InscriptionEnvelope(size_t payload_bytes)
{
    return CScript() << OP_FALSE << OP_IF
                     << std::vector<unsigned char>(payload_bytes, 0x42)
                     << OP_ENDIF;
}

//! A plain monetary transaction: one input, `outputs` spendable outputs.
CMutableTransaction MonetaryTx(CCoinsViewCache& coins, size_t outputs = 2,
                               CAmount value = ABOVE_DUST)
{
    CMutableTransaction tx;
    tx.vin.push_back(FundInput(coins, P2WPKH_SCRIPT));
    for (size_t i = 0; i < outputs; ++i) {
        tx.vout.emplace_back(value, P2WPKH_SCRIPT);
    }
    return tx;
}

} // namespace

// ---------------------------------------------------------------------------
// The §1 invariant: monetary bytes are never premium-multiplied.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(monetary_tx_pays_no_premium)
{
    CCoinsViewCache coins(&CoinsViewEmpty::Get());
    const CTransaction tx{MonetaryTx(coins)};

    const TolerantTxAnalysis a = AnalyzeTolerantTx(tx, coins);

    BOOST_CHECK(a.classification == TolerantDataClass::MONETARY);
    BOOST_CHECK_EQUAL(a.op_return_bytes, 0);
    BOOST_CHECK_EQUAL(a.witness_bytes, 0);
    BOOST_CHECK_EQUAL(a.data_vbytes, 0);
    // economic == normal, so a payment is priced exactly as it is today.
    BOOST_CHECK_EQUAL(a.economic_vbytes, a.tx_vbytes);
    BOOST_CHECK_EQUAL(a.base_monetary_vbytes, a.economic_vbytes);

    // ...and therefore as-if feerate == normal feerate.
    const CFeeRate reference{10000}; // 10 sat/vB
    const CAmount fee = reference.GetFee(static_cast<uint32_t>(a.tx_vbytes));
    const TolerantPricingResult r = ComputeTolerantPricing(a, fee, reference);
    BOOST_CHECK(r.as_if_feerate == r.normal_feerate);
    BOOST_CHECK(!r.hidden_subsidy);
    BOOST_CHECK(r.eligible_for_template);
}

// ---------------------------------------------------------------------------
// Witness data loses the segwit discount: this is the whole point of V2.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(witness_payload_loses_the_discount)
{
    CCoinsViewCache coins(&CoinsViewEmpty::Get());

    constexpr size_t kPayload = 1000;
    CMutableTransaction mtx;
    mtx.vin.push_back(FundInput(coins, TaprootSpk()));
    // Taproot script-path witness: {tapscript, control block}.
    const CScript envelope = InscriptionEnvelope(kPayload);
    mtx.vin[0].scriptWitness.stack.emplace_back(envelope.begin(), envelope.end());
    mtx.vin[0].scriptWitness.stack.emplace_back(33, 0xc0); // control block
    mtx.vout.emplace_back(ABOVE_DUST, P2WPKH_SCRIPT);

    const CTransaction tx{mtx};
    const TolerantTxAnalysis a = AnalyzeTolerantTx(tx, coins);

    BOOST_CHECK(a.classification == TolerantDataClass::WITNESS_DATA);
    // The envelope is detected and at least the payload is counted as data.
    BOOST_CHECK_GE(a.witness_bytes, static_cast<int64_t>(kPayload));
    BOOST_CHECK_EQUAL(a.op_return_bytes, 0);

    // Witness data sits at 1 WU/byte by consensus; honest pricing charges
    // 4 WU/byte, i.e. +3 WU per data byte. Verify exactly.
    const int64_t expected = (a.tx_weight + a.witness_bytes * 3 + 3) / 4;
    BOOST_CHECK_EQUAL(a.economic_vbytes, expected);
    BOOST_CHECK_GT(a.economic_vbytes, a.tx_vbytes);

    // The as-if feerate exposes what the normal feerate hides: paying "market
    // rate" on the discounted size is really paying well below market.
    const CFeeRate reference{10000}; // 10 sat/vB
    const CAmount discounted_fee = reference.GetFee(static_cast<uint32_t>(a.tx_vbytes));
    const TolerantPricingResult r = ComputeTolerantPricing(a, discounted_fee, reference);
    BOOST_CHECK(r.normal_feerate >= reference); // looks like it paid market
    BOOST_CHECK(r.as_if_feerate < reference);   // but it did not
    BOOST_CHECK(r.hidden_subsidy);
    BOOST_CHECK(!r.eligible_for_template);

    // Paying the honest price clears the bar — data is priced, never banned.
    const TolerantPricingResult honest = ComputeTolerantPricing(a, r.required_fee, reference);
    BOOST_CHECK(honest.eligible_for_template);
    BOOST_CHECK(!honest.hidden_subsidy);
}

// ---------------------------------------------------------------------------
// OP_RETURN bytes are already full price; the default premium must not
// double-charge them (design §1).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(op_return_is_not_double_charged)
{
    CCoinsViewCache coins(&CoinsViewEmpty::Get());

    CMutableTransaction mtx;
    mtx.vin.push_back(FundInput(coins, P2WPKH_SCRIPT));
    mtx.vout.emplace_back(ABOVE_DUST, P2WPKH_SCRIPT);
    mtx.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>(200, 0x07));

    const CTransaction tx{mtx};
    const TolerantTxAnalysis a = AnalyzeTolerantTx(tx, coins);

    BOOST_CHECK(a.classification == TolerantDataClass::OP_RETURN_DATA);
    BOOST_CHECK_EQUAL(a.op_return_count, 1);
    BOOST_CHECK_GE(a.op_return_bytes, 200);
    // Output data already costs 1 vB/B at consensus, so at the default premium
    // the economic size must equal the real size: no extra charge.
    BOOST_CHECK_EQUAL(a.economic_vbytes, a.tx_vbytes);
}

BOOST_AUTO_TEST_CASE(small_commitment_is_not_data_heavy)
{
    CCoinsViewCache coins(&CoinsViewEmpty::Get());

    CMutableTransaction mtx;
    mtx.vin.push_back(FundInput(coins, P2WPKH_SCRIPT));
    mtx.vout.emplace_back(ABOVE_DUST, P2WPKH_SCRIPT);
    mtx.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>(20, 0x09));

    const CTransaction tx{mtx};
    const TolerantTxAnalysis a = AnalyzeTolerantTx(tx, coins);

    BOOST_CHECK(a.classification == TolerantDataClass::SMALL_COMMITMENT);
    BOOST_CHECK_EQUAL(a.economic_vbytes, a.tx_vbytes);
}

// ---------------------------------------------------------------------------
// The load-bearing test for the §3 decision: batching is the most
// block-space-efficient behavior in Bitcoin and must never be penalized.
// This fails the moment anyone reintroduces an output-count premium.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(payment_batching_is_not_utxo_bloat)
{
    CCoinsViewCache coins(&CoinsViewEmpty::Get());
    const CTransaction tx{MonetaryTx(coins, /*outputs=*/100, ABOVE_DUST)};

    const TolerantTxAnalysis a = AnalyzeTolerantTx(tx, coins);

    BOOST_CHECK_EQUAL(a.output_count, 100);
    BOOST_CHECK_EQUAL(a.dust_outputs, 0);
    BOOST_CHECK(a.classification != TolerantDataClass::UTXO_BLOAT);
    BOOST_CHECK(a.classification == TolerantDataClass::MONETARY);
    // A large batch pays exactly its real size — no premium whatsoever.
    BOOST_CHECK_EQUAL(a.economic_vbytes, a.tx_vbytes);
    BOOST_CHECK_EQUAL(a.data_vbytes, 0);
}

BOOST_AUTO_TEST_CASE(dust_outputs_flagged_as_utxo_bloat)
{
    CCoinsViewCache coins(&CoinsViewEmpty::Get());
    const CTransaction tx{MonetaryTx(coins, /*outputs=*/5, DUST_VALUE)};

    const TolerantTxAnalysis a = AnalyzeTolerantTx(tx, coins);

    BOOST_CHECK_EQUAL(a.dust_outputs, 5);
    BOOST_CHECK(a.classification == TolerantDataClass::UTXO_BLOAT);
}

// ---------------------------------------------------------------------------
// Pricing arithmetic and the reference rate.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(required_fee_tracks_reference_rate)
{
    CCoinsViewCache coins(&CoinsViewEmpty::Get());

    constexpr size_t kPayload = 2000;
    CMutableTransaction mtx;
    mtx.vin.push_back(FundInput(coins, TaprootSpk()));
    const CScript envelope = InscriptionEnvelope(kPayload);
    mtx.vin[0].scriptWitness.stack.emplace_back(envelope.begin(), envelope.end());
    mtx.vin[0].scriptWitness.stack.emplace_back(33, 0xc0);
    mtx.vout.emplace_back(ABOVE_DUST, P2WPKH_SCRIPT);

    const CTransaction tx{mtx};
    const TolerantTxAnalysis a = AnalyzeTolerantTx(tx, coins);

    // Calm market: 10 sat/vB.
    const CFeeRate calm{10000};
    const TolerantPricingResult r_calm = ComputeTolerantPricing(a, 0, calm);
    BOOST_CHECK_EQUAL(r_calm.required_fee,
                      calm.GetFee(static_cast<uint32_t>(a.economic_vbytes)));

    // Fee spike: the honest bar rises with the market, so data cannot sneak in
    // cheap exactly when block space is dearest (design §2).
    const CFeeRate spike{66000}; // 66 sat/vB
    const TolerantPricingResult r_spike = ComputeTolerantPricing(a, 0, spike);
    BOOST_CHECK_GT(r_spike.required_fee, r_calm.required_fee);

    // Drought: nothing is displaced, so the honest price falls with the market.
    // Tolerant must not exploit a quiet market to punish data.
    const CFeeRate drought{1000}; // 1 sat/vB
    const TolerantPricingResult r_drought = ComputeTolerantPricing(a, 0, drought);
    BOOST_CHECK_LT(r_drought.required_fee, r_calm.required_fee);

    // A fee that is a subsidy during the spike is honest during the drought.
    const TolerantPricingResult paid_low_in_drought =
        ComputeTolerantPricing(a, r_drought.required_fee, drought);
    BOOST_CHECK(paid_low_in_drought.eligible_for_template);
    BOOST_CHECK(!paid_low_in_drought.hidden_subsidy);

    const TolerantPricingResult same_fee_in_spike =
        ComputeTolerantPricing(a, r_drought.required_fee, spike);
    BOOST_CHECK(!same_fee_in_spike.eligible_for_template);
    BOOST_CHECK(same_fee_in_spike.hidden_subsidy);
}

// ---------------------------------------------------------------------------
// Operator premiums scale the data portion only.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(premiums_scale_data_not_monetary)
{
    CCoinsViewCache coins(&CoinsViewEmpty::Get());

    TolerantPremiums doubled;
    doubled.permanence = 2.0;

    // A monetary transaction is untouched no matter how high the premium.
    {
        const CTransaction tx{MonetaryTx(coins)};
        const TolerantTxAnalysis base = AnalyzeTolerantTx(tx, coins);
        const TolerantTxAnalysis prem = AnalyzeTolerantTx(tx, coins, doubled);
        BOOST_CHECK_EQUAL(prem.economic_vbytes, base.economic_vbytes);
        BOOST_CHECK_EQUAL(prem.economic_vbytes, prem.tx_vbytes);
    }

    // A witness payload scales with the premium.
    {
        CMutableTransaction mtx;
        mtx.vin.push_back(FundInput(coins, TaprootSpk()));
        const CScript envelope = InscriptionEnvelope(1000);
        mtx.vin[0].scriptWitness.stack.emplace_back(envelope.begin(), envelope.end());
        mtx.vin[0].scriptWitness.stack.emplace_back(33, 0xc0);
        mtx.vout.emplace_back(ABOVE_DUST, P2WPKH_SCRIPT);

        const CTransaction tx{mtx};
        const TolerantTxAnalysis base = AnalyzeTolerantTx(tx, coins);
        const TolerantTxAnalysis prem = AnalyzeTolerantTx(tx, coins, doubled);
        BOOST_CHECK_GT(prem.economic_vbytes, base.economic_vbytes);
        BOOST_CHECK_EQUAL(prem.data_premium_multiplier, 2.0);
    }
}

// ---------------------------------------------------------------------------
// Cheap is not subsidized. Only the witness discount creates a subsidy;
// payments and OP_RETURN pay full weight however little they pay.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(cheap_full_weight_txs_are_not_subsidies)
{
    CCoinsViewCache coins(&CoinsViewEmpty::Get());
    const CFeeRate reference{66000}; // a fee spike: 66 sat/vB

    const CTransaction payment{MonetaryTx(coins)};
    const TolerantTxAnalysis pa = AnalyzeTolerantTx(payment, coins);
    const TolerantPricingResult pr = ComputeTolerantPricing(pa, 10, reference);
    BOOST_CHECK(pr.as_if_feerate < reference);
    BOOST_CHECK(!pr.hidden_subsidy);
    BOOST_CHECK(pr.eligible_for_template);

    CMutableTransaction mtx;
    mtx.vin.push_back(FundInput(coins, P2WPKH_SCRIPT));
    mtx.vout.emplace_back(ABOVE_DUST, P2WPKH_SCRIPT);
    mtx.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>(200, 0x07));
    const CTransaction op_return{mtx};
    const TolerantTxAnalysis oa = AnalyzeTolerantTx(op_return, coins);
    const TolerantPricingResult orr = ComputeTolerantPricing(oa, 10, reference);
    BOOST_CHECK(orr.as_if_feerate < reference);
    BOOST_CHECK(!orr.hidden_subsidy);
    BOOST_CHECK(orr.eligible_for_template);
}

BOOST_AUTO_TEST_SUITE_END()
