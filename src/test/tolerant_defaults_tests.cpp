// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Tests for Bitcoin Tolerant's defaults layer (doc/tolerant-upstream.md).

#include <node/tolerant_defaults.h>

#include <common/args.h>

#include <array>
#include <string>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(tolerant_defaults_tests)

namespace {
constexpr std::array<TolerantDefaultOverride, 2> SAMPLE{{
    {"-tolerant-test-a", "1", "test fixture"},
    {"-tolerant-test-b", "two", "test fixture"},
}};
} // namespace

BOOST_AUTO_TEST_CASE(not_applied_when_tolerant_disabled)
{
    // -tolerantv2=0 must leave every Bitcoin Core default untouched.
    ArgsManager args;
    BOOST_CHECK(ApplyTolerantDefaults(args, SAMPLE).empty());
    BOOST_CHECK(!args.IsArgSet("-tolerant-test-a"));

    args.ForceSetArg("-tolerantv2", "0");
    BOOST_CHECK(ApplyTolerantDefaults(args, SAMPLE).empty());
    BOOST_CHECK(!args.IsArgSet("-tolerant-test-b"));
}

BOOST_AUTO_TEST_CASE(applied_when_tolerant_enabled)
{
    ArgsManager args;
    args.ForceSetArg("-tolerantv2", "1");
    const auto applied = ApplyTolerantDefaults(args, SAMPLE);
    BOOST_REQUIRE_EQUAL(applied.size(), 2U);
    BOOST_CHECK_EQUAL(applied[0], "-tolerant-test-a=1");
    BOOST_CHECK_EQUAL(args.GetArg("-tolerant-test-b", ""), "two");
}

BOOST_AUTO_TEST_CASE(operator_value_always_wins)
{
    // A Tolerant default is a position, never an imposition.
    ArgsManager args;
    args.ForceSetArg("-tolerantv2", "1");
    args.ForceSetArg("-tolerant-test-a", "operator");
    const auto applied = ApplyTolerantDefaults(args, SAMPLE);
    BOOST_REQUIRE_EQUAL(applied.size(), 1U);
    BOOST_CHECK_EQUAL(applied[0], "-tolerant-test-b=two");
    BOOST_CHECK_EQUAL(args.GetArg("-tolerant-test-a", ""), "operator");
}

BOOST_AUTO_TEST_CASE(every_override_is_justified)
{
    // An override without a written reason is not accepted.
    for (const auto& o : TolerantDefaultOverrides()) {
        BOOST_CHECK_MESSAGE(o.arg && o.arg[0] == '-', "override arg must start with '-'");
        BOOST_CHECK_MESSAGE(o.value != nullptr, "override for " << o.arg << " has no value");
        BOOST_CHECK_MESSAGE(o.reason && std::string{o.reason}.size() >= 10,
                            "override for " << o.arg << " needs a written reason");
    }
}

BOOST_AUTO_TEST_SUITE_END()
