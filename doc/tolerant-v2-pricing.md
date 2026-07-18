# Bitcoin Tolerant — V2 Design: Honest Blockspace Pricing

**Status:** Design proposal. No implementation. No consensus change at any phase.

## Principles

V1 established:

> Strict in relay. Conservative in mining. Tolerant in consensus.

V2 adds:

> No moral filtering. No hidden subsidy. Honest blockspace pricing.

The question V2 asks about a transaction is never "is this data good or bad?" It is:

> Is this transaction paying the real economic cost of the block space it consumes, displaces, or burdens?

## 1. What already exists (read this first)

Bitcoin Knots already implements most of the proposed pricing pipeline. V2 must extend it, not duplicate it.

| Proposed V2 concept | Existing Knots mechanism |
|---|---|
| `economic_vbytes` | `CTxMemPoolEntry::GetTxSize()` — `GetVirtualTransactionSize(nTxWeight + m_extra_weight, sigOpCost, nBytesPerSigOp)` (`src/kernel/mempool_entry.h:186`) |
| `data_vbytes × data_premium_multiplier` | `CalculateExtraTxWeight(tx, view, g_weight_per_data_byte)` (`src/policy/policy.cpp:585`) |
| `data_premium_multiplier` | `-datacarriercost`, stored as `g_weight_per_data_byte` (`src/init.cpp:1231`) |
| `as_if_feerate` | Entry feerate — already computed over `GetTxSize()`, i.e. already as-if |
| `required_fee = economic_vbytes × rate` | `packageFees < m_options.blockMinFeeRate.GetFee(packageSize)` (`src/node/miner.cpp:463`), where `packageSize = GetSizeWithAncestors()` is as-if vbytes |
| Data byte accounting | `CScript::DatacarrierBytes()` (`src/script/script.h:582`), covering both witness/input and output paths |

`DEFAULT_WEIGHT_PER_DATA_BYTE` is `4` (`src/policy/policy.h:66`) — 1 vbyte per data byte. Default Knots already denies data bytes the witness discount. Under `-corepolicy`, `init.cpp:884` soft-sets `-datacarriercost=0.25`, restoring it.

**Consequence:** a stock Knots node already prices data at as-if vbytes and already excludes underpaying packages from templates via `blockMinFeeRate`. The honest framing of V2 is therefore *not* "introduce as-if feerate." It is:

1. **Scarcity is missing.** `blockMinFeeRate` is a static floor, not the marginal opportunity cost of the block being built.
2. **The multiplier is one undifferentiated scalar.** A 1-byte hash commitment and a 380 kB witness payload get the same vB/B rate. Permanence and externality are not separated.
3. **Relay and mining are fused.** `extra_weight` is baked in at mempool acceptance, so any change to the multiplier changes relay behavior. There is no template-only lens.
4. **It is unobservable.** Nothing logs why a transaction was priced the way it was.

V2 addresses those four gaps and nothing else.

### The pricing formula, corrected

The instruction offered a simplification:

```txt
economic_vbytes = tx_vbytes × data_premium_multiplier   // REJECTED
```

This is rejected. It applies the data premium to monetary bytes — the signature, the change output, the ordinary payment. That is a hidden tax on monetary transactions, which is the mirror image of the hidden subsidy V2 exists to remove. It also regresses from what Knots already does correctly. V2 uses only:

```txt
economic_vbytes = base_monetary_vbytes + data_vbytes × data_premium_multiplier
data_premium_multiplier = permanence_multiplier × externality_multiplier
required_fee = economic_vbytes × reference_rate          // reference_rate defined in §2
```

`base_monetary_vbytes` is never multiplied. A pure monetary transaction always has `data_premium_multiplier` applied to a zero-byte base, so `economic_vbytes == tx_vbytes` and `as_if_feerate == normal_feerate`. This is a required invariant, tested in §7.

## 2. Scarcity: the recent-block reference rate

**Design decision (supersedes the earlier template-introspection model): scarcity is the realized market price of recently confirmed block space — a plain average over the last N blocks.**

