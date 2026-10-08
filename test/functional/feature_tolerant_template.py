#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Tolerant developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Bitcoin Tolerant V2 Phase 2: honest pricing in local block templates.

Three connected nodes share one mempool:
  node0  -tolerantv2 -tolerantpricingmode=market
  node1  -tolerantv2 (observe, the default)
  node2  plain Bitcoin Core

- market: a witness envelope paying below the reference rate on its true
  size is left out of node0's templates and blocks; it stays in the mempool.
- The same envelope paying the honest price is included.
- A cheap payment and a cheap OP_RETURN are included: they pay full weight.
- observe: node1's template is unchanged, and reports what market would drop.
- A block mined elsewhere containing the excluded envelope is accepted.
- Restarting keeps the reference rate reliable (startup backfill).
- An unknown mode refuses to start.
"""
from decimal import Decimal

from test_framework.address import address_to_scriptpubkey, script_to_p2wsh
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut
from test_framework.script import CScript, OP_0, OP_1, OP_ENDIF, OP_IF
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

ENVELOPE_PAYLOAD = 500
AMOUNT = Decimal("0.1")


class TolerantTemplateTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            # -mintxfee lowered so the wallet will build deliberately cheap transactions.
            ["-tolerantv2=1", "-tolerantpricingmode=market", "-debug=tolerant", "-fallbackfee=0.0002",
             "-mintxfee=0.0000015"],
            ["-tolerantv2=1", "-debug=tolerant"],
            [],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def fund_envelope(self, node, witness_script):
        address = script_to_p2wsh(witness_script)
        txid = node.sendtoaddress(address, AMOUNT)
        vout = next(o["n"] for o in node.getrawtransaction(txid, True)["vout"]
                    if o["scriptPubKey"].get("address") == address)
        return COutPoint(int(txid, 16), vout)

    def spend_envelope(self, node, outpoint, witness_script, fee_sat):
        tx = CTransaction()
        tx.vin = [CTxIn(outpoint)]
        dest = address_to_scriptpubkey(node.getnewaddress(address_type="bech32"))
        tx.vout = [CTxOut(int(AMOUNT * COIN) - fee_sat, dest)]
        wit = CTxInWitness()
        wit.scriptWitness.stack = [bytes(witness_script)]
        tx.wit.vtxinwit = [wit]
        return node.sendrawtransaction(tx.serialize().hex())

    def template_txids(self, node):
        return {t["txid"] for t in node.getblocktemplate({"rules": ["segwit"]})["transactions"]}

    def run_test(self):
        market, observe, plain = self.nodes

        self.log.info("Unknown pricing mode refuses to start")
        self.stop_node(2)
        self.nodes[2].assert_start_raises_init_error(
            ["-tolerantv2=1", "-tolerantpricingmode=filter"],
            "Error: Unknown -tolerantpricingmode 'filter' (use observe or market)")
        self.start_node(2)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)

        self.log.info("Build a chain and a reliable reference rate")
        self.generate(market, 110)
        witness_script = CScript([OP_0, OP_IF, b"\x42" * ENVELOPE_PAYLOAD, OP_ENDIF, OP_1])
        cheap_out = self.fund_envelope(market, witness_script)
        honest_out = self.fund_envelope(market, witness_script)
        self.generate(market, 1)
        assert market.gettolerantpricing()["reliable"]

        self.log.info("Broadcast: a subsidized envelope, an honest one, a cheap payment, a cheap OP_RETURN")
        floor = market.gettolerantpricing()["floor"]
        cheap_fee = int(floor * (180 + len(witness_script) // 4)) + 10
        cheap = self.spend_envelope(market, cheap_out, witness_script, cheap_fee)
        priced = market.gettolerantpricing(cheap)["tx"]
        assert_equal(priced["verdict"], "hidden-subsidy")
        honest = self.spend_envelope(market, honest_out, witness_script, priced["required_fee"] + 10)
        assert_equal(market.gettolerantpricing(honest)["tx"]["verdict"], "honest")
        payment = market.send(outputs={plain.getnewaddress(): 1}, fee_rate=Decimal("0.15"))["txid"]
        op_return = market.send(outputs=[{"data": "42" * 200}], fee_rate=Decimal("0.15"))["txid"]
        for txid in (payment, op_return):
            assert_equal(market.gettolerantpricing(txid)["tx"]["hidden_subsidy"], False)
        self.sync_mempools()

        self.log.info("market: the subsidized envelope is left out; everything else stays")
        txids = self.template_txids(market)
        assert cheap not in txids
        assert {honest, payment, op_return} <= txids
        last = market.gettolerantpricing()["last_template"]
        assert_equal(last["applied"], True)
        assert_equal(last["chunks_flagged"], 1)
        assert_equal(last["txs_flagged"], 1)
        assert_equal(last["fees_flagged"], cheap_fee)

        self.log.info("observe: the template is unchanged, the would-be exclusion is reported")
        assert {cheap, honest, payment, op_return} <= self.template_txids(observe)
        last = observe.gettolerantpricing()["last_template"]
        assert_equal(last["applied"], False)
        assert_equal(last["chunks_flagged"], 1)

        self.log.info("market mines a block without it; it stays in the mempool, nothing is rejected")
        block = self.generate(market, 1)[0]
        mined = set(market.getblock(block)["tx"])
        assert cheap not in mined
        assert {honest, payment, op_return} <= mined
        for node in self.nodes:
            assert cheap in node.getrawmempool()

        self.log.info("A block mined elsewhere with the envelope is accepted by the market node")
        other = self.generate(plain, 1)[0]
        assert cheap in plain.getblock(other)["tx"]
        assert_equal(market.getbestblockhash(), other)
        assert cheap not in market.getrawmempool()

        self.log.info("After a restart the reference rate is reliable at once (startup backfill)")
        self.restart_node(0)
        info = market.gettolerantpricing()
        assert_equal(info["reliable"], True)
        assert_greater_than(info["blocks_observed"], 5)
        assert_equal(info["mode"], "market")


if __name__ == "__main__":
    TolerantTemplateTest(__file__).main()
