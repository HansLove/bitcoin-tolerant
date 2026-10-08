// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Unit tests for Phase 2 chunk pricing (node/tolerant_template.h).

#include <node/tolerant_template.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(tolerant_template_tests)

namespace {
const CFeeRate REFERENCE{10000}; // 10 sat/vB

std::pair<TolerantTxAnalysis, CAmount> Tx(int64_t vbytes, int64_t economic_vbytes, CAmount fee)
{
    TolerantTxAnalysis a;
    a.tx_vbytes = vbytes;
    a.economic_vbytes = economic_vbytes;
    return {a, fee};
}
} // namespace

BOOST_AUTO_TEST_CASE(parse_modes)
{
    BOOST_CHECK(ParseTolerantPricingMode("observe") == TolerantPricingMode::OBSERVE);
    BOOST_CHECK(ParseTolerantPricingMode("market") == TolerantPricingMode::MARKET);
    BOOST_CHECK(!ParseTolerantPricingMode("filter"));
    BOOST_CHECK(!ParseTolerantPricingMode(""));
    BOOST_CHECK(DEFAULT_TOLERANT_PRICING_MODE == TolerantPricingMode::OBSERVE);
}

BOOST_AUTO_TEST_CASE(cheap_payment_chunk_is_never_flagged)
{
    // 140 vB payment at 1 sat/vB during a 10 sat/vB market: cheap, not subsidized.
    const auto v = EvaluateTolerantChunk({Tx(140, 140, 140)}, REFERENCE);
    BOOST_CHECK(!v.uses_discount);
    BOOST_CHECK(!v.hidden_subsidy);
}

BOOST_AUTO_TEST_CASE(witness_data_chunk_below_reference_is_flagged)
{
    // 12,650 vB as Core counts it, 50,150 vB of real block space.
    const auto cheap = EvaluateTolerantChunk({Tx(12650, 50150, 126500)}, REFERENCE);
    BOOST_CHECK(cheap.uses_discount);
    BOOST_CHECK(cheap.hidden_subsidy);

    const auto honest = EvaluateTolerantChunk({Tx(12650, 50150, 501500)}, REFERENCE);
    BOOST_CHECK(honest.uses_discount);
    BOOST_CHECK(!honest.hidden_subsidy);
}

BOOST_AUTO_TEST_CASE(cpfp_child_can_pay_for_a_data_parent)
{
    // A cheap inscription parent alone is a subsidy...
    BOOST_CHECK(EvaluateTolerantChunk({Tx(500, 1250, 500)}, REFERENCE).hidden_subsidy);
    // ...but a child paying enough makes the chunk pay the honest price on
    // the chunk's whole economic size (1,250 + 140 vB at 10 sat/vB = 13,900).
    const auto rescued = EvaluateTolerantChunk({Tx(500, 1250, 500), Tx(140, 140, 13400)}, REFERENCE);
    BOOST_CHECK_EQUAL(rescued.economic_vbytes, 1390);
    BOOST_CHECK_EQUAL(rescued.fee, 13900);
    BOOST_CHECK(!rescued.hidden_subsidy);
    const auto short_by_one = EvaluateTolerantChunk({Tx(500, 1250, 500), Tx(140, 140, 13399)}, REFERENCE);
    BOOST_CHECK(short_by_one.hidden_subsidy);
}

BOOST_AUTO_TEST_SUITE_END()
