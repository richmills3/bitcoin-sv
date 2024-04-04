#!/usr/bin/env python3
# Copyright (c) 2024 BSV Blockchain Association
# Distributed under the Open BSV software license, see the accompanying file LICENSE.

"""
Test scenario CPFP member with over 1000 unconfirmed txs
"""

from test_framework.blocktools import ChainManager
from test_framework.mininode import (CTransaction,
                                     CTxIn,
                                     CTxOut,
                                     COutPoint,
                                     msg_tx,
                                     msg_block)
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, wait_until


class Cpfp1000children(BitcoinTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def setup_network(self):
        self.add_nodes(self.num_nodes)

    def run_test(self):

        args = ['-genesisactivationheight=1',
                '-maxmempool=10000',
                '-maxnonstdtxvalidationduration=100000',
                '-maxtxnvalidatorasynctasksrunduration=100001',
                '-minminingtxfee=0.00000001']

        with self.run_node_with_connections("Test CPFP member 1000+ unconfirmed txs",
                                            0,
                                            args,
                                            1) as (conn,):

            def create_tx(ptx, n, value=1, num_outs=1):
                tx = CTransaction()
                tx.vin.append(CTxIn(COutPoint(ptx.sha256, n), b"", 0xffffffff))
                for i in range(num_outs):
                    tx.vout.append(CTxOut(value, CScript([OP_TRUE])))
                tx.rehash()
                return tx

            chain = ChainManager()
            chain.set_genesis_hash(int(conn.rpc.getbestblockhash(), 16))

            for i in range(101):
                block = chain.next_block(i + 1)
                chain.save_spendable_output()
                conn.send_message(msg_block(block))

            wait_until(lambda: conn.rpc.getbestblockhash() == block.hash)

            num_outputs = 2500
            out = chain.get_spendable_output()
            total_amount = out.tx.vout[0].nValue
            no_fee_output_amount = int(total_amount / num_outputs)
            tx1 = create_tx(out.tx, 0, no_fee_output_amount, num_outputs)
            conn.send_message(msg_tx(tx1))
            wait_until(lambda: tx1.hash in conn.rpc.getrawmempool())

            # parent tx is in secondary mempool
            info = conn.rpc.getmempoolinfo()
            assert_equal(info['size'], 1)
            assert_equal(info['journalsize'], 0)

            # Total fee from all children pays enough for the parent,
            # but no child on its own pays enough
            num_child_txns = 2200
            extra_fee_required = 3000
            child_fee = (extra_fee_required / num_child_txns)
            child_output_amount = int(no_fee_output_amount - child_fee)

            # send child paying
            for t in range(num_child_txns):
                tx2 = create_tx(tx1, t, child_output_amount)
                conn.send_message(msg_tx(tx2))
                wait_until(lambda: tx2.hash in conn.rpc.getrawmempool())

            # all txs are in secondary mempool
            all_txs = num_child_txns + 1  # parent tx
            info = conn.rpc.getmempoolinfo()
            assert_equal(info['size'], all_txs)
            assert_equal(info['journalsize'], 0)

            # Final child tx pays enough
            tx3 = create_tx(tx2, 0, child_output_amount - extra_fee_required)
            conn.send_message(msg_tx(tx3))
            wait_until(lambda: tx3.hash in conn.rpc.getrawmempool())

            # parent tx, child and grandchild tx
            # +998 children are moved to primary mempool
            all_txs += 1  # child tx
            info = conn.rpc.getmempoolinfo()
            assert_equal(info['size'], all_txs)
            assert_equal(info['journalsize'], 1001)
            conn.rpc.generate(1)

            # make sure cpfp group is not in mempool
            assert tx1.hash not in conn.rpc.getrawmempool()
            assert tx2.hash not in conn.rpc.getrawmempool()
            assert tx3.hash not in conn.rpc.getrawmempool()

            # txs in primary mempool are mined
            # all transactions from secondary mempool are moved to primary
            all_txs -= 1001  # mined in a block
            info = conn.rpc.getmempoolinfo()
            assert_equal(info['size'], all_txs)
            assert_equal(info['journalsize'], all_txs)
            conn.rpc.generate(1)

            # all txs are mined
            info = conn.rpc.getmempoolinfo()
            assert_equal(info['size'], 0)
            assert_equal(info['journalsize'], 0)


if __name__ == '__main__':
    Cpfp1000children().main()
