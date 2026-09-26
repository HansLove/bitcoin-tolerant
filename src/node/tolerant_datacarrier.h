// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Bitcoin Tolerant's own datacarrier detector.
//
// This is a from-scratch implementation, written as free functions against
// vanilla Bitcoin Core's CScript/CTransaction -- it does NOT patch Core's
// script.{h,cpp} or policy.{h,cpp}. That is a deliberate architectural
// choice: keeping all Tolerant-specific detection logic in Tolerant-owned
// files means syncing with upstream Core releases stays close to friction
// free. See doc/tolerant-v2-pricing.md.
//
// What it detects, and why the split matters for pricing:
//   - An unconditional OP_RETURN output is provably unspendable, archival,
//     and already serialized in the transaction's base (non-witness) bytes
//     -- it already costs full weight today. No discount to remove.
//   - An "envelope" is a script-structure pattern that carries bytes without
//     affecting execution: OP_FALSE OP_IF <data> OP_ENDIF (the inscription/
//     ordinal pattern), or a data push immediately followed by OP_DROP. When
//     this pattern lives inside a WITNESS location (a segwit v0 witness
//     script or a taproot script-path spend), those bytes cost only 1 weight
//     unit/byte at consensus instead of 4 -- the segwit discount, designed
//     for signatures, that V2 exists to stop subsidizing.

#ifndef BITCOIN_NODE_TOLERANT_DATACARRIER_H
#define BITCOIN_NODE_TOLERANT_DATACARRIER_H

#include <script/script.h>

#include <cstddef>

class CCoinsViewCache;
class CTransaction;
class CTxIn;

/** Datacarrier bytes found in a transaction, split by whether they already
 *  pay full consensus weight (op_return_bytes, base_envelope_bytes) or
 *  benefit from the witness discount today (witness_envelope_bytes). */
struct TolerantDatacarrierBytes {
    size_t op_return_bytes{0};        //!< Unconditional OP_RETURN; already full price.
    size_t base_envelope_bytes{0};    //!< Envelope/push-drop in a non-witness location; already full price.
    size_t witness_envelope_bytes{0}; //!< Envelope/push-drop in a witness location; today discounted 4:1.

    size_t Total() const { return op_return_bytes + base_envelope_bytes + witness_envelope_bytes; }
};

/** Scan a single script's structure for datacarrier content. Does not
 *  execute the script -- only walks its opcodes. Pure function. */
TolerantDatacarrierBytes ScanScriptForDatacarrier(const CScript& script, bool is_witness_location);

/** The script actually being spent by a transaction input, resolved through
 *  P2SH and witness-program indirection, and whether that location is a
 *  witness location (i.e. already discounted 4:1 at consensus today). */
struct TolerantResolvedInputScript {
    CScript script;
    bool resolved{false};
    bool is_witness_location{false};
};
TolerantResolvedInputScript ResolveSpentScript(const CScript& prev_script, const CTxIn& txin,
                                               const CScriptWitness& witness);

/** Total datacarrier bytes across every input and output of `tx`. Reads
 *  `view` only for input prevout scripts. */
TolerantDatacarrierBytes ComputeTxDatacarrierBytes(const CTransaction& tx, const CCoinsViewCache& view);

#endif // BITCOIN_NODE_TOLERANT_DATACARRIER_H
