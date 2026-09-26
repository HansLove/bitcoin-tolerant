#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Tolerant developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Bitcoin Tolerant V2: gettolerantpricing, end to end (doc/tolerant-v2-pricing.md).

- With -tolerantv2=0 the RPC refuses clearly instead of reporting a fake price.
- The reference rate is fed by confirmed blocks and floored at -minrelaytxfee.
- A monetary transaction's economic size equals its real size.
- An OP_RETURN payload is never double-charged.
- A witness envelope (OP_FALSE OP_IF <data> OP_ENDIF in a P2WSH script) loses
  the witness discount: paying the relay minimum on its *discounted* size is a
  hidden subsidy, while paying the honest required_fee is accepted as honest.
- Nothing here is rejected: every transaction still enters the mempool.
"""
from decimal import Decimal

from test_framework.address import address_to_scriptpubkey, script_to_p2wsh
from test_framework.messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxInWitness,
    CTxOut,
)
from test_framework.script import CScript, OP_0, OP_1, OP_ENDIF, OP_IF
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

ENVELOPE_PAYLOAD = 500


class TolerantPricingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-tolerantv2=1", "-debug=tolerant", "-fallbackfee=0.0002"],
            [],  # plain Bitcoin Core behavior
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        # Independent nodes: node1 only exists to check the disabled path.
        self.setup_nodes()

    def fund_envelope_output(self, node, witness_script, amount):
        address = script_to_p2wsh(witness_script)
        txid = node.sendtoaddress(address, amount)
        tx = node.getrawtransaction(txid, True)
        vout = next(o["n"] for o in tx["vout"] if o["scriptPubKey"].get("address") == address)
        self.generate(node, 1, sync_fun=self.no_op)
        return COutPoint(int(txid, 16), vout)

    def spend_envelope(self, node, outpoint, amount, witness_script, fee_sat):
        tx = CTransaction()
        tx.vin = [CTxIn(outpoint)]
        dest = address_to_scriptpubkey(node.getnewaddress(address_type="bech32"))
        tx.vout = [CTxOut(int(amount * COIN) - fee_sat, dest)]
        wit = CTxInWitness()
        wit.scriptWitness.stack = [bytes(witness_script)]
        tx.wit.vtxinwit = [wit]
        return node.sendrawtransaction(tx.serialize().hex())

    def run_test(self):
        node = self.nodes[0]

        self.log.info("Disabled node refuses instead of reporting a fake price")
        assert_raises_rpc_error(-1, "Bitcoin Tolerant V2 is disabled", self.nodes[1].gettolerantpricing)

        self.log.info("Reference rate warms up from confirmed blocks, floored at minrelaytxfee")
        self.generate(node, 3, sync_fun=self.no_op)
        info = node.gettolerantpricing()
        assert_equal(info["reliable"], False)
        assert_equal(info["half_life_blocks"], 6)
        self.generate(node, 107, sync_fun=self.no_op)
        info = node.gettolerantpricing()
        assert_equal(info["reliable"], True)
        assert_equal(info["blocks_observed"], 110)
        # Coinbase-only blocks pay no fees: the rate sits exactly on the floor.
        assert_equal(info["reference_rate"], info["floor"])
        assert "tx" not in info

        self.log.info("Unknown txid is an error, not a guess")
        assert_raises_rpc_error(-5, "Transaction not in mempool", node.gettolerantpricing, "00" * 32)

        self.log.info("Monetary transaction: economic size == real size")
        txid = node.sendtoaddress(node.getnewaddress(), 1)
        p = node.gettolerantpricing(txid)["tx"]
        assert_equal(p["classification"], "MONETARY")
        assert_equal(p["economic_vsize"], p["vsize"])
        assert_equal(p["data_bytes"], 0)
        assert_equal(p["verdict"], "honest")

        self.log.info("OP_RETURN payload: already full price, never double-charged")
        txid = node.send([{"data": "42" * 200}])["txid"]
        p = node.gettolerantpricing(txid)["tx"]
        assert_equal(p["classification"], "OP_RETURN_DATA")
        assert_equal(p["economic_vsize"], p["vsize"])
        assert_greater_than(p["data_bytes"], 200)
        self.generate(node, 1, sync_fun=self.no_op)

        self.log.info("Witness envelope loses the discount")
        witness_script = CScript([OP_0, OP_IF, b"\x42" * ENVELOPE_PAYLOAD, OP_ENDIF, OP_1])
        cheap_out = self.fund_envelope_output(node, witness_script, Decimal("0.1"))
        honest_out = self.fund_envelope_output(node, witness_script, Decimal("0.1"))

        # Pay the relay minimum on the DISCOUNTED size: relayable, but a subsidy.
        floor = node.gettolerantpricing()["floor"]
        probe_vsize = 180 + len(witness_script) // 4
        cheap_fee = int(floor * probe_vsize) + 10
        cheap_txid = self.spend_envelope(node, cheap_out, Decimal("0.1"), witness_script, cheap_fee)
        p = node.gettolerantpricing(cheap_txid)
        tx = p["tx"]
        assert_equal(tx["classification"], "WITNESS_DATA")
        assert_greater_than(tx["witness_data_bytes"], ENVELOPE_PAYLOAD - 1)
        # Each witness data byte gains +3 weight units: ~0.75 vB per byte.
        assert_greater_than(tx["economic_vsize"], tx["vsize"] + (ENVELOPE_PAYLOAD * 3) // 4 - 1)
        assert_greater_than(p["reference_rate"], tx["as_if_feerate"])
        assert_equal(tx["hidden_subsidy"], True)
        assert_equal(tx["verdict"], "hidden-subsidy")
        # Observe-only: the transaction is still in the mempool.
        assert cheap_txid in node.getrawmempool()

        # The same envelope paying the honest required fee is simply honest.
        honest_fee = tx["required_fee"] + 10
        honest_txid = self.spend_envelope(node, honest_out, Decimal("0.1"), witness_script, honest_fee)
        tx = node.gettolerantpricing(honest_txid)["tx"]
        assert_equal(tx["verdict"], "honest")
        assert_equal(tx["hidden_subsidy"], False)

        self.log.info("The verdicts reach the log")
        with node.assert_debug_log(expected_msgs=["[TolerantV2] block", "-> reference_rate="]):
            self.generate(node, 1, sync_fun=self.no_op)


if __name__ == "__main__":
    TolerantPricingTest(__file__).main()