An earlier draft tried to measure scarcity by introspecting the node's own block template: find the marginal displaced package, detect whether the template was space-constrained, price per-transaction displacement. That was correct but elaborate — a two-pass assembler, a space-constrained exit analysis, a per-transaction slack comparison — and it measured scarcity through the node's *own* mempool, which V1 relay policy has already filtered. It under-reported the very scarcity it was trying to price (the "filtered-lens problem").

The recent-block average dissolves all of that:

```txt
reference_rate = total_fees(last N blocks) / total_vbytes(last N blocks)
```

One division over data every node already has. It requires no template introspection, no fixed-point iteration, no fullness detection, no mempool inspection. It is the same number for the whole node until the next block arrives.

### Why this is honest, and why it is simpler

- **It is a realized price, never a fabricated one.** Every term is a fact about blocks that already confirmed on the most-work chain. There is no "invent scarcity where none exists" failure mode, because the model never consults local state — it reads what the global market actually paid.
- **It self-scales with real demand.** When demand is low, recent blocks are cheap and `reference_rate` falls toward the minimum relay feerate. When blocks are full and contested, it rises. Competition is encoded automatically, with no explicit displacement logic. This is a strictly better answer to the "empty mempool" question (§11.1) than the old local model: if *my* mempool is empty but the *network* is busy, the honest price is still high — and only the network reference reports that.
- **It is block-limit-normalized by construction.** A feerate is fee-per-vbyte. Because a block is capped, `total_fees / total_vbytes` is already an absolute number expressed against the block-space budget. Block maximums therefore never appear in the equation — the scarcity of the asset is baked into the denominator, not modeled on top of it. (For the record: the cap is **4,000,000 weight units = 1,000,000 virtual bytes** per block. Not 100,000 — that figure is closer to an old-style 100 kB *standard-tx* soft cap. Under the reference-rate model the exact number is immaterial to the math; it matters only as the reason block space is scarce at all.)

### The formula, end to end

```txt
reference_rate = total_fees(last N blocks) / total_vbytes(last N blocks)   // sat/vB, floored at minrelayfee
economic_vbytes = base_monetary_vbytes + data_vbytes × data_premium_multiplier   // §1
required_fee    = economic_vbytes × reference_rate
as_if_feerate   = actual_fee / economic_vbytes

hidden subsidy  ⟺  actual_fee < required_fee  ⟺  as_if_feerate < reference_rate
```

That is the entire scarcity model. A monetary transaction has `economic_vbytes == tx_vbytes`, so it simply needs to clear the recent market rate like any transaction — no penalty. A data-heavy transaction has `economic_vbytes > tx_vbytes`, so it must pay proportionally more fee to clear the *same* market rate. It is never rejected for carrying data; it is only asked to pay the market rate for the footprint it really imposes. **That is the whole meaning of "no subsidy": pay recent market price on your true economic size.**

### Two parameters, and the one honest caveat

- **`reference_rate` uses the mean, not the marginal.** The average feerate of recent blocks sits *above* the marginal (lowest-included) feerate. A miner maximizing revenue would use the marginal rate — the true opportunity cost. Tolerant deliberately uses the mean, and this is a feature, not a bug, *for this project*: Tolerant's stated goal is to keep block space scarce, so setting the bar at "better than typical recent block space" rather than "better than the worst that squeaked in" is an intentional, conservative bias in favor of scarcity. This must be stated plainly rather than hidden, because it changes what "subsidy" means: a transaction paying between the marginal and the mean is *flagged* by Tolerant though it would have been profitably mined elsewhere. If an operator wants the strict opportunity-cost bar instead, `-tolerantopportunitycost=marginal` uses the lowest-feerate package of each recent block. Default is `average`.
- **`N` is the only real judgment call left, and it is a *range* with a principle, not a point.** The purpose — anti-subsidy — breaks the tradeoff's symmetry and disqualifies large windows on principle, not taste: a reference set too *low* lets subsidy leak through (the failure V2 exists to prevent), while one set too *high* is merely more conservative than the market (the safe direction for a scarcity-first project). A large window (e.g. 1000 blocks ≈ 1 week) anchors the reference to last week's price, so during a fee spike data pays the cheap historical rate exactly when block space is most valuable — subsidy. So the reference must track a *current* price, which rules out the large end. The small end is bounded by noise and manipulability: with `N` of 1–2, a single anomalous or self-stuffed block swings the rate. The defensible range is ~6–144 blocks (one hour to one day). **Default `N = 6` (≈ one hour of recent blocks)** — short enough to reflect the price *now* and react up quickly into a spike, long enough that one anomalous block is diluted. The "one hour" framing is deliberate: it answers "why 6?" with an economic horizon rather than a bare number. Configurable via `-tolerantreferenceblocks`. Operators wanting extra robustness to a single stuffed block can set `-tolerantopportunitycost=median-block` (median of per-block average rates) — one line more code, offered but not default.

