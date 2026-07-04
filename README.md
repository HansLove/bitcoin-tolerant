Bitcoin Tolerant
================

**Strict in relay. Conservative in mining. Tolerant in consensus.**

Bitcoin Tolerant is an independent Bitcoin full-node implementation with a conservative relay and mining policy. It is a fork of [Bitcoin Knots](https://bitcoinknots.org/) (which derives from Bitcoin Core), not a contribution or variant intended to merge back upstream.

Repository: https://github.com/HansLove/bitcoin-tolerant

## Philosophy

- **Relay:** Reject or avoid relaying transactions with excessive arbitrary data (OP_RETURN and related carriers).
- **Mining:** Exclude those transactions from locally generated block templates.
- **Consensus:** Accept any block that is valid under Bitcoin consensus and on the most-work chain.

**Prefer clean blocks in a tie. Follow the most-work valid chain once the tie is broken.**

## Attribution

Bitcoin Tolerant is derived from Bitcoin Knots, which is derived from Bitcoin Core. Bitcoin Knots and Bitcoin Core are released under the MIT License.

Bitcoin Tolerant is an independent project and is not affiliated with or endorsed by Bitcoin Core or Bitcoin Knots maintainers.

## Build

```bash
cmake -B build -DRDTS_CONSENT=UNSUPPORTED_UNSAFE_NO_ENFORCEMENT
cmake --build build
```

Binaries remain `bitcoind`, `bitcoin-cli`, and `bitcoin-qt` for compatibility. The client identifies itself as **Bitcoin Tolerant** in the GUI, logs, and about dialog.

## Documentation

- [README-TOLERANT.md](README-TOLERANT.md) — project overview
- [doc/tolerant-policy.md](doc/tolerant-policy.md) — Technical Policy V1
- [contrib/tolerant.conf.example](contrib/tolerant.conf.example) — example configuration

## Tests

```bash
build/test/functional/test_runner.py feature_tolerant_policy.py
```

## License

Released under the MIT license. See [COPYING](COPYING).
