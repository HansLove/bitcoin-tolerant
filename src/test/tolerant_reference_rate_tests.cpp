// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Unit tests for the recent-block reference rate tracker (design §2).

#include <node/tolerant_reference_rate.h>

#include <consensus/validation.h>
#include <key.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <primitives/block.h>
#include <script/script.h>
#include <script/sign.h>
#include <test/util/setup_common.h>
#include <undo.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(tolerant_reference_rate_tests)

namespace {
const CFeeRate FLOOR{1000}; // 1 sat/vB, e.g. minrelayfee.

void FeedFlatMarket(TolerantReferenceRateTracker& t, CAmount sat_per_vb, int64_t vbytes_per_block, int blocks)
{
    for (int i = 0; i < blocks; ++i) {
        t.AddBlock(sat_per_vb * vbytes_per_block, vbytes_per_block);
    }
}
} // namespace

// ---------------------------------------------------------------------------
// Bootstrap guard: unreliable before min_reliable_blocks (design §2).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(unreliable_until_warm)
{
    TolerantReferenceRateTracker t;
    BOOST_CHECK(!t.IsReliable());
    for (int i = 0; i < 5; ++i) {
        t.AddBlock(10000, 1000); // 10 sat/vB
        BOOST_CHECK(!t.IsReliable());
    }
    t.AddBlock(10000, 1000);
    BOOST_CHECK_EQUAL(t.BlocksObserved(), 6);
    BOOST_CHECK(t.IsReliable());
}

// ---------------------------------------------------------------------------
// A flat market converges to its own rate.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(flat_market_converges)
{
    TolerantReferenceRateTracker t;
    FeedFlatMarket(t, /*sat_per_vb=*/10, /*vbytes_per_block=*/100000, /*blocks=*/30);
    const CFeeRate rate = t.GetReferenceRate(FLOOR);
    // 10 sat/vB == 10000 sat/kvB; allow small rounding tolerance.
    BOOST_CHECK_CLOSE(static_cast<double>(rate.GetFeePerK()), 10000.0, 1.0);
}

// ---------------------------------------------------------------------------
// The floor always applies -- an idle chain never reports below it.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(floor_always_applies)
{
    TolerantReferenceRateTracker t;
    // Blocks paying far below the floor (e.g. only coinbase, ~0 fees).
    FeedFlatMarket(t, /*sat_per_vb=*/0, /*vbytes_per_block=*/100000, /*blocks=*/20);
    BOOST_CHECK(t.GetReferenceRate(FLOOR) == FLOOR);
}

BOOST_AUTO_TEST_CASE(no_blocks_returns_floor)
{
    TolerantReferenceRateTracker t;
    BOOST_CHECK(t.GetReferenceRate(FLOOR) == FLOOR);
}

// ---------------------------------------------------------------------------
// The three climates from the worked examples: spike > calm > drought,
// and the rate tracks the *current* regime, not stale history.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(spike_calm_drought)
{
    TolerantReferenceRateTracker calm;
    FeedFlatMarket(calm, 10, 100000, 20);
    const CAmount calm_rate = calm.GetReferenceRate(FLOOR).GetFeePerK();
    BOOST_CHECK_CLOSE(static_cast<double>(calm_rate), 10000.0, 0.1); // sanity: converged

    // A sustained regime change dominates within a handful of half-lives,
    // but is not instantaneous -- that gradualness is the whole point of
    // decay weighting (a single block never swings the rate outright).
    TolerantReferenceRateTracker spike = calm; // same 20-block calm history
    FeedFlatMarket(spike, 66, 100000, 20);      // ~3.3 half-lives of a spike
    const CAmount spike_rate = spike.GetReferenceRate(FLOOR).GetFeePerK();
    BOOST_CHECK_GT(spike_rate, calm_rate);
    // Already most of the way to the new regime (10 -> 66): >90% converged.
    BOOST_CHECK_GT(spike_rate, 60000);
    BOOST_CHECK_LT(spike_rate, 66000);

    TolerantReferenceRateTracker drought = calm;
    FeedFlatMarket(drought, 1, 100000, 20);     // ~3.3 half-lives of a drought
    const CAmount drought_rate = drought.GetReferenceRate(FLOOR).GetFeePerK();
    BOOST_CHECK_LT(drought_rate, calm_rate);
    BOOST_CHECK_LT(drought_rate, 2000);
    BOOST_CHECK_GT(drought_rate, 1000);

    // After ~6.7 half-lives (40 blocks), the old calm regime is forgotten
    // (<1% residual weight) and the rate has essentially fully converged.
    TolerantReferenceRateTracker spike_converged = calm;
    FeedFlatMarket(spike_converged, 66, 100000, 40);
    BOOST_CHECK_CLOSE(static_cast<double>(spike_converged.GetReferenceRate(FLOOR).GetFeePerK()),
                      66000.0, 2.0);

    // The drought target (1 sat/vB) is small, so a tiny absolute residual
    // from the old 10 sat/vB regime reads as a large relative error --
    // check absolute closeness here instead of a percentage.
    TolerantReferenceRateTracker drought_converged = calm;
    FeedFlatMarket(drought_converged, 1, 100000, 40);
    const CAmount drought_converged_rate = drought_converged.GetReferenceRate(FLOOR).GetFeePerK();
    BOOST_CHECK_LT(drought_converged_rate, 1150); // within ~0.15 sat/vB of the 1 sat/vB target
}

