# Bitcoin Tolerant V2 — Honest Blockspace Pricing

**Status:** Phase 1 (observe-only) complete on Bitcoin Core v30.3. Nothing in
this document changes consensus, chain selection, or which valid blocks are
accepted — at any phase.

> No moral filtering. No hidden subsidy. Honest blockspace pricing.

## Principles

Bitcoin Tolerant never asks whether a transaction's data is good or bad. It asks:

> Is this transaction paying the real economic cost of the block space it
> consumes?

Three consequences run through the whole design:

1. **Monetary bytes are never multiplied.** A purely monetary transaction has
   `economic_vbytes == tx_vbytes`, so its as-if feerate equals its normal
   feerate. Sending money never costs extra.
2. **Data is priced, never banned.** A data-carrying transaction that pays the
   honest price is treated like any other transaction.
3. **Scarcity is never invented.** When the network is not competing for block
   space, the reference price falls to the floor and data pays the floor like
   everyone else. Overcharging without demand would be as dishonest as
   subsidizing during a spike.

## Base and architecture

Bitcoin Tolerant builds directly on **Bitcoin Core** (currently `v30.3`) and
tracks upstream Core releases. All Tolerant logic lives in Tolerant-owned files:

| File | Role |
|---|---|
| `src/node/tolerant_datacarrier.{h,cpp}` | Detects data embedded in scripts |
| `src/node/tolerant_pricing.{h,cpp}` | Classifies and prices a transaction |
| `src/node/tolerant_reference_rate.{h,cpp}` | Reference rate, chain monitor, mempool evaluation |

Changes to Core's own files are deliberately minimal — wiring only, no logic:
`src/init.cpp` (flags, lifecycle), `src/node/context.{h,cpp}` (ownership),
`src/logging.{h,cpp}` (the `tolerant` log category), `src/rpc/register.h`
(one registration line), and the two `CMakeLists.txt`. The RPC itself lives in
the Tolerant-owned `src/rpc/tolerant.cpp`. Keeping logic out of Core's files keeps upstream syncs close to
conflict-free.

## Why this exists: the witness discount

SegWit (2017) counts a witness byte as 1 weight unit instead of 4 — a 75%
discount designed for signatures. Since 2023, the same discount applies to any
data placed in a witness inside an `OP_FALSE OP_IF … OP_ENDIF` envelope
(inscriptions). Core v30 additionally raised the default OP_RETURN relay limit to
100,000 vbytes without attaching a price to it.

Banning this data is not possible without judging content or forking consensus.
Removing the discount *for data* is a local pricing decision that requires
neither. V2 does exactly that, and nothing more.

## 1. The formula

```txt
reference_rate  = Σ(fees_i · w_i) / Σ(vbytes_i · w_i)      over confirmed blocks
economic_vbytes = vsize(tx_weight + extra_weight)
required_fee    = economic_vbytes × reference_rate
as_if_feerate   = actual_fee / economic_vbytes
hidden_subsidy  ⟺ as_if_feerate < reference_rate
```

`extra_weight` charges each data byte the honest rate *relative to the consensus
weight it already pays*:

| Data location | Consensus weight today | Target (premium 1.0) | Extra |
|---|---|---|---|
| Witness (P2WSH script, taproot script path) | 1 WU/byte | 4 WU/byte | **+3 WU/byte** |
| OP_RETURN output, legacy scriptSig | 4 WU/byte | 4 WU/byte | 0 |

OP_RETURN bytes were never discounted, so they are never charged twice.

## 2. Scarcity: the reference rate

Scarcity is the realized market price of **recently confirmed** block space,
weighted toward the most recent blocks:

```txt
w_i = 0.5^(age_in_blocks / half_life)      half_life = 6 blocks (~1 hour)
```

- **Source: confirmed blocks, via undo data.** Fees are computed from each
  connected block's `CBlockUndo` (the exact spent outputs), never from the local
  mempool. A mempool-derived price inherits whatever the node's own relay policy
  filtered out and under-reports real scarcity. Undo data makes the price a fact
  about the network, not about this node.
- **Denominator: the whole block**, coinbase included — the block space the fees
  were paid against.
- **Floor:** the node's `-minrelaytxfee` (0.1 sat/vB by default in Core v30). The
  rate never reports below it.
- **Bootstrap guard:** fewer than 6 observed blocks ⇒ `warming-up`, and no subsidy
  verdict is drawn.
- **Background chainstate skipped:** assumeutxo background validation replays
  historical blocks and does not describe the current market.

### Why 6 blocks, and why decay

The window has a principled *range*, not a principled point:

