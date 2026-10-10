Bitcoin Tolerant
================

**Block space has a price.** Bitcoin Tolerant measures it from every confirmed
block and charges data its real weight. It is an independent Bitcoin node built
directly on Bitcoin Core v30.3.

https://bitcointolerant.com

What it does
------------

SegWit counts a witness byte as one weight unit instead of four, a discount
designed for signatures. Since 2023 the same discount also applies to arbitrary
files stored in the witness. Bitcoin Tolerant removes that discount for data,
and only for data. It does not filter content or change consensus.

```
reference_rate  = recent block fees / recent block vbytes
economic_vbytes = real vbytes + witness data bytes × 3/4
required_fee    = economic_vbytes × reference_rate
```

- **The price comes from confirmed blocks.** It is read from each block's undo
  data, never from the local mempool. Recent blocks count most: a block's weight
  halves every 6 blocks, and fades to nothing over one difficulty period.
- **Payments are never multiplied.** A monetary transaction's economic size is
  its real size.
- **OP_RETURN is never charged twice.** It already pays full weight.
- **Quiet markets stay cheap.** When nobody competes for space, the price falls
  to the minimum relay fee for everyone, data included.

Status: **Phase 1, observe.** The node measures every block, gives each
data-carrying transaction a verdict (`honest` or `hidden-subsidy`), and
publishes the reference price over RPC and logs. It rejects nothing: relay,
mempool and mining behave exactly as in Bitcoin Core.

Run it
------

Bitcoin Tolerant needs the same build tools as Bitcoin Core (a C++ compiler,
CMake, Boost and libevent). See [`doc/build-osx.md`](doc/build-osx.md),
[`doc/build-unix.md`](doc/build-unix.md) or
[`doc/build-windows.md`](doc/build-windows.md).

```sh
git clone https://github.com/HansLove/bitcoin-tolerant.git && cd bitcoin-tolerant
cmake -B build -DENABLE_IPC=OFF
cmake --build build
build/bin/bitcoind -tolerantv2=1 -debug=tolerant
build/bin/bitcoin-cli gettolerantpricing
build/bin/bitcoin-cli gettolerantpricing <mempool txid>
```

`-DENABLE_IPC=OFF` skips the optional Cap'n Proto dependency.

| Option | Default | |
|---|---|---|
| `-tolerantv2` | `0` | Enables Bitcoin Tolerant pricing. With `0`, the node is plain Bitcoin Core. |
| `-tolerantreferenceblocks` | `6` | Half-life of the reference rate, in blocks (1–2016). |
| `-debug=tolerant` | off | Logs each block's rate and each data transaction's verdict. |

How it differs
--------------

| | Bitcoin Core v30 | Bitcoin Knots | Bitcoin Tolerant |
|---|---|---|---|
| Arbitrary data | Relayed, with the witness discount | Filtered | Priced at real weight |
| Basis | Permissive | Content judgment | Economic cost only |
| Quiet market | Cheap | Still filtered | Cheap, no penalty |
| Price visible | No | No | Yes, over RPC and logs |

Outside its reach
-----------------

Bitcoin Tolerant never changes consensus rules or chain selection, never rejects
a valid block, and never filters by what data means.

Design and code
---------------

- Design: [`doc/tolerant-v2-pricing.md`](doc/tolerant-v2-pricing.md)
- Detection: `src/node/tolerant_datacarrier.{h,cpp}`
- Pricing: `src/node/tolerant_pricing.{h,cpp}`
- Reference rate and chain monitor: `src/node/tolerant_reference_rate.{h,cpp}`
- RPC: `src/rpc/tolerant.cpp`

All Tolerant logic lives in its own files. Bitcoin Core's files carry only
wiring, so upstream releases merge with little friction.

Tests:

```sh
build/bin/test_bitcoin --run_test=tolerant_pricing_tests,tolerant_reference_rate_tests
build/test/functional/feature_tolerant_pricing.py
```

Branches
--------

| Branch | Base |
|---|---|
| `tolerant-core` | Bitcoin Core v30.3 (stable, default) |
| `tolerant-core-v32` | Bitcoin Core v32.0 release candidate, with Phase 2 work |

Upstream and license
--------------------

Bitcoin Tolerant is an independent project built on
[Bitcoin Core](https://github.com/bitcoin/bitcoin) and tracks its releases. It is
not affiliated with or endorsed by Bitcoin Core or its maintainers. Everything
outside the Tolerant files above is Bitcoin Core; see `doc/` for its
documentation and [`CONTRIBUTING.md`](CONTRIBUTING.md) for its development
process.

Released under the MIT license. See [COPYING](COPYING).

Support
-------

Bitcoin Tolerant is independent and unfunded. Donations:
`bc1q6n0h05956f4hlnwdppptjh6aj8wemymyns7r5c`