// ---------------------------------------------------------------------------
// A single anomalous block (empty, or miner-stuffed) is diluted, not decisive.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(single_anomalous_block_is_diluted)
{
    TolerantReferenceRateTracker baseline;
    FeedFlatMarket(baseline, 10, 100000, 20);
    const CAmount baseline_rate = baseline.GetReferenceRate(FLOOR).GetFeePerK();

    TolerantReferenceRateTracker with_anomaly = baseline; // same history
    with_anomaly.AddBlock(0, 100000); // one empty/zero-fee block
    const CAmount after_rate = with_anomaly.GetReferenceRate(FLOOR).GetFeePerK();

    // The rate moves, but nowhere near down to the anomalous block's own rate.
    BOOST_CHECK_LT(after_rate, baseline_rate);
    BOOST_CHECK_GT(after_rate, baseline_rate / 2);
}
BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// End-to-end: a real connected block (mined via TestChain100Setup) produces
// exactly the expected (fee, vbytes) totals, and the chain monitor feeds
// them into the tracker correctly. This is the piece that matters most: the
// reference rate is read from CONFIRMED blocks via undo data, never from the
// local mempool -- see the "filtered-lens problem" in the module comment.
// ---------------------------------------------------------------------------
BOOST_FIXTURE_TEST_SUITE(tolerant_reference_rate_chain_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(real_connected_block_totals)
{
    const CScript scriptPubKey = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    // Spend the first mature coinbase, paying a known, exact fee.
    const CAmount coinbase_value = m_coinbase_txns[0]->vout[0].nValue;
    const CAmount pay_out = coinbase_value - 5000; // fee = 5000 sat, exactly.

    CMutableTransaction spend;
    spend.vin.resize(1);
    spend.vin[0].prevout = COutPoint(m_coinbase_txns[0]->GetHash(), 0);
    spend.vout.resize(1);
    spend.vout[0].nValue = pay_out;
    spend.vout[0].scriptPubKey = scriptPubKey;

    std::vector<unsigned char> vchSig;
    const uint256 hash = SignatureHash(scriptPubKey, spend, 0, SIGHASH_ALL, 0, SigVersion::BASE);
    BOOST_REQUIRE(coinbaseKey.Sign(hash, vchSig));
    vchSig.push_back(static_cast<unsigned char>(SIGHASH_ALL));
    spend.vin[0].scriptSig << vchSig;

    const CTransaction spend_tx{spend};
    const CBlock block = CreateAndProcessBlock({spend}, scriptPubKey);

    const CBlockIndex* tip = WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip());
    BOOST_REQUIRE(tip);
    BOOST_REQUIRE_EQUAL(tip->GetBlockHash(), block.GetHash());

    CBlockUndo undo;
    BOOST_REQUIRE(m_node.chainman->m_blockman.ReadBlockUndo(undo, *tip));

    const TolerantBlockTotals totals = ComputeBlockFeeTotals(block, undo);
    BOOST_CHECK_EQUAL(totals.total_fees, 5000);

    int64_t expected_vbytes{0};
    for (const auto& tx : block.vtx) expected_vbytes += GetVirtualTransactionSize(*tx);
    BOOST_CHECK_EQUAL(totals.total_vbytes, expected_vbytes);

    // The chain monitor wires this into a live tracker exactly the same way.
    TolerantReferenceRateTracker tracker;
    TolerantChainMonitor monitor(*m_node.chainman, *m_node.mempool, tracker);
    monitor.BlockConnected(ChainstateRole::NORMAL, std::make_shared<const CBlock>(block), tip);

    BOOST_CHECK_EQUAL(monitor.BlocksObserved(), 1);
    const CFeeRate floor{1000};
    const CFeeRate expected_rate(totals.total_fees, static_cast<uint32_t>(totals.total_vbytes));
    // CFeeRate's own equality compares its internal exact fraction (fee/size),
    // not the rounded sat/kvB display value -- and the tracker necessarily
    // stores an already-rounded scalar (a decayed running average has no
    // single exact source fraction to preserve). Compare the representable
    // value instead of exact-fraction identity.
    BOOST_CHECK_EQUAL(monitor.GetReferenceRate().GetFeePerK(),
                      std::max(expected_rate.GetFeePerK(), floor.GetFeePerK()));
}

