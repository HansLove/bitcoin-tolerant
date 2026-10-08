// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/tolerant_defaults.h>

#include <common/args.h>
#include <node/tolerant_reference_rate.h> // DEFAULT_TOLERANT_V2

#include <array>

namespace {
// Each entry must name the principle it serves (doc/tolerant-upstream.md,
// "Principles") and be recorded in that document's decision log.
// Example shape, deliberately not active:
//   {"-someoption", "value", "Principle 2: price block space, don't filter it."},
constexpr std::array<TolerantDefaultOverride, 0> OVERRIDES{};
} // namespace

std::span<const TolerantDefaultOverride> TolerantDefaultOverrides()
{
    return OVERRIDES;
}

std::vector<std::string> ApplyTolerantDefaults(ArgsManager& args,
                                               std::span<const TolerantDefaultOverride> overrides)
{
    std::vector<std::string> applied;
    if (!args.GetBoolArg("-tolerantv2", DEFAULT_TOLERANT_V2)) return applied;
    for (const auto& o : overrides) {
        if (args.SoftSetArg(o.arg, o.value)) {
            applied.push_back(std::string{o.arg} + "=" + o.value);
        }
    }
    return applied;
}