- **Not ~1000 blocks (a week).** The goal is anti-subsidy. A long window anchors
  the price to the past and prices data at last week's rate during a spike — a
  subsidy exactly when block space is dearest.
- **Not 1 block.** One anomalous block (empty, or stuffed by its own miner) is not
  a market, and would be cheap to manipulate.

The harm is asymmetric: under-pricing leaks subsidy, over-pricing is merely
conservative. That favors a short, current window. A half-life of 6 blocks
(~1 hour) keeps the price current while diluting any single anomalous block.

Exponential decay replaces a hard 6-block cutoff. Recent blocks dominate and old
data fades instead of falling off a cliff. It is implemented as a running
accumulator, so no block history is stored. After 2016 blocks — Bitcoin's own
retarget period — a block's weight is `0.5^336`, effectively zero. The retarget
period is the model's **memory ceiling, not a reset point**: difficulty
adjustments have no causal relationship to the fee market, and a reset there
would introduce an artificial discontinuity.

The half-life is **one fixed, visible constant**, never auto-tuned. An adaptive
window would trade one arguable number for several buried ones.

### Known limitation: reorgs

Exponential decay cannot be undone exactly without storing history, so
`BlockDisconnected` is intentionally a no-op. A short reorg causes a small,
transient skew. This is acceptable while nothing enforces on the rate, and must
be revisited before any phase does.

## 3. Permanence

| Usage | Permanence | Why |
|---|---|---|
| Monetary transaction | 1.0 | Baseline |
| Small commitment (≤ 40 B) | 1.0 | Bounded, archival only |
| OP_RETURN payload | 1.0 | Archival, provably unspendable, never enters the UTXO set |
| Witness / script-path payload | 1.0 | Archival; its discount is removed by the formula above |
| Dust outputs (UTXO bloat) | higher | Burdens **active state** on every node, possibly forever |

**UTXO bloat outranks OP_RETURN.** Ranking OP_RETURN higher would be filtering by
what data *looks like* rather than by what it costs.

Only `dust_outputs` is a priceable bloat signal. **Output count is never priced:**
payment batching is the most block-space-efficient behavior in Bitcoin, and an
output-count premium would punish the most efficient users.

## 4. Externality

Bandwidth, validation, and archival costs to third parties are real but not
locally measurable. The externality multiplier is therefore a pure operator
constant, default **1.0**. It is never inferred, auto-tuned, or derived from
peers. "Social coordination risk" and "filter-fork pressure" are excluded
outright: pricing political effects is moral filtering in economic vocabulary.

## 5. Detection and classification

`tolerant_datacarrier` walks script structure without executing it:

- **Unconditional OP_RETURN**: output-side, already full price.
- **Envelope**: `OP_FALSE OP_IF <data> OP_ENDIF`, and `<push> OP_DROP`. Split by
  location into *base* (already full price) and *witness* (discounted today).
- Input scripts are resolved through P2SH, P2WSH, and taproot script path; the
  annex and control block are stripped.

Protocol-specific heuristics (e.g. detectors for individual token protocols) are
**deliberately absent**. They encode judgments about particular uses rather than
measuring cost.

Classes: `MONETARY`, `SMALL_COMMITMENT`, `OP_RETURN_DATA`, `WITNESS_DATA`,
`UTXO_BLOAT`, `MIXED`. `TAPROOT_DATA` is reserved. Taproot envelopes currently
report as `WITNESS_DATA`, since both are priced identically and V2 never guesses
toward a higher premium. Classification is descriptive only: it never rejects
anything.

## 6. Configuration

| Option | Default | Status | Description |
|---|---|---|---|
| `-tolerantv2` | `0` | **Implemented** | Master switch. Off ⇒ no Tolerant object is even constructed; the node is plain Bitcoin Core. |
| `-tolerantreferenceblocks` | `6` | **Implemented** | Half-life in blocks. Rejected at startup outside 1–2016. |
| `-debug=tolerant` | off | **Implemented** | Per-block and per-transaction observations. |
| `-tolerantpermanencepremium` | `1.0` | Planned | Multiplier on data bytes. |
| `-tolerantexternalitypremium` | `1.0` | Planned | Operator judgment, never inferred. |
| `-tolerantpricingmode` | `observe` | Planned | Phase 2 gate. |
| `-tolerantmarketexceptions` | `0` | Planned | Never default-on. |

No flag is added before the code that reads it exists.

## 7. Phasing

