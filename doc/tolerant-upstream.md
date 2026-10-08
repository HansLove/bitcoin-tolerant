# Bitcoin Tolerant and upstream Bitcoin Core

Bitcoin Tolerant follows Bitcoin Core release by release. It does not accept
every change automatically, and it does not drift away out of habit. It
diverges in policy, not in code: Core's code stays as close to upstream as
possible, and Tolerant's identity lives in three layers it owns.

| Layer | Where |
|---|---|
| Honest pricing | `src/node/tolerant_*`, `src/rpc/tolerant.cpp` |
| Defaults that differ from Core | `src/node/tolerant_defaults.cpp` |
| Decisions about upstream changes | this document |

Keeping Core's code intact is what makes each upgrade cheap. Writing every
divergence down, with its reason, is what makes the independence credible.

## Principles

Every decision below is checked against these. A Core change that violates
none of them is accepted.

1. **Consensus is never touched.** Bitcoin Tolerant validates exactly the same
   blocks as Bitcoin Core and follows the same most-work chain.
2. **Block space is priced, not filtered.** Data is never rejected or penalized
   for what it means, only priced for the space it takes.
3. **The price comes from confirmed blocks.** The reference rate is measured
   from the network, never from this node's own mempool or relay choices.
4. **Payments never pay a premium.** A monetary transaction's economic size is
   its real size.
5. **The operator decides.** Every Tolerant default can be changed in
   `bitcoin.conf`. With `-tolerantv2=0` the node is plain Bitcoin Core.

## Triage

| Tier | Changes | Decision |
|---|---|---|
| 1 | Consensus and security: validation rules, script interpreter, CVE fixes | Always accepted, unmodified (principle 1) |
| 2 | Infrastructure: wallet, RPC, P2P, build, performance, GUI | Accepted by default |
| 3 | Block-space policy: standardness, data carriers, relay fees, mempool limits, block templates | Reviewed against the principles |

Only tier 3 needs judgment. A tier-3 change ends in one of three outcomes:

- **Accept.** It doesn't conflict with the principles.
- **Accept, override the default.** Core's code is kept; Tolerant sets a
  different default in `src/node/tolerant_defaults.cpp`, citing the
  principle. Operators can still choose Core's value.
- **Adapt.** Tolerant's own modules change to work with Core's new behavior.

Reverting or patching Core's policy code is a last resort, and requires a
written case here explaining why a default override is not enough.

## Upgrading to a new Core release

Bitcoin Tolerant builds on release tags only, never on `master`. Versions are
named after their base: `v32.0-tolerant.1`, `v32.0-tolerant.2`, …

1. Generate the review:
   ```sh
   contrib/tolerant/upstream-review.sh <current-core-tag> <new-core-tag> > review.md
   ```
   It lists changed policy defaults, the flagged tier-3 PRs, consensus PRs,
   and the Core files where the rebase will conflict.
2. Decide every changed default and every flagged PR. Record the outcome in
   the decision log below.
3. Rebase the Tolerant commits onto the new tag. Conflicts should appear only
   in the files listed under "Rebase risk".
4. Build and run the Tolerant tests:
   ```sh
   build/bin/test_bitcoin --run_test=tolerant_defaults_tests,tolerant_pricing_tests,tolerant_reference_rate_tests,tolerant_reference_rate_chain_tests,tolerant_template_tests,miner_tests
   build/test/functional/feature_tolerant_pricing.py
   build/test/functional/feature_tolerant_template.py
   ```
5. Tag the release once the base tag is final, not a release candidate.

## Default overrides in effect

None. No Bitcoin Core default currently conflicts with the principles.

Bitcoin Core v30 raised the default OP_RETURN relay limit to 100,000 vbytes.
That does not conflict with principle 2: Bitcoin Tolerant prices that data
instead of filtering it, so Core's permissive default stands.

## Decision log

### v30.3 → v32.0rc3 (`tolerant-core-v32`)

4,520 upstream commits. Generated with `upstream-review.sh v30.3 v32.0rc3`.

**Changed policy defaults: all accepted.**

| Change | Decision |
|---|---|
| Ancestor/descendant size limits replaced by cluster limits (`DEFAULT_CLUSTER_LIMIT` 64, `DEFAULT_CLUSTER_SIZE_LIMIT_KVB` 101) | Accept. Mempool structure, not a judgment about data. |
| `DEFAULT_COINBASE_OUTPUT_MAX_ADDITIONAL_SIGOPS` 400 (new) | Accept. Block template accounting. |
| `MAX_PREVOUTFETCH_THREADS`, `DEFAULT_PRINT_MODIFIED_FEE` | Accept. Performance and logging. |
| `MIN_DISK_SPACE_FOR_BLOCK_FILES` | No change: same value, rewritten as `550_MiB`. |

`DEFAULT_ACCEPT_DATACARRIER` and `MAX_OP_RETURN_RELAY` are unchanged.

**Flagged tier-3 PRs: all accepted, one adaptation required.**

| PR | Decision |
|---|---|
| #33629, #33591, #34616 Cluster mempool | **Accept, adapt.** Block templates are now built from cluster chunks; `addPackageTxs` no longer exists. Tolerant's Phase 2 hooks into `BlockAssembler::addChunks()` and prices each chunk as a unit, skipping with Core's own `SkipBuilderChunk()`. Done; Phase 2 exists only on the v32 base. |
| #33453 Undeprecate `-datacarrier` / `-datacarriersize` | Accept. Operators keep the option. Tolerant does not use it to filter (principle 2) and leaves its default alone. |
| #33892 Allow sub-minrelay transactions in a package when CPFP pays for them | Accept. Judging a package by what it pays in total is consistent with pricing by economic cost. The reference-rate floor is unaffected. |
| #33199 Fee estimator returns sub-1 sat/vB estimates | Accept. Tolerant's price does not use the fee estimator (principle 3). |
| #34552 Feerate format separated from estimate mode | Accept, adapted: `FeeEstimateMode::SAT_VB` → `FeeRateFormat::SAT_VB`. |
| #33475 Miner `addPackageTxs` overflow fix | Accept. Bug fix, superseded by cluster mempool. |
| #33504 TRUC checks skipped on reorg | Accept. |
| #34184 `createNewBlock()` cooldown after IBD | Accept. |

**Consensus PRs:** accepted unmodified (tier 1). Includes the removal of the
Taproot BIP 9 deployment (#26201) and 64-bit script verification flags (#32998).

**Other API adaptations in Tolerant's own code:** `ChainstateRole` became a
struct (`role.historical`), `LogAcceptCategory` became
`util::log::ShouldDebugLog`, `RPCHelpMan` became `RPCMethod`, and log
categories moved to `src/logging/categories.h`, with `TOLERANT` on bit 31
because upstream assigned bit 29 to `PRIVBROADCAST`.

**Status:** not tagged. The base is a release candidate; tag
`v32.0-tolerant.1` after rebasing onto the final `v32.0`.