### What is dropped

The `static`, `template`, and `mempool` modes, the two-pass assembler, the space-constrained exit analysis, per-transaction slack pricing, and the fullness-interpolation discussion are all **removed**. `-datacarriercost`'s `blockMinFeeRate` still serves as the absolute floor under `reference_rate` so the required fee can never round to zero, but it is no longer a "scarcity mode."

The guardrail survives in a stronger form, because it is now automatic rather than enforced: **the model cannot fabricate scarcity, because it never reads local state.** Where the network genuinely was not competing for block space, recent blocks were cheap, and `reference_rate` reflects that on its own.

## 3. Permanence

Permanence measures how durable the burden is within *current* Bitcoin mechanics. No SegData, no prunable regions, no OP_RETURN2 — those do not exist and V2 must not be designed as if they will.

| Usage | Permanence | Rationale |
|---|---|---|
| Monetary transaction | 1.0 | Baseline |
| Small hash commitment (≤ 40 B) | 1.0 | Bounded, archival-only, no state burden |
| OP_RETURN payload | 1.0–1.5 | Archival, provably unspendable, no UTXO burden |
| Witness / script-path payload | 1.0–1.5 | Archival; already discounted 4:1 by consensus weight |
| UTXO bloat | 2.0–4.0 | Burdens **active state**, not just history |

The ordering is the substantive claim: **UTXO bloat outranks OP_RETURN.** An OP_RETURN output is provably unspendable and never enters the UTXO set — it costs archival bytes once. A dust output enters the UTXO set and is carried in the working state of every validating node indefinitely, possibly forever if never spent. Pricing OP_RETURN above dust would be moral filtering (OP_RETURN "looks like" data) rather than honest cost accounting.

Note the interaction with consensus weight: witness data already receives the 4:1 discount. A permanence multiplier of 1.0 on witness bytes leaves that discount intact — which is why the default `-datacarriercost=4` (1 vB/B) already represents a ~4× premium over the consensus-discounted rate for witness data. **Stacking `-tolerantpermanencepremium` on top of `-datacarriercost` would double-charge.** §6 resolves this.

### Decision: UTXO bloat is the economic headline, but only dust is priceable

The permanence ordering above stands — dust genuinely burdens active state in a way OP_RETURN does not, and V2 should say so plainly. It is also the claim that most clearly demonstrates V2 is not an anti-OP_RETURN policy wearing an economics costume: the class Tolerant prices *highest* is the one the filter debate ignores.

But the signals proposed for detecting UTXO bloat are not equally safe, and this splits the decision:

| Signal | Verdict | Reason |
|---|---|---|
| `dust_outputs` (below dust threshold) | **Priceable** | Unambiguous. A below-dust output costs more to spend than it holds, so it will likely never be spent, and every node carries it indefinitely. |
| `low-value outputs` (above dust) | Score only | "Low value" is a judgment about what the money is for. Pricing it is moral filtering. |
| `output_count` | **Score only — never priced** | See below. |
| `high output count relative to value moved` | **Score only — never priced** | Same. |

**Output count must never raise a premium, because payment batching is the single most block-space-efficient behavior in Bitcoin.** An exchange sending 100 withdrawals in one transaction consumes far less block space per payment than 100 separate transactions, and produces 100 outputs that are all well above dust and all destined to be spent. A naive output-count premium would price the most efficient users of block space as if they were the least efficient — precisely inverted, and it would be a self-inflicted wound on the credibility of the reference price (§11).