| Phase | Scope | Status |
|---|---|---|
| **1a** | Detection, classification, economic size, pricing | ✅ |
| **1b** | Reference rate from confirmed blocks | ✅ |
| **1c** | Flags, lifecycle in `bitcoind` | ✅ |
| **1d** | Evaluate mempool transactions, `[TolerantV2]` logging | ✅ |
| **1e** | `gettolerantpricing` RPC — the reference price, machine-readable | ✅ |
| **2** | Local mining templates only; market-priced, default off | Planned |
| **3** | Relay policy; optional, deliberately last | Planned |

Phase 3 is last for a reason: relay policy that diverges from the network
partitions a node's mempool from its peers and is how "local policy" becomes de
facto soft enforcement. **No phase touches consensus.**

## 8. Observability

The deliverable of V2 is a **price, not a filter**. The reference rate must be
visible even when it condemns nothing.

With `-tolerantv2=1 -debug=tolerant`:

```txt
[TolerantV2] block <hash> height=111 fees=6680 vbytes=475 -> reference_rate=4.107 sat/vB (blocks_observed=111)
[TolerantV2] tx <txid> class=OP_RETURN_DATA vbytes=334 economic_vbytes=334 fee=6680
             normal_feerate=20.000 sat/vB as_if_feerate=20.000 sat/vB
             reference_rate=0.100 sat/vB required_fee=34 verdict=honest
```

Verdicts: `honest`, `hidden-subsidy`, `warming-up`.

### RPC: `gettolerantpricing ( "txid" )`

Read-only; requires `-tolerantv2=1` (otherwise it errors rather than report a
price that is not being tracked). Listed under its own `== Tolerant ==` section
in `bitcoin-cli help`.

```txt
$ bitcoin-cli gettolerantpricing
{ "reference_rate": 4.107, "floor": 0.1, "reliable": true,
  "blocks_observed": 111, "half_life_blocks": 6 }

$ bitcoin-cli gettolerantpricing <mempool txid>      # illustrative values
{ ..., "tx": { "classification": "WITNESS_DATA", "vsize": 210, "economic_vsize": 588,
               "data_bytes": 505, "witness_data_bytes": 505, "fee": 40,
               "normal_feerate": 0.19, "as_if_feerate": 0.068, "required_fee": 59,
               "hidden_subsidy": true, "verdict": "hidden-subsidy" } }
```

Rates are in sat/vB, amounts in satoshis. It reports every transaction,
including monetary ones: a reference price that is only visible when it
condemns something is not a reference price. Monetary transactions are not
logged; their verdict is fixed by construction.

Per-transaction evaluation takes `cs_main` to resolve prevouts, so it runs **only
when the `tolerant` category is enabled**. With the category off, `-tolerantv2`
costs one validation-interface subscriber and one accumulator update per block.

## 9. Tests

`src/test/tolerant_pricing_tests.cpp`, `src/test/tolerant_reference_rate_tests.cpp`:

| Invariant | Test |
|---|---|
| Money never pays a premium | `monetary_tx_pays_no_premium` |
| Witness data loses the discount, exactly +3 WU/byte | `witness_payload_loses_the_discount` |
| OP_RETURN is not double-charged | `op_return_is_not_double_charged` |
| Batching is never bloat | `payment_batching_is_not_utxo_bloat` |
| Spike > calm > drought; same fee honest in drought, subsidy in spike | `required_fee_tracks_reference_rate`, `spike_calm_drought` |
| One anomalous block is diluted | `single_anomalous_block_is_diluted` |
| Bootstrap guard, floor | `unreliable_until_warm`, `floor_always_applies` |
| Exact fees from a real connected block | `real_connected_block_totals` |
| End-to-end verdicts on a real chain; unknown prevouts never guessed | `evaluate_transaction_against_real_chain` |
| RPC end to end, incl. a real P2WSH witness envelope on regtest | `test/functional/feature_tolerant_pricing.py` |

## 10. Non-negotiables

- No consensus changes. No BIP-110 enforcement. No new opcodes.
- No chain-selection change. The most-work valid chain always wins.
- Externally valid blocks are always accepted. Local policy is never consensus invalidity.
- Never filter by content meaning — only by economic cost.
- Scarcity is measured from confirmed blocks, never fabricated from local state.
- Output count is never priced.
- The externality multiplier is never inferred.
- Market-priced exceptions are never default-on.

## 11. Open questions

1. **Reorg exactness** before any enforcing phase: accept the transient skew, or
   keep a small per-block buffer (bounded by max reorg depth) to rebuild the
   accumulator exactly?
2. **UTXO-bloat pricing mechanics:** Core's `-dustrelayfee` already covers part of
   this at relay. V2's dust contribution may stay analytical.
3. **Taproot vs. witness distinction:** worth resolving only if the two ever
   warrant different permanence multipliers.
