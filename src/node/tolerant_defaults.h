// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Bitcoin Tolerant's defaults layer.
//
// When Bitcoin Tolerant disagrees with a Bitcoin Core *default*, it does not
// revert or patch Core's code. It records the override here, in one list,
// with the principle that justifies it. That keeps Core's code untouched (so
// upstream releases rebase cheaply) and keeps every divergence visible and
// reviewable in a single place. See doc/tolerant-upstream.md.
//
// Overrides are applied with SoftSetArg: a value the operator sets in
// bitcoin.conf or on the command line always wins. A Tolerant default is a
// position, never an imposition. They apply only with -tolerantv2=1; with it
// off, the node keeps Bitcoin Core's defaults exactly.

#ifndef BITCOIN_NODE_TOLERANT_DEFAULTS_H
#define BITCOIN_NODE_TOLERANT_DEFAULTS_H

#include <span>
#include <string>
#include <vector>

class ArgsManager;

struct TolerantDefaultOverride {
    //! The option, with its leading dash, e.g. "-datacarriersize".
    const char* arg;
    //! The value Bitcoin Tolerant uses when the operator sets none.
    const char* value;
    //! The Tolerant principle that justifies diverging from Core's default.
    //! Required: an override without a written reason is not accepted.
    const char* reason;
};

//! Bitcoin Core defaults that Bitcoin Tolerant overrides. Empty today: no
//! Core default currently contradicts Tolerant's principles.
std::span<const TolerantDefaultOverride> TolerantDefaultOverrides();

//! Apply `overrides` (soft: operator values always win) when -tolerantv2 is
//! enabled. Returns the overrides that actually took effect, as
//! "-arg=value" strings, so the caller can log them.
std::vector<std::string> ApplyTolerantDefaults(ArgsManager& args,
                                               std::span<const TolerantDefaultOverride> overrides = TolerantDefaultOverrides());

#endif // BITCOIN_NODE_TOLERANT_DEFAULTS_H