The distinguishing feature is dust, not count. A batch has many outputs, each economically spendable. Bloat has outputs that will never be economically spendable. `dust_outputs` separates these cleanly; `output_count` does not separate them at all.

Overlap to map before Phase 2: Knots already ships `-dustrelayfee` and `-subdustfeepenalty`, which address part of this at relay. V2 must not double-charge dust any more than it double-charges data bytes (§6). The likely outcome is that V2's UTXO-bloat contribution is *analytical* — surfacing the cost in the pricing model and the reference price — while the enforcement continues to live in the existing dust options.

## 4. Externality

Costs borne by neither sender nor miner: bandwidth, propagation, validation, archival burden, mempool congestion, pressure on smaller nodes.

These are real but **not locally measurable from a transaction**. A node cannot compute the bandwidth cost it imposes on a peer in Argentina. Any number here is a policy judgment, not a measurement, and the design must say so rather than dress a preference up as a calculation.

Therefore: `externality_multiplier` is a **pure operator-configured constant**, defaulting to **1.0** (no externality premium). It is never inferred, never auto-tuned, never derived from peer behavior. An operator who believes large payloads impose uncompensated network costs may raise it; the default expresses no opinion.

Two items from the instruction are **explicitly excluded** from the externality multiplier:

- **Social coordination risk** — not an economic cost of block space. Pricing it is moral filtering with an economics vocabulary.
- **Filter-fork pressure** — pricing transactions by their effect on the political position of a fork is the opposite of neutrality.

Both are dropped. Naming why matters more than the omission: if V2 prices *reputational* effects, it has abandoned "no moral filtering" while claiming to uphold it.

## 5. Data classification

New module: `src/node/tolerant_pricing.{h,cpp}`, alongside the existing `src/node/tolerant.{h,cpp}`. Classification is descriptive — it selects a permanence multiplier and nothing else. No class is ever rejected *for being* its class.

```cpp
enum class TolerantDataClass {
    MONETARY,          // no detectable arbitrary data
    SMALL_COMMITMENT,  // data present, <= small-commitment threshold
    OP_RETURN_DATA,    // output-side payload above threshold
    WITNESS_DATA,      // input/witness payload above threshold
    TAPROOT_DATA,      // script-path payload, where safely detectable
    UTXO_BLOAT,        // output-count / dust dominated
    MIXED,             // multiple sources above threshold
};

struct TolerantTxAnalysis {
    TolerantDataClass classification{TolerantDataClass::MONETARY};
    int64_t tx_vbytes{0};
    int64_t tx_weight{0};
    int64_t base_monetary_vbytes{0};   // added: never premium-multiplied
    int64_t data_vbytes{0};            // added: the premium base
    int64_t op_return_bytes{0};
    int64_t op_return_count{0};
    int64_t witness_bytes{0};
    int64_t taproot_payload_bytes{0};
    int64_t output_count{0};           // scored only — never priced (§3)
    int64_t dust_outputs{0};           // the only priceable UTXO-bloat signal (§3)
    double data_score{0.0};
    double permanence_multiplier{1.0};
    double externality_multiplier{1.0};
    double data_premium_multiplier{1.0};
};

struct TolerantPricingResult {
    CAmount actual_fee{0};
    CAmount required_fee{0};
    CFeeRate normal_feerate{};
    CFeeRate as_if_feerate{};
    int64_t economic_vbytes{0};
    bool eligible_for_template{true};  // defaults open, not closed
    std::string reason;
};
```

`base_monetary_vbytes` and `data_vbytes` are added to the proposed struct because the corrected formula (§1) needs both, and their sum-to-`tx_vbytes` relationship is a cheap internal consistency check.

**Detection reuses `DatacarrierBytes(tx, view)`** (`src/policy/policy.h:244`), which already returns `{witness_side, output_side}` bytes and already handles P2SH/witness script recovery. V2 must not write a second parser. The `view` dependency means analysis requires a `CCoinsViewCache` — available in both `AcceptToMemoryPool` and the block assembler (V1 already threads `m_template_view` for this reason, `src/node/miner.h:178`).

