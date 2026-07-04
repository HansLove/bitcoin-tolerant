# Bitcoin Tolerant

**Strict in relay. Conservative in mining. Tolerant in consensus.**

| | |
|---|---|
| **Website** | [bitcointolerant.com](https://bitcointolerant.com) |
| **Repository** | [github.com/HansLove/bitcoin-tolerant](https://github.com/HansLove/bitcoin-tolerant) |
| **Current version** | `v29.3.tolerant1` (Technical Policy V1) |
| **Report issues** | [GitHub Issues](https://github.com/HansLove/bitcoin-tolerant/issues) |

---

## What is Bitcoin Tolerant?

**Bitcoin Tolerant** is a full-node Bitcoin implementation with a conservative **relay and mining policy**. It helps operators, miners, and integrators run stricter local rules against excessive arbitrary data (for example large `OP_RETURN` payloads) **without creating a hard fork** and **without rejecting valid blocks on the chain with the most proof-of-work**.

It is an **independent fork** of [Bitcoin Knots](https://bitcoinknots.org/) (which derives from Bitcoin Core). This project is **not** a pull request to Knots and **not** affiliated with Bitcoin Core or Bitcoin Knots maintainers.

> Bitcoin does not need more unnecessary forks. It needs stronger consensus.

Learn more on the project website: **[bitcointolerant.com](https://bitcointolerant.com)**

---

## The core idea

Operators should remain sovereign over what they relay and mine. But **local policy should not automatically become a reason to fracture Nakamoto consensus**.

| Layer | Bitcoin Tolerant behavior |
|-------|---------------------------|
| **Relay** | Strict — reject or avoid relaying transactions above your configured arbitrary-data limits |
| **Mining** | Conservative — exclude those transactions from **your** block templates |
| **Consensus** | Tolerant — accept any block that is valid under Bitcoin consensus on the most-work chain |

### The rule

**Prefer clean blocks in a tie. Follow the most-work valid chain once the tie is broken.**

In V1, equal-work clean-block preference is **logged for observation only**; chain selection is never overridden by data content.

---

## What Bitcoin Tolerant is not

- Not a hard fork
- Not a mandatory new consensus rule in V1
- Not a UASF-style enforcement mechanism by default
- Not an attempt to replace or merge into Bitcoin Knots upstream
- Not official Bitcoin Core or Bitcoin Knots software

RDTS (BIP110) consensus enforcement is **off by default** in Tolerant builds. Operators may opt in with `consensusrules=rdts` in `bitcoin.conf`.

---

## Quick start

### Requirements

- C++20 toolchain, CMake 3.22+, and standard Bitcoin/Knots build dependencies  
- See [doc/build-unix.md](doc/build-unix.md) for platform-specific notes

### Build

```bash
git clone https://github.com/HansLove/bitcoin-tolerant.git
cd bitcoin-tolerant
git checkout main

cmake -B build -DRDTS_CONSENT=UNSUPPORTED_UNSAFE_NO_ENFORCEMENT
cmake --build build
```

Executables: `build/bin/bitcoind`, `build/bin/bitcoin-cli`, and optionally `build/bin/bitcoin-qt`.  
The running client identifies itself as **Bitcoin Tolerant** in the GUI, logs, and about dialog.

### Configure (V1 defaults)

Copy the example config and adjust as needed:

```bash
cp contrib/tolerant.conf.example ~/.bitcoin/bitcoin.conf
```

Key options:

```ini
tolerant=1
tolerantdatacarriersize=83
tolerantlogpolicy=1
tolerantminingfilter=1
toleranttiepreference=0
```

Enable policy logging with `-debug=tolerant`.

### Run

```bash
./build/bin/bitcoind
./build/bin/bitcoin-cli getblockchaininfo
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [doc/tolerant-policy.md](doc/tolerant-policy.md) | Full Technical Policy V1 specification |
| [contrib/tolerant.conf.example](contrib/tolerant.conf.example) | Commented example configuration |
| [README-TOLERANT.md](README-TOLERANT.md) | Extended project overview |

---

## Testing

```bash
build/test/functional/test_runner.py feature_tolerant_policy.py
```

---

## Lineage & license

Bitcoin Tolerant is derived from **Bitcoin Knots**, which is derived from **Bitcoin Core**.  
Bitcoin Knots and Bitcoin Core are released under the MIT License.

Bitcoin Tolerant is an independent project and is **not** affiliated with or endorsed by Bitcoin Core or Bitcoin Knots maintainers.

Upstream reference (read-only): [bitcoinknots/bitcoin](https://github.com/bitcoinknots/bitcoin)

This project is released under the MIT license. See [COPYING](COPYING).

---

## Support the project

Visit **[bitcointolerant.com](https://bitcointolerant.com)** for the manifesto, policy overview, and ways to support the initiative.
