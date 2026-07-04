// Copyright (c) 2026 The Tolerant Knots developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_TOLERANT_H
#define BITCOIN_NODE_TOLERANT_H

#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <string>

class ArgsManager;
class CBlockIndex;
class CCoinsViewCache;

struct CleanBlockScore {
    size_t op_return_outputs{0};
    size_t op_return_bytes{0};
    size_t txs_exceeding_policy{0};
    size_t total_arbitrary_data_bytes{0};
    bool exceeds_local_policy{false};
};

/** Defaults for Tolerant policy options. */
static constexpr bool DEFAULT_TOLERANT{true};
static constexpr unsigned int DEFAULT_TOLERANT_DATACARRIER_SIZE{83};
static constexpr bool DEFAULT_TOLERANT_LOG_POLICY{true};
static constexpr bool DEFAULT_TOLERANT_MINING_FILTER{true};
static constexpr bool DEFAULT_TOLERANT_TIE_PREFERENCE{false};

void InitTolerantOptions(const ArgsManager& args);

bool IsTolerantProfileEnabled();
bool IsTolerantLogPolicyEnabled();
bool IsTolerantMiningFilterEnabled();
bool IsTolerantTiePreferenceEnabled();
size_t GetTolerantDatacarrierPolicyLimit();

CleanBlockScore CalculateCleanBlockScore(const CBlock& block, size_t policy_limit_bytes);
bool TxExceedsTolerantDatacarrierPolicy(const CTransaction& tx, const CCoinsViewCache& view, size_t policy_limit_bytes);

void TolerantLogMempoolRejection(const uint256& txid, const std::string& reason, size_t data_bytes, size_t limit);
void TolerantLogMempoolRejectionIfDatacarrier(const CTransaction& tx, const CCoinsViewCache& view, const std::string& reason);
void TolerantLogTemplateExclusion(const uint256& txid, size_t data_bytes, size_t limit);
void TolerantLogBlockAcceptedAbovePolicy(const uint256& block_hash, int height, const CleanBlockScore& score, size_t limit);
void TolerantLogCleanBlockScore(const uint256& block_hash, int height, const CleanBlockScore& score);
void TolerantLogEqualWorkTie(const CBlockIndex& active_tip, const CleanBlockScore& active_score,
                             const CBlockIndex& competing_tip, const CleanBlockScore& competing_score,
                             const uint256& preferred_hash);
void TolerantLogMostWorkChainSelected(const uint256& tip_hash, int height);

#endif // BITCOIN_NODE_TOLERANT_H