`TAPROOT_DATA` vs `WITNESS_DATA` is a best-effort distinction. Script-path spends are not always distinguishable from ordinary complex scripts, and **a taproot annex or a large legitimate script is not data**. Where the distinction is not safe, classify as `WITNESS_DATA`. Never guess in a way that raises a premium.

## 6. Configuration and the double-charging problem

`-datacarriercost` already applies a multiplier at mempool acceptance. If V2 also applies `permanence × externality`, data bytes are charged twice.

**Resolution: V2 multipliers are expressed relative to the rate already applied.** The effective premium for a class is:

```txt
effective_vB_per_B = (g_weight_per_data_byte / 4) × permanence × externality
```

`-datacarriercost` remains the base rate (operator-facing, unchanged, backward-compatible). V2 multipliers are *refinements per class* on top of it, defaulting to 1.0 — so **V2 default-off is byte-for-byte identical to current Knots behavior.** This is the acceptance criterion for Phase 1.

| Option | Default | Description |
|---|---|---|
| `-tolerantv2` | `0` | Master switch. Off ⇒ no V2 code path executes. |
| `-tolerantpricingmode` | `observe` | `off` / `observe` / `conservative` / `market-priced` / `opportunity-cost` |
| `-tolerantmarketexceptions` | `0` | Allow paying data-heavy txs into local templates |
| `-tolerantpermanencepremium` | `1.0` | Multiplier on the `-datacarriercost` base for data classes |
| `-tolerantexternalitypremium` | `1.0` | Operator judgment; never inferred |
| `-tolerantopportunitycost` | `average` | `average` / `marginal` / `median-block` — how the recent-block rate is reduced (§2) |
| `-tolerantreferenceblocks` | `6` | Window `N` for the recent-block reference rate — ≈ one hour of blocks (§2) |
| `-tolerantwitnesspolicy` | `observe` | |
| `-toleranttaprootpolicy` | `observe` | |
| `-tolerantutxobloatpolicy` | `observe` | |
| `-tolerantasiffeerate` | `1` | Log as-if vs normal feerate |

`-tolerantdatapremium` from the instruction is **dropped**: it is exactly `-datacarriercost`, and a second knob for the same quantity is a footgun. Operators should be told to use `-datacarriercost`. The old `static` / `template` / `mempool` opportunity-cost modes are also dropped (§2); `blockMinFeeRate` survives only as the absolute floor under the reference rate.

## 7. Phasing

### Phase 1 — Observe only

- Maintain `reference_rate` as a rolling average over the last `N` connected blocks. This is cheap: update on each `BlockConnected`/`BlockDisconnected` from block-level fee and vbyte totals the node already computes; no per-transaction UTXO lookups needed for the rate itself.
- Classify each transaction; compute `data_score`, `economic_vbytes`, `as_if_feerate`, `required_fee = economic_vbytes × reference_rate`.
- Log. Change nothing.
- **Acceptance: with `-tolerantv2=0`, zero behavioral delta — identical templates, identical mempool contents, identical block hashes given identical inputs.** Verified by running the full existing functional suite unchanged.
- Analysis is computed only when `-tolerantv2=1`. `DatacarrierBytes` walks every input and output with UTXO lookups; running it unconditionally on every mempool acceptance is a DoS-relevant cost. Where the mempool entry already carries `m_extra_weight`, reuse it rather than recomputing.

### Phase 2 — Mining template only

- Apply pricing at template construction. Do not touch `AcceptToMemoryPool`.
- Insertion point: `BlockAssembler::PassesTolerantMiningFilter` (`src/node/miner.cpp:262`), which V1 already established and which already receives `m_template_view`. Because `reference_rate` is precomputed from confirmed blocks, the filter needs no template introspection and no second assembler pass — it is a per-transaction comparison of `actual_fee` against `economic_vbytes × reference_rate`.
- **Known V1 issue to fix first:** the filter is called from `TestPackageTransactions`, whose failure path only increments `nConsecutiveFailed` under `fNeedSizeAccounting` (`src/node/miner.cpp:498`). A V2 exclusion should be a clean skip, not a signal that the block is nearly full. Confirm this before layering pricing on top, or a template full of priced-out data transactions may terminate early and under-fill the block — costing the miner more than the excluded transactions were worth.
- Market-priced exception, when `-tolerantmarketexceptions=1`: if `actual_fee >= required_fee`, the transaction is eligible. **Default off**, per the non-negotiables.

