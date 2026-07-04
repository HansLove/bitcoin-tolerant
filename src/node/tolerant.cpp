// Copyright (c) 2026 The Tolerant Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/tolerant.h>

#include <chain.h>
#include <coins.h>
#include <common/args.h>
#include <consensus/consensus.h>
#include <logging.h>
#include <policy/policy.h>
#include <script/script.h>

namespace {
bool g_tolerant{DEFAULT_TOLERANT};
bool g_tolerant_log_policy{DEFAULT_TOLERANT_LOG_POLICY};
bool g_tolerant_mining_filter{DEFAULT_TOLERANT_MINING_FILTER};
bool g_tolerant_tie_preference{DEFAULT_TOLERANT_TIE_PREFERENCE};
size_t g_tolerant_datacarrier_limit{DEFAULT_TOLERANT_DATACARRIER_SIZE};

bool IsDatacarrierRejectReason(const std::string& reason)
{
    return reason == "scriptpubkey" ||
           reason == "txn-datacarrier-exceeded" ||
           reason == "txn-datacarrier-nonstandard" ||
           reason == "multi-op-return" ||
           reason == "bare-datacarrier";
}
} // namespace

void InitTolerantOptions(const ArgsManager& args)
{
    g_tolerant = args.GetBoolArg("-tolerant", DEFAULT_TOLERANT);
    g_tolerant_log_policy = args.GetBoolArg("-tolerantlogpolicy", DEFAULT_TOLERANT_LOG_POLICY);
    g_tolerant_mining_filter = args.GetBoolArg("-tolerantminingfilter", DEFAULT_TOLERANT_MINING_FILTER);
    g_tolerant_tie_preference = args.GetBoolArg("-toleranttiepreference", DEFAULT_TOLERANT_TIE_PREFERENCE);

    if (args.IsArgSet("-datacarriersize")) {
        g_tolerant_datacarrier_limit = args.GetIntArg("-datacarriersize", MAX_OP_RETURN_RELAY);
    } else if (args.IsArgSet("-tolerantdatacarriersize")) {
        g_tolerant_datacarrier_limit = args.GetIntArg("-tolerantdatacarriersize", DEFAULT_TOLERANT_DATACARRIER_SIZE);
    } else if (g_tolerant) {
        g_tolerant_datacarrier_limit = DEFAULT_TOLERANT_DATACARRIER_SIZE;
    } else {
        g_tolerant_datacarrier_limit = MAX_OP_RETURN_RELAY;
    }
    g_tolerant_datacarrier_limit = std::min(g_tolerant_datacarrier_limit, size_t{MAX_OUTPUT_DATA_SIZE});
}

bool IsTolerantProfileEnabled() { return g_tolerant; }
bool IsTolerantLogPolicyEnabled() { return g_tolerant && g_tolerant_log_policy; }
bool IsTolerantMiningFilterEnabled() { return g_tolerant && g_tolerant_mining_filter; }
bool IsTolerantTiePreferenceEnabled() { return g_tolerant && g_tolerant_tie_preference; }
size_t GetTolerantDatacarrierPolicyLimit() { return g_tolerant_datacarrier_limit; }

CleanBlockScore CalculateCleanBlockScore(const CBlock& block, size_t policy_limit_bytes)
{
    CleanBlockScore score;
    for (const auto& tx : block.vtx) {
        if (!tx) continue;
        size_t tx_data_bytes{0};
        for (const CTxOut& txout : tx->vout) {
            if (txout.scriptPubKey.size() >= 1 && txout.scriptPubKey[0] == OP_RETURN) {
                ++score.op_return_outputs;
                score.op_return_bytes += txout.scriptPubKey.size();
            }
            const auto dcb = txout.scriptPubKey.DatacarrierBytes(0);
            tx_data_bytes += dcb.first + dcb.second;
        }
        score.total_arbitrary_data_bytes += tx_data_bytes;
        if (tx_data_bytes > policy_limit_bytes) {
            ++score.txs_exceeding_policy;
        }
    }
    score.exceeds_local_policy = score.total_arbitrary_data_bytes > policy_limit_bytes ||
                                 score.txs_exceeding_policy > 0;
    return score;
}

