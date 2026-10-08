Bitcoin Tolerant
================

**Block space has a price.** Bitcoin Tolerant measures it from every confirmed
block and charges data its real weight. It is built directly on Bitcoin Core.

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

The node measures every block, gives each data-carrying transaction a verdict
(`honest` or `hidden-subsidy`), and publishes the reference price. In `market`
mode its own block templates leave out chunks that use the witness discount and
pay below that price. It never rejects a transaction from the mempool, and
always accepts blocks mined by others.

Run it
------

```sh
cmake -B build -DENABLE_IPC=OFF
cmake --build build
build/bin/bitcoind -tolerantv2=1 -debug=tolerant
build/bin/bitcoin-cli gettolerantpricing
build/bin/bitcoin-cli gettolerantpricing <mempool txid>
```

`-DENABLE_IPC=OFF` skips the optional Cap'n Proto dependency. Full build
instructions are in `doc/build-*.md`.

| Option | Default | |
|---|---|---|
| `-tolerantv2` | `0` | Enables Bitcoin Tolerant. With `0`, the node is plain Bitcoin Core. |
| `-tolerantreferenceblocks` | `6` | Half-life of the reference rate, in blocks (1–2016). |
| `-tolerantpricingmode` | `observe` | `observe` logs what templates would leave out; `market` leaves it out. |
| `-debug=tolerant` | off | Logs each block's rate and each data transaction's verdict. |

Outside its reach
-----------------

Bitcoin Tolerant never changes consensus rules or chain selection, never rejects
a valid block, never enforces BIP-110 or any soft fork, and never filters by
what data means.

Design and code
---------------

- Design: [`doc/tolerant-v2-pricing.md`](doc/tolerant-v2-pricing.md)
- Upstream policy and decision log: [`doc/tolerant-upstream.md`](doc/tolerant-upstream.md)
- Detection: `src/node/tolerant_datacarrier.{h,cpp}`
- Pricing: `src/node/tolerant_pricing.{h,cpp}`
- Reference rate and chain monitor: `src/node/tolerant_reference_rate.{h,cpp}`
- Block templates: `src/node/tolerant_template.{h,cpp}`
- RPC: `src/rpc/tolerant.cpp`

All Tolerant logic lives in its own files. Bitcoin Core's files carry only
wiring, so upstream releases merge with little friction. Each Core release is
reviewed with `contrib/tolerant/upstream-review.sh` before Tolerant moves to it.

Tests:

```sh
build/bin/test_bitcoin --run_test=tolerant_defaults_tests,tolerant_pricing_tests,tolerant_reference_rate_tests,tolerant_reference_rate_chain_tests,tolerant_template_tests
build/test/functional/feature_tolerant_pricing.py
build/test/functional/feature_tolerant_template.py
```

Branches
--------

| Branch | Base |
|---|---|
| `tolerant-core` | Bitcoin Core v30.3 (stable) |
| `tolerant-core-v32` | Bitcoin Core v32.0 release candidate |

Upstream and license
--------------------

Bitcoin Tolerant is an independent project built on
[Bitcoin Core](https://github.com/bitcoin/bitcoin) and tracks its releases. It is
not affiliated with or endorsed by Bitcoin Core or its maintainers. Everything
outside the Tolerant files above is Bitcoin Core; see `doc/` for its
documentation and `CONTRIBUTING.md` for its development process.

Released under the MIT license. See [COPYING](COPYING).

Support
-------

Bitcoin Tolerant is independent and unfunded. Donations:
`bc1q6n0h05956f4hlnwdppptjh6aj8wemymyns7r5c`