### Phase 3 — Relay policy

Optional, only after Phase 2 is stable in production. Deliberately last: relay policy that diverges from the network partitions a node's mempool from its peers', degrades compact-block reconstruction, and is the mechanism by which "local policy" becomes de-facto soft enforcement. Phase 3 should ship only with evidence from Phase 2 that the pricing model is sound, and should stay default-off.

**No phase touches consensus.** Externally mined valid blocks are always accepted regardless of pricing verdicts (`TolerantLogBlockAcceptedAbovePolicy` already exists for exactly this, `src/node/tolerant.cpp:112`).

## 8. Logging and the reference-price surface

### The deliverable is a price, not a filter

Block space is an asset issued globally and in a decentralized way. V2's purpose is to publish **the least-arbitrary available reference point for what that asset costs**, so that it is not undersold by accounting that hides the cost of data. It is not to control what confirms. If another operator wants to be more tolerant in their own template, that is theirs to choose — and V2's output should make that choice *better informed*, not harder.

This has a direct engineering consequence: **a reference price must be queryable, not merely logged.** Log lines are for debugging the node that emitted them. A price that other operators, pools, wallets, and analysts are expected to compare against needs a stable machine-readable surface.

Proposed RPC surface (Phase 1, observe-mode, read-only):

- **`gettolerantpricing`** — node-level: the current `reference_rate`, the window `N` and reduction mode (`average` / `marginal` / `median-block`) that produced it, the floor (`blockMinFeeRate`), and the configured multipliers. This is the reference price itself.
- **`gettolerantpricing <txid>`** — transaction-level: the full `TolerantTxAnalysis` and `TolerantPricingResult` for a mempool entry, including `economic_vbytes`, both feerates, `required_fee`, and the human-readable `reason`.
- **`getblocktemplate`** — *not* extended in Phase 1. It is consensus-adjacent, consumed by pool software, and adding fields there invites the pricing model being read as a rule. Keep the reference price in its own namespace where nothing mistakes it for consensus.

The RPC must report the verdict in every mode, including `observe`, and including when the verdict is "this transaction is priced correctly." A reference price that is only visible when it condemns something is not a reference price.

### Log lines

Namespace: `BCLog::TOLERANT` (already registered, `src/logging.h`). Prefix `[TolerantV2]`. Enable with `-debug=tolerant`.

```txt
[TolerantV2] tx classified as OP_RETURN_DATA (txid=..., data_vbytes=..., permanence=1.20)
[TolerantV2] economic_vbytes=X normal_vbytes=Y (txid=...)
[TolerantV2] normal_feerate=X as_if_feerate=Y (txid=...)
[TolerantV2] required_fee=X actual_fee=Y reference_rate=Z (N=6, mode=average) (txid=...)
[TolerantV2] hidden subsidy detected: pays X, economic cost Y (txid=...)
[TolerantV2] eligible under market-priced exception (txid=..., surplus=...)
[TolerantV2] excluded from local template: insufficient as-if feerate (txid=...)
[TolerantV2] external valid block accepted under consensus (block=..., height=...)
```

"Hidden subsidy detected" fires when `as_if_feerate < reference_rate` — i.e. the transaction pays below what recently confirmed block space actually cost. Because `reference_rate` is itself low when the network was quiet, this does not spuriously fire on ordinary low-fee transactions during genuinely slack periods: in a quiet market the reference falls to near the floor and a low-fee monetary transaction clears it. The log fires precisely when a transaction underpays *relative to realized recent demand* — which is the only honest definition of a subsidy.

## 9. Tests

Functional, extending `test/functional/feature_tolerant_policy.py` into a new `feature_tolerant_v2_pricing.py`:

