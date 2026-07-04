#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Tolerant developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test Tolerant Knots relay, mining template, and consensus-tolerant behavior."""

from random import randbytes

from test_framework.messages import (
    CTransaction,
    CTxOut,
    MAX_OP_RETURN_RELAY,
)
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet


class TolerantPolicyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [
            [
                "-tolerant=1",
                "-tolerantdatacarriersize=80",
                "-tolerantlogpolicy=1",
                "-tolerantminingfilter=1",
                "-debug=tolerant",
            ],
            [
                "-tolerant=0",
                "-datacarriersize=200",
                "-acceptnonstddatacarrier=1",
            ],
        ]
        self.setup_clean_chain = True

    def test_op_return(self, node, data_len, success):
        tx = self.wallet.create_self_transfer(fee_rate=0)["tx"]
        data = randbytes(data_len)
        tx.vout.append(CTxOut(nValue=0, scriptPubKey=CScript([OP_RETURN] + [data])))
        tx.vout[0].nValue -= tx.get_vsize()
        tx_hex = tx.serialize().hex()
        if success:
            self.wallet.sendrawtransaction(from_node=node, tx_hex=tx_hex)
            assert tx.rehash() in node.getrawmempool()
        else:
            assert_raises_rpc_error(-26, "scriptpubkey", self.wallet.sendrawtransaction, from_node=node, tx_hex=tx_hex)

    def run_test(self):
        self.wallet = MiniWallet(self.nodes[0])
        self.generatetoaddress(self.nodes[0], 101, self.wallet.get_address())
        self.wallet.rescan_utxos()

        self.log.info("Normal transaction accepted on strict Tolerant node")
        self.wallet.send_self_transfer(from_node=self.nodes[0])

        self.log.info("OP_RETURN within policy accepted")
        self.test_op_return(self.nodes[0], 76, success=True)

        self.log.info("OP_RETURN above policy rejected from mempool")
        self.test_op_return(self.nodes[0], 78, success=False)

        self.log.info("Accepted transactions appear in block template")
        template = self.nodes[0].getblocktemplate({"rules": ["segwit"]})
        mempool = set(self.nodes[0].getrawmempool())
        template_txids = {
            tx["txid"]
            for tx in template["transactions"]
            if tx["txid"] in mempool
        }
        assert_equal(template_txids, mempool)

        self.log.info("Consensus-valid block with large OP_RETURN accepted from permissive peer")
        node1_height = self.nodes[1].getblockcount()
        permissive_wallet = MiniWallet(self.nodes[1])
        self.generatetoaddress(self.nodes[1], 101, permissive_wallet.get_address())
        permissive_wallet.rescan_utxos()
        large_data = randbytes(80)
        tx = permissive_wallet.create_self_transfer(fee_rate=0)["tx"]
        tx.vout.append(CTxOut(nValue=0, scriptPubKey=CScript([OP_RETURN] + [large_data])))
        tx.vout[0].nValue -= tx.get_vsize()
        permissive_wallet.sendrawtransaction(from_node=self.nodes[1], tx_hex=tx.serialize().hex())
        self.generate(self.nodes[1], 1)
        block_hash = self.nodes[1].getbestblockhash()
        assert_equal(self.nodes[1].getblockcount(), node1_height + 102)

        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(self.nodes[0].getbestblockhash(), block_hash)
        assert_equal(self.nodes[0].getblockcount(), node1_height + 102)

        self.log.info("Most-work chain followed after reorg regardless of data content")
        height_before = self.nodes[0].getblockcount()
        self.generate(self.nodes[0], 2)
        longer_tip = self.nodes[0].getbestblockhash()
        assert_equal(self.nodes[0].getblockcount(), height_before + 2)
        self.sync_blocks()
        assert_equal(self.nodes[1].getbestblockhash(), longer_tip)
        assert_equal(self.nodes[1].getblockcount(), height_before + 2)


if __name__ == "__main__":
    TolerantPolicyTest(__file__).main()
