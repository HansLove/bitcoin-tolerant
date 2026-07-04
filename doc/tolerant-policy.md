# Tolerant Knots — Technical Policy V1

## Summary

Tolerant Knots implements a three-layer policy:

| Layer | Behavior |
|-------|----------|
| Relay / mempool | Strict — reject transactions above configured arbitrary-data limits |
| Mining templates | Conservative — exclude policy-violating transactions from local templates |
| Block validation | Tolerant — accept consensus-valid blocks on the most-work chain |

## Guiding rules

1. **Strict in relay. Conservative in mining. Tolerant in consensus.**
2. **Prefer clean blocks in a tie. Follow the most-work valid chain once the tie is broken.**

V1 logs equal-work clean-block preferences but does **not** change chain selection (`toleranttiepreference=0` by default).

## Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `tolerant` | `1` | Enable the Tolerant profile (`datacarrier=1`, `datacarriersize=83`, `datacarrierfullcount=1`, `permitbaredatacarrier=0`) |
| `tolerantdatacarriersize` | `83` | Convenience alias for `datacarriersize` when not set explicitly |
| `tolerantlogpolicy` | `1` | Log Tolerant policy decisions (`-debug=tolerant`) |
| `tolerantminingfilter` | `1` | Exclude policy-violating txs from local block templates |
| `toleranttiepreference` | `0` | Reserved; V1 is observation-only |

Explicit `-datacarriersize` overrides `-tolerantdatacarriersize`.

Set `tolerant=0` to disable the profile without changing other settings.

## RDTS / BIP110

Tolerant Knots builds should use:

```bash
cmake -DRDTS_CONSENT=UNSUPPORTED_UNSAFE_NO_ENFORCEMENT ...
```

This disables RDTS consensus enforcement on mainnet while preserving standard Bitcoin consensus. To opt in:

```ini
consensusrules=rdts
```

## Logging examples

Enable with `-debug=tolerant` (and `-tolerantlogpolicy=1`):

```txt
[Tolerant] Transaction rejected from mempool: OP_RETURN/arbitrary data exceeds policy limit ...
[Tolerant] Transaction excluded from block template: excessive arbitrary data ...
[Tolerant] Received valid block with data above local policy. Accepting under consensus rules ...
[Tolerant] Equal-work fork observed. Clean block preference would select block ...
[Tolerant] Most-work chain selected. Following Nakamoto consensus ...
```

## Clean block score (V1)

Each connected block is scored (observation only):

- OP_RETURN output count
- Total OP_RETURN bytes
- Transactions exceeding local policy (output-side estimate)
- Total arbitrary-data bytes

Lower score means a cleaner block.

## What V1 does NOT do

- No consensus rule changes
- No block rejection by local policy
- No chain-selection override by data content
- No UASF-style enforcement
- No binary renames (`bitcoind`, `bitcoin-cli`, `bitcoin-qt` unchanged)

## Build

```bash
cmake -B build -DRDTS_CONSENT=UNSUPPORTED_UNSAFE_NO_ENFORCEMENT
cmake --build build
./build/test/functional/test_runner.py feature_tolerant_policy.py
```

## Attribution

Tolerant Knots is derived from Bitcoin Knots, which is derived from Bitcoin Core. Bitcoin Knots and Bitcoin Core are released under the MIT License.

Tolerant Knots is an independent project and is not affiliated with or endorsed by Bitcoin Core or Bitcoin Knots maintainers.