| # | Test | Assertion |
|---|---|---|
| 1 | Monetary transaction | `economic_vbytes == tx_vbytes`; `as_if_feerate == normal_feerate` (the §1 invariant) |
| 2 | Small OP_RETURN commitment | `SMALL_COMMITMENT`; permanence 1.0; no premium |
| 3 | Large OP_RETURN, low fee | Priced; logged; **included** in observe mode |
| 4 | Large OP_RETURN, high fee | Passes pricing even in `market-priced` |
| 5 | Witness-heavy tx | Observe-only; not excluded |
| 6 | Taproot script-path payload | Observe-only; safe-detection fallback to `WITNESS_DATA` |
| 7 | UTXO bloat | Dust outputs score above equivalent OP_RETURN bytes (the §3 ordering) |
| 8 | Market exception enabled, paying tx | Included |
| 9 | Market exception disabled, paying tx | Excluded — with an explicit note that the miner forgoes real revenue |
| 10 | External block with data-heavy tx | Accepted; `[TolerantV2] external valid block accepted` |
| 11 | Chain selection | Most-work chain wins irrespective of pricing |
| 12 | Consensus | No validation change; `-tolerantv2=0` produces byte-identical behavior |
| 13 | **Quiet network, data-heavy tx** | After N cheap blocks `reference_rate` sits near the floor; a data tx paying that floor is not flagged as subsidized (§2) |
| 14 | **Double-charging** | `-datacarriercost=2` + `permanence=1.0` ⇒ effective rate is 2 vB/B, not 4 |
| 15 | **Reference rate tracks recent blocks** | With N blocks averaging R sat/vB, `reference_rate == R` (floored at minrelayfee); `required_fee == economic_vbytes × R` (§2) |
| 16 | **Payment batching is not bloat** | A 100-output batch, all outputs above dust, gets **no** premium and is **not** classified `UTXO_BLOAT` (§3) |
| 17 | **Reference price is queryable** | `gettolerantpricing` returns a verdict in `observe` mode, including for correctly-priced txs (§8) |

Tests 13–17 are additions, and each pins a specific way honest pricing could decay back into filtering. Test 16 is the load-bearing one for the §3 decision: it fails the moment anyone reintroduces an output-count premium.

Unit tests in `src/test/`: multiplier arithmetic, `base + data == tx_vbytes` consistency, classification boundaries, overflow on adversarial `economic_vbytes` (a large payload × a large operator-set multiplier must saturate, mirroring the existing `int32_t` clamp in `CalculateExtraTxWeight`, `src/policy/policy.cpp:610`).

## 10. Non-negotiables

- No consensus validation changes.
- No BIP-110 enforcement. No SegData. No prunable regions. No new opcodes. No pruning.
- No chain-selection change. Most-work valid chain always wins.
- No rejection of externally valid blocks. Local policy is never consensus invalidity.
- Market-priced exceptions are never default-on.
- Local policy never filters by content meaning — only by economic cost.
- Scarcity is measured only from realized recent-block prices, never fabricated from local state (§2).
- Output count is never priced; payment batching is never penalized (§3).
- The externality multiplier is never inferred, auto-tuned, or derived from peer behavior (§4).

## 11. Resolved questions

**1. Is the reference rate honest under a quiet network? — Resolved: yes, and it self-scales.**

The scarcity reference is the realized average feerate of the last `N` confirmed blocks (§2), not anything measured from the local mempool. So the honesty question changes shape: there is no empty-mempool failure mode to reason about, because the model never reads the mempool. When the *network* was quiet, recent blocks were cheap, and `reference_rate` falls on its own toward the minimum relay feerate. When the network was busy, it rises. Competition is encoded in the reference, automatically.

This is strictly better than the local-displacement model an earlier draft used. If the local mempool is empty but the network is busy, the honest price is still high — and only a network-sourced reference can report that. The local model would have said "nothing displaced, price at zero" and mispriced exactly when block space was most contested.

The one honest caveat is the mean-vs-marginal choice (§2): using the average of recent blocks sets the bar slightly above true opportunity cost. For a project whose stated purpose is to keep block space scarce, that conservative bias is intended, and `-tolerantopportunitycost=marginal` is offered for operators who want the strict opportunity-cost bar.

**2. Should `UTXO_BLOAT` be V2's headline instead of OP_RETURN? — Resolved: yes economically, but only dust is priceable.**