bool TxExceedsTolerantDatacarrierPolicy(const CTransaction& tx, const CCoinsViewCache& view, size_t policy_limit_bytes)
{
    const auto dcb = DatacarrierBytes(tx, view);
    return dcb.first + dcb.second > policy_limit_bytes;
}

void TolerantLogMempoolRejection(const uint256& txid, const std::string& reason, size_t data_bytes, size_t limit)
{
    if (!IsTolerantLogPolicyEnabled() || !IsDatacarrierRejectReason(reason)) return;
    LogPrintLevel(BCLog::TOLERANT, BCLog::Level::Info,
        "[Tolerant] Transaction rejected from mempool: OP_RETURN/arbitrary data exceeds policy limit "
        "(txid=%s, data_bytes=%u, limit=%u, reason=%s).\n",
        txid.ToString(), data_bytes, limit, reason);
}

void TolerantLogMempoolRejectionIfDatacarrier(const CTransaction& tx, const CCoinsViewCache& view, const std::string& reason)
{
    if (!IsTolerantLogPolicyEnabled() || !IsDatacarrierRejectReason(reason)) return;
    const auto dcb = DatacarrierBytes(tx, view);
    TolerantLogMempoolRejection(tx.GetHash(), reason, dcb.first + dcb.second, GetTolerantDatacarrierPolicyLimit());
}

void TolerantLogTemplateExclusion(const uint256& txid, size_t data_bytes, size_t limit)
{
    if (!IsTolerantLogPolicyEnabled()) return;
    LogPrintLevel(BCLog::TOLERANT, BCLog::Level::Info,
        "[Tolerant] Transaction excluded from block template: excessive arbitrary data "
        "(txid=%s, data_bytes=%u, limit=%u).\n",
        txid.ToString(), data_bytes, limit);
}

void TolerantLogBlockAcceptedAbovePolicy(const uint256& block_hash, int height, const CleanBlockScore& score, size_t limit)
{
    if (!IsTolerantLogPolicyEnabled() || !score.exceeds_local_policy) return;
    LogPrintLevel(BCLog::TOLERANT, BCLog::Level::Info,
        "[Tolerant] Received valid block with data above local policy. Accepting under consensus rules "
        "(block=%s, height=%d, arbitrary_data_bytes=%u, op_return_outputs=%u, txs_over_policy=%u, limit=%u).\n",
        block_hash.ToString(), height, score.total_arbitrary_data_bytes, score.op_return_outputs,
        score.txs_exceeding_policy, limit);
}

void TolerantLogCleanBlockScore(const uint256& block_hash, int height, const CleanBlockScore& score)
{
    if (!IsTolerantLogPolicyEnabled()) return;
    LogPrintLevel(BCLog::TOLERANT, BCLog::Level::Debug,
        "[Tolerant] Clean block score (block=%s, height=%d, score_bytes=%u, op_return_outputs=%u, op_return_bytes=%u, txs_over_policy=%u).\n",
        block_hash.ToString(), height, score.total_arbitrary_data_bytes, score.op_return_outputs,
        score.op_return_bytes, score.txs_exceeding_policy);
}

void TolerantLogEqualWorkTie(const CBlockIndex& active_tip, const CleanBlockScore& active_score,
                             const CBlockIndex& competing_tip, const CleanBlockScore& competing_score,
                             const uint256& preferred_hash)
{
    if (!IsTolerantLogPolicyEnabled()) return;
    LogPrintLevel(BCLog::TOLERANT, BCLog::Level::Info,
        "[Tolerant] Equal-work fork observed. Clean block preference would select block %s "
        "(active=%s score=%u, competing=%s score=%u). Chain selection unchanged in V1.\n",
        preferred_hash.ToString(),
        active_tip.GetBlockHash().ToString(), active_score.total_arbitrary_data_bytes,
        competing_tip.GetBlockHash().ToString(), competing_score.total_arbitrary_data_bytes);
}

void TolerantLogMostWorkChainSelected(const uint256& tip_hash, int height)
{
    if (!IsTolerantLogPolicyEnabled()) return;
    LogPrintLevel(BCLog::TOLERANT, BCLog::Level::Debug,
        "[Tolerant] Most-work chain selected. Following Nakamoto consensus (tip=%s, height=%d).\n",
        tip_hash.ToString(), height);
}
