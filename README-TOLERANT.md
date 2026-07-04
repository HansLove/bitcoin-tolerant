# Tolerant Knots

**Strict in relay. Conservative in mining. Tolerant in consensus.**

Tolerant Knots is a conservative Bitcoin node policy built on [Bitcoin Knots](https://bitcoinknots.org/). It helps operators run stricter relay and mining rules without creating a hard fork or rejecting valid blocks on the most-work chain.

## Philosophy

- **Relay:** Reject or avoid relaying transactions with excessive arbitrary data (OP_RETURN and related carriers).
- **Mining:** Exclude those transactions from locally generated block templates.
- **Consensus:** Accept any block that is valid under Bitcoin consensus and on the most-work chain.

**Prefer clean blocks in a tie. Follow the most-work valid chain once the tie is broken.**

## Attribution

Tolerant Knots is derived from Bitcoin Knots, which is derived from Bitcoin Core. Bitcoin Knots and Bitcoin Core are released under the MIT License.

Tolerant Knots is an independent project and is not affiliated with or endorsed by Bitcoin Core or Bitcoin Knots maintainers.

## Build

```bash
cmake -B build -DRDTS_CONSENT=UNSUPPORTED_UNSAFE_NO_ENFORCEMENT
cmake --build build
```

`RDTS_CONSENT=UNSUPPORTED_UNSAFE_NO_ENFORCEMENT` disables BIP110/RDTS consensus enforcement by default, matching the Tolerant V1 philosophy. Operators may opt in with `consensusrules=rdts` in `bitcoin.conf`.

## Default configuration

See [contrib/tolerant.conf.example](contrib/tolerant.conf.example) and [doc/tolerant-policy.md](doc/tolerant-policy.md).

## Tests

```bash
build/test/functional/test_runner.py feature_tolerant_policy.py
```

## Further reading

- [doc/tolerant-policy.md](doc/tolerant-policy.md) — full V1 policy specification