The permanence ordering in §3 stands: dust burdens active state indefinitely, OP_RETURN costs archival bytes once. Ranking OP_RETURN higher would be filtering on what data looks like.

But enforcement is limited to `dust_outputs`. `output_count` is scored and never priced, because payment batching is the most block-space-efficient behavior in Bitcoin and an output-count premium would price the most efficient users as the least efficient (§3). Given the overlap with the existing `-dustrelayfee` and `-subdustfeepenalty`, V2's UTXO-bloat contribution is expected to be analytical rather than enforcing.

**3. Does template-only pricing accomplish anything at low hashrate share? — Resolved: the deliverable is the price, not the template.**

Block space is an asset issued globally and in a decentralized way. V2 exists to publish the least-arbitrary available reference point for its cost, so the asset is not undersold by accounting that conceals what data actually consumes. Whether the node's own template excludes anything is close to irrelevant at low hashrate share — and that is fine, because the template was never the product. Another operator who chooses to be more tolerant in their own template is exercising exactly the freedom Tolerant defends; V2's job is to make that choice better informed, not harder.

The reference-rate model (§2) reinforces this: because the price is derived from confirmed blocks that every node sees, *every* Tolerant node computes the same reference from the same data, independent of its hashrate or its local mempool. The reference is a network-wide quantity that a one-CPU node can publish as credibly as a large pool. That is what makes it a reference rather than a local preference.

Two consequences, both now in the design:

- **The price must be queryable.** §8 adds a `gettolerantpricing` RPC surface. Logs are for debugging the emitting node; a reference price needs a stable machine-readable form, and must report verdicts even when nothing is condemned.
- **A reference price is only as authoritative as its inputs are unbiased.** The reference-block model resolves the filtered-lens problem structurally: it sources the rate from confirmed blocks, not the V1-filtered local mempool, so strict relay can no longer bias the measurement downward. This is the deciding reason it replaced the template-introspection model outright.

It also raises the bar for every constant in this document. A number that only has to justify a local template can be a preference. A number published as a reference price must be defensible to people who disagree with the project. This is why `-tolerantexternalitypremium` defaults to `1.0` (§4), why social-coordination and filter-fork pressure are excluded outright, and why §3 refuses to price output count.

## 12. Remaining open question

**The window `N` and its reduction.** The reference rate needs a chosen number of blocks (`-tolerantreferenceblocks`, default **6** — about one hour) and a reduction (`average` default; `marginal` and `median-block` offered). This is the one irreducible judgment call left in the scarcity model, and the design is honest that it is a judgment, not a measurement. Three things pin it down enough to defend:

1. **Constant, configurable, never self-adjusting.** `N` is a visible operator knob, not an adaptive controller. An adaptive window would replace one honest, arguable number with several buried ones (how fast to adapt, on what volatility threshold, with what smoothing) — the opposite of "the least-arbitrary reference possible." Note the distinction: the *rate* is adaptive (it rolls forward with every new block); the *window size* is fixed.
2. **The range is principled even though the point is not.** The anti-subsidy purpose disqualifies large `N`: a week-long window prices data at last week's rate during a spike, i.e. subsidizes it precisely when block space is dearest. Noise and manipulability disqualify `N` of 1–2. That leaves ~6–144 blocks (one hour to one day), and within it the exact value is a preference stated as such.
3. **`N = 6` is chosen for its economic horizon, not its magnitude.** "Roughly the last hour of confirmed blocks" is an answer that survives scrutiny; "ten" is not. Short-and-current beats long-and-smooth here because the harm is asymmetric — under-pricing leaks subsidy, over-pricing is merely conservative.

Ship `observe`-mode first, so operators can watch the published `reference_rate` against their own read of the fee market before any Phase 2 exclusion depends on it. Two refinements are documented but **deferred, not adopted**, because each only *relocates* the judgment call (to a half-life or a duration) rather than removing it, at the cost of more math: a **time-based window** ("last 2 hours" instead of 6 blocks), which trades block-count variance for dependence on miner-influenced timestamps; and an **EWMA** decay weighting, which smooths the hard cutoff but substitutes "why this half-life?" for "why this `N`?". The simple constant window is preferred for honesty and simplicity.
