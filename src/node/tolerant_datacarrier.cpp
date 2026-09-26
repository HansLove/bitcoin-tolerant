// Copyright (c) 2026 The Bitcoin Tolerant developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/tolerant_datacarrier.h>

#include <coins.h>
#include <consensus/consensus.h>   // WITNESS_SCALE_FACTOR
#include <primitives/transaction.h>
#include <script/interpreter.h>    // EvalScript, WITNESS_V0_SCRIPTHASH_SIZE, WITNESS_V1_TAPROOT_SIZE

TolerantDatacarrierBytes ScanScriptForDatacarrier(const CScript& script, bool is_witness_location)
{
    TolerantDatacarrierBytes result;

    size_t counted{0};
    opcodetype opcode, last_opcode{OP_INVALIDOPCODE};
    std::vector<unsigned char> push_data;
    unsigned int inside_noop{0}, inside_conditional{0};
    CScript::const_iterator opcode_it = script.begin(), data_began = script.begin();

    for (CScript::const_iterator it = script.begin(); it < script.end(); last_opcode = opcode) {
        opcode_it = it;
        if (!script.GetOp(it, opcode, push_data)) {
            // A script that fails to parse is, for our purposes, entirely data.
            if (is_witness_location) result.witness_envelope_bytes = script.size();
            else result.base_envelope_bytes = script.size();
            return result;
        }

        if (opcode == OP_IF || opcode == OP_NOTIF) {
            ++inside_conditional;
        } else if (opcode == OP_ENDIF) {
            if (inside_conditional == 0) {
                // Malformed (unbalanced OP_ENDIF): treat as opaque data.
                if (is_witness_location) result.witness_envelope_bytes = script.size();
                else result.base_envelope_bytes = script.size();
                return result;
            }
            --inside_conditional;
        } else if (opcode == OP_RETURN && inside_conditional == 0) {
            // Unconditional OP_RETURN: provably unspendable, archival,
            // already priced at full weight via the base transaction bytes.
            result.op_return_bytes = script.size();
            return result;
        }

        // Match the inscription/ordinal envelope: OP_FALSE OP_IF <data> OP_ENDIF.
        // The branch never executes, but the bytes are serialized and stored.
        if (inside_noop) {
            switch (opcode) {
            case OP_IF:
            case OP_NOTIF:
                ++inside_noop;
                break;
            case OP_ENDIF:
                if (--inside_noop == 0) {
                    counted += static_cast<size_t>(it - data_began) + 1;
                }
                break;
            default:
                break;
            }
        } else if (opcode == OP_IF && last_opcode == OP_FALSE) {
            inside_noop = 1;
            data_began = opcode_it;
        // Match the "<data> OP_DROP" pattern: a push whose only purpose is to
        // be discarded, another common way to carry bytes without effect.
        } else if (opcode <= OP_PUSHDATA4) {
            data_began = opcode_it;
        } else if (opcode == OP_DROP && last_opcode <= OP_PUSHDATA4) {
            counted += static_cast<size_t>(it - data_began);
        }
    }

    if (is_witness_location) result.witness_envelope_bytes = counted;
    else result.base_envelope_bytes = counted;
    return result;
}

TolerantResolvedInputScript ResolveSpentScript(const CScript& prev_script, const CTxIn& txin,
                                               const CScriptWitness& witness)
{
    CScript script = prev_script;
    bool p2sh{false};

    if (script.IsPayToScriptHash()) {
        std::vector<std::vector<unsigned char>> stack;
        if (!EvalScript(stack, txin.scriptSig, SCRIPT_VERIFY_NONE, BaseSignatureChecker(), SigVersion::BASE) ||
            stack.empty()) {
            return {};
        }
        script = CScript(stack.back().begin(), stack.back().end());
        p2sh = true;
    }

    int witnessversion{0};
    std::vector<unsigned char> witnessprogram;
    if (!script.IsWitnessProgram(witnessversion, witnessprogram)) {
        // Legacy spend: the real script is the redeemScript (P2SH) or the
        // scriptSig itself. Either way, non-witness -- full price today.
        return {p2sh ? script : txin.scriptSig, true, false};
    }

    const auto& stack = witness.stack;
    if (witnessversion == 0 && witnessprogram.size() == WITNESS_V0_SCRIPTHASH_SIZE) {
        if (stack.empty()) return {};
        return {CScript(stack.back().begin(), stack.back().end()), true, true};
    }
    if (witnessversion == 1 && witnessprogram.size() == WITNESS_V1_TAPROOT_SIZE && !p2sh) {
        size_t n = stack.size();
        if (n >= 2 && !stack[n - 1].empty() && stack[n - 1][0] == ANNEX_TAG) {
            --n; // Drop the annex.
        }
        if (n >= 2) {
            --n; // Drop the control block.
            return {CScript(stack[n - 1].begin(), stack[n - 1].end()), true, true};
        }
        // Key-path spend: no script at all, nothing to scan.
        return {CScript(), true, true};
    }

    // Unknown witness version/program: nothing we can safely scan.
    return {};
}

TolerantDatacarrierBytes ComputeTxDatacarrierBytes(const CTransaction& tx, const CCoinsViewCache& view)
{
    TolerantDatacarrierBytes total;

    for (const CTxIn& txin : tx.vin) {
        const Coin& coin = view.AccessCoin(txin.prevout);
        if (coin.IsSpent()) continue; // Unknown prevout (e.g. package-relay context); skip rather than guess.
        const auto resolved = ResolveSpentScript(coin.out.scriptPubKey, txin, txin.scriptWitness);
        if (!resolved.resolved || resolved.script.empty()) continue;
        const auto dcb = ScanScriptForDatacarrier(resolved.script, resolved.is_witness_location);
        total.op_return_bytes += dcb.op_return_bytes;
        total.base_envelope_bytes += dcb.base_envelope_bytes;
        total.witness_envelope_bytes += dcb.witness_envelope_bytes;
    }
    for (const CTxOut& txout : tx.vout) {
        const auto dcb = ScanScriptForDatacarrier(txout.scriptPubKey, /*is_witness_location=*/false);
        total.op_return_bytes += dcb.op_return_bytes;
        total.base_envelope_bytes += dcb.base_envelope_bytes;
        total.witness_envelope_bytes += dcb.witness_envelope_bytes;
    }
    return total;
}