namespace {
//! A spend of `prevout` with a realistic-size dummy scriptSig. EvaluateTransaction
//! never validates signatures -- it only measures -- so a real signature is
//! unnecessary; only the size matters.
CMutableTransaction SpendOf(const COutPoint& prevout, CAmount value_out, const CScript& spk)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(prevout, CScript() << std::vector<unsigned char>(72, 0x30));
    tx.vout.emplace_back(value_out, spk);
    return tx;
}
} // namespace

BOOST_AUTO_TEST_CASE(evaluate_transaction_against_real_chain)
{
    const CScript spk = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    TolerantReferenceRateTracker tracker;
    TolerantChainMonitor monitor(*m_node.chainman, *m_node.mempool, tracker);

    // Evaluation only needs the prevout to be unspent, not mature; use a
    // coinbase the block-mining loop below never touches.
    const COutPoint coin{m_coinbase_txns[50]->GetHash(), 0};
    const CAmount coin_value = m_coinbase_txns[50]->vout[0].nValue;

    // Monetary spend: economic size == real size, no premium (design §1).
    {
        const CTransaction tx{SpendOf(coin, coin_value - 1000, spk)};
        const auto v = monitor.EvaluateTransaction(tx, 1000);
        BOOST_REQUIRE(v);
        BOOST_CHECK(v->analysis.classification == TolerantDataClass::MONETARY);
        BOOST_CHECK_EQUAL(v->analysis.economic_vbytes, v->analysis.tx_vbytes);
        // Fresh tracker: no verdict may be drawn yet.
        BOOST_CHECK(!v->reference_reliable);
        BOOST_CHECK_EQUAL(std::string{v->Verdict()}, "warming-up");
    }

    // OP_RETURN payload: already full price at consensus, never double charged.
    CMutableTransaction op_return = SpendOf(coin, coin_value - 1000, spk);
    op_return.vout.emplace_back(0, CScript() << OP_RETURN << std::vector<unsigned char>(200, 0x07));
    const CTransaction op_return_tx{op_return};
    {
        const auto v = monitor.EvaluateTransaction(op_return_tx, 1000);
        BOOST_REQUIRE(v);
        BOOST_CHECK(v->analysis.classification == TolerantDataClass::OP_RETURN_DATA);
        BOOST_CHECK_EQUAL(v->analysis.economic_vbytes, v->analysis.tx_vbytes);
    }

    // Warm the tracker with real connected blocks carrying a real fee market.
    // Block 101+i spends the coinbase at height i+1: exactly 100 confirmations.
    for (int i = 0; i < 6; ++i) {
        const CTransaction paying{SpendOf(COutPoint{m_coinbase_txns[i]->GetHash(), 0},
                                          m_coinbase_txns[i]->vout[0].nValue - 20000, spk)};
        CMutableTransaction signed_tx{paying};
        signed_tx.vin[0].scriptSig = CScript();
        std::vector<unsigned char> sig;
        const uint256 hash = SignatureHash(spk, signed_tx, 0, SIGHASH_ALL, 0, SigVersion::BASE);
        BOOST_REQUIRE(coinbaseKey.Sign(hash, sig));
        sig.push_back(static_cast<unsigned char>(SIGHASH_ALL));
        signed_tx.vin[0].scriptSig << sig;
        const CBlock block = CreateAndProcessBlock({signed_tx}, spk);
        const CBlockIndex* tip = WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE_EQUAL(tip->GetBlockHash(), block.GetHash());
        monitor.BlockConnected(ChainstateRole::NORMAL, std::make_shared<const CBlock>(block), tip);
    }
    BOOST_CHECK(monitor.IsReferenceReliable());
    const CFeeRate reference = monitor.GetReferenceRate();

    // Paying well below the realized market rate is now a real verdict...
    {
        const auto v = monitor.EvaluateTransaction(op_return_tx, 100);
        BOOST_REQUIRE(v);
        BOOST_CHECK(v->reference_reliable);
        BOOST_CHECK_EQUAL(std::string{v->Verdict()}, "hidden-subsidy");
    }
    // ...and paying the honest required fee clears it -- data is priced, never banned.
    {
        const auto probe = monitor.EvaluateTransaction(op_return_tx, 0);
        BOOST_REQUIRE(probe);
        const auto v = monitor.EvaluateTransaction(op_return_tx, probe->pricing.required_fee);
        BOOST_REQUIRE(v);
        BOOST_CHECK_EQUAL(std::string{v->Verdict()}, "honest");
        BOOST_CHECK(v->pricing.eligible_for_template);
        BOOST_CHECK(v->reference_rate == reference);
    }

    // An unknown prevout yields no verdict at all -- we never guess.
    {
        const CTransaction orphan{SpendOf(COutPoint{Txid::FromUint256(uint256::ONE), 0}, 1000, spk)};
        BOOST_CHECK(!monitor.EvaluateTransaction(orphan, 1000));
    }
}

BOOST_AUTO_TEST_SUITE_END()
