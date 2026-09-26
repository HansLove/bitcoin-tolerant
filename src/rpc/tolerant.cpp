// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Bitcoin Tolerant V2 -- gettolerantpricing (Phase 1e).
//
// The deliverable of V2 is a reference price for block space, not a filter.
// A price other operators, pools and analysts can compare against must be
// machine-readable, and it must report verdicts even when nothing is
// condemned. This RPC is read-only: it never changes node state or policy.
// See doc/tolerant-v2-pricing.md section 8.

#include <kernel/mempool_entry.h>
#include <node/context.h>
#include <node/tolerant_pricing.h>
#include <node/tolerant_reference_rate.h>
#include <policy/feerate.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <sync.h>
#include <txmempool.h>
#include <univalue.h>

#include <optional>
#include <string>

using node::NodeContext;

namespace {

//! Fee rates are reported in sat/vB, matching how the fee market talks.
UniValue SatPerVB(const CFeeRate& rate)
{
    return UniValue{static_cast<double>(rate.GetFeePerK()) / 1000.0};
}

const TolerantChainMonitor& EnsureTolerantMonitor(const NodeContext& node)
{
    if (!node.tolerant_chain_monitor) {
        throw JSONRPCError(RPC_MISC_ERROR,
            "Bitcoin Tolerant V2 is disabled. Start the node with -tolerantv2=1 to track the reference rate.");
    }
    return *node.tolerant_chain_monitor;
}

} // namespace

static RPCHelpMan gettolerantpricing()
{
    return RPCHelpMan{
        "gettolerantpricing",
        "Returns Bitcoin Tolerant V2's honest blockspace reference price and, optionally, how a mempool\n"
        "transaction is priced against it.\n"
        "\nThe reference rate is the recency-weighted fee rate of recently CONFIRMED blocks (half-life\n"
        "-tolerantreferenceblocks), floored at -minrelaytxfee. It is read from block undo data, never from\n"
        "the local mempool. Data bytes are priced at full weight (the witness discount removed); monetary\n"
        "bytes are never multiplied. Read-only: this never rejects, excludes or reorders anything.\n"
        "Requires -tolerantv2=1.\n",
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "A mempool transaction to price against the reference rate"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "reference_rate", "Current reference rate, in sat/vB"},
                {RPCResult::Type::NUM, "floor", "Floor under the reference rate (-minrelaytxfee), in sat/vB"},
                {RPCResult::Type::BOOL, "reliable", "False while warming up (too few blocks observed to draw verdicts)"},
                {RPCResult::Type::NUM, "blocks_observed", "Connected blocks fed into the tracker since startup"},
                {RPCResult::Type::NUM, "half_life_blocks", "Half-life of the recency weighting, in blocks"},
                {RPCResult::Type::OBJ, "tx", /*optional=*/true, "Only present if txid was given",
                {
                    {RPCResult::Type::STR_HEX, "txid", "The transaction id"},
                    {RPCResult::Type::STR, "classification", "MONETARY, SMALL_COMMITMENT, OP_RETURN_DATA, WITNESS_DATA, UTXO_BLOAT or MIXED"},
                    {RPCResult::Type::NUM, "vsize", "Virtual size, as consensus counts it (witness discounted)"},
                    {RPCResult::Type::NUM, "economic_vsize", "Virtual size with data bytes priced at full weight"},
                    {RPCResult::Type::NUM, "data_bytes", "Arbitrary-data bytes detected (OP_RETURN + script envelopes)"},
                    {RPCResult::Type::NUM, "witness_data_bytes", "Of which carried in witness locations (discounted today)"},
                    {RPCResult::Type::NUM, "output_count", "Number of outputs (scored only, never priced)"},
                    {RPCResult::Type::NUM, "dust_outputs", "Number of dust outputs"},
                    {RPCResult::Type::NUM, "fee", "Fee actually paid, in satoshis"},
                    {RPCResult::Type::NUM, "normal_feerate", "fee / vsize, in sat/vB"},
                    {RPCResult::Type::NUM, "as_if_feerate", "fee / economic_vsize, in sat/vB"},
                    {RPCResult::Type::NUM, "required_fee", "economic_vsize x reference_rate, in satoshis"},
                    {RPCResult::Type::BOOL, "hidden_subsidy", "True if as_if_feerate is below the reference rate"},
                    {RPCResult::Type::STR, "verdict", "honest, hidden-subsidy, or warming-up (no verdict drawn yet)"},
                }},
            }},
        RPCExamples{
            HelpExampleCli("gettolerantpricing", "") +
            HelpExampleCli("gettolerantpricing", "\"mytxid\"") +
            HelpExampleRpc("gettolerantpricing", "\"mytxid\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            const NodeContext& node = EnsureAnyNodeContext(request.context);
            const TolerantChainMonitor& monitor = EnsureTolerantMonitor(node);
            const TolerantReferenceSnapshot snap = monitor.GetSnapshot();

            UniValue result{UniValue::VOBJ};
            result.pushKV("reference_rate", SatPerVB(snap.reference_rate));
            result.pushKV("floor", SatPerVB(snap.floor));
            result.pushKV("reliable", snap.reliable);
            result.pushKV("blocks_observed", snap.blocks_observed);
            result.pushKV("half_life_blocks", snap.half_life_blocks);

            if (request.params[0].isNull()) return result;

            const Txid txid{Txid::FromUint256(ParseHashV(request.params[0], "txid"))};
            const CTxMemPool& mempool = EnsureMemPool(node);
            CTransactionRef tx;
            CAmount fee{0};
            {
                // Released before EvaluateTransaction, which takes cs_main
                // then mempool.cs; holding mempool.cs here would invert that order.
                LOCK(mempool.cs);
                const auto it = mempool.GetIter(txid);
                if (!it) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not in mempool");
                tx = (*it)->GetSharedTx();
                fee = (*it)->GetFee();
            }

            const std::optional<TolerantMempoolVerdict> v = monitor.EvaluateTransaction(*tx, fee);
            if (!v) {
                throw JSONRPCError(RPC_MISC_ERROR, "Transaction inputs are no longer available (confirmed or replaced meanwhile)");
            }

            const auto& a = v->analysis;
            const auto& r = v->pricing;
            UniValue txobj{UniValue::VOBJ};
            txobj.pushKV("txid", txid.GetHex());
            txobj.pushKV("classification", TolerantDataClassToString(a.classification));
            txobj.pushKV("vsize", a.tx_vbytes);
            txobj.pushKV("economic_vsize", a.economic_vbytes);
            txobj.pushKV("data_bytes", a.data_bytes);
            txobj.pushKV("witness_data_bytes", a.witness_bytes);
            txobj.pushKV("output_count", a.output_count);
            txobj.pushKV("dust_outputs", a.dust_outputs);
            txobj.pushKV("fee", r.actual_fee);
            txobj.pushKV("normal_feerate", SatPerVB(r.normal_feerate));
            txobj.pushKV("as_if_feerate", SatPerVB(r.as_if_feerate));
            txobj.pushKV("required_fee", r.required_fee);
            txobj.pushKV("hidden_subsidy", r.hidden_subsidy);
            txobj.pushKV("verdict", v->Verdict());
            result.pushKV("tx", std::move(txobj));
            return result;
        },
    };
}

void RegisterTolerantRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"tolerant", &gettolerantpricing},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
