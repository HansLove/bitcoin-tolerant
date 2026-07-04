# Bitcoin Tolerant — Project Overview

**Website:** [bitcointolerant.com](https://bitcointolerant.com)  
**Source code:** [github.com/HansLove/bitcoin-tolerant](https://github.com/HansLove/bitcoin-tolerant)

This repository contains the **Bitcoin Tolerant** full node — an independent fork of [Bitcoin Knots](https://bitcoinknots.org/) implementing Technical Policy V1.

For the primary introduction, quick start, and links, see **[README.md](README.md)**.

## Philosophy

**Strict in relay. Conservative in mining. Tolerant in consensus.**

- **Relay:** Reject or avoid relaying transactions with excessive arbitrary data.
- **Mining:** Exclude those transactions from locally generated block templates.
- **Consensus:** Accept any block valid under Bitcoin consensus on the most-work chain.

**Prefer clean blocks in a tie. Follow the most-work valid chain once the tie is broken.**

## Technical Policy V1

Full specification: [doc/tolerant-policy.md](doc/tolerant-policy.md)

V1 focuses on:

1. Conservative default configuration (`-tolerant=1`)
2. Mempool / relay policy and logging
3. Mining template filtering
4. Clean-block scoring (observation only)
5. Documentation and regtest coverage

V1 explicitly does **not** change Bitcoin consensus rules, override proof-of-work chain selection, or enforce BIP110/RDTS by default.

## Build flag

Tolerant builds use:

```bash
cmake -DRDTS_CONSENT=UNSUPPORTED_UNSAFE_NO_ENFORCEMENT ...
```

## Attribution

Bitcoin Tolerant is derived from Bitcoin Knots, which is derived from Bitcoin Core.  
MIT License. Not affiliated with or endorsed by Bitcoin Core or Bitcoin Knots maintainers.
