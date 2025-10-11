// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/meta.h>

#include <flat-database.h>
#include <univalue.h>
#include <util/time.h>

const std::string MasternodeMetaStore::SERIALIZATION_VERSION_STRING = "CMasternodeMetaMan-Version-4";

CMasternodeMetaMan::CMasternodeMetaMan() :
    m_db{std::make_unique<db_type>("mncache.dat", "magicMasternodeCache")}
{
}

bool CMasternodeMetaMan::LoadCache(bool load_cache)
{
    assert(m_db != nullptr);
    is_valid = load_cache ? m_db->Load(*this) : m_db->Store(*this);
    return is_valid;
}

CMasternodeMetaMan::~CMasternodeMetaMan()
{
    if (!is_valid) return;
    m_db->Store(*this);
}

UniValue CMasternodeMetaInfo::ToJson() const
{
    UniValue ret(UniValue::VOBJ);

    int64_t now = GetTime<std::chrono::seconds>().count();

    ret.pushKV("lastDSQ", nLastDsq.load());
    ret.pushKV("mixingTxCount", nMixingTxCount.load());
    ret.pushKV("outboundAttemptCount", outboundAttemptCount.load());
    ret.pushKV("lastOutboundAttempt", lastOutboundAttempt.load());
    ret.pushKV("lastOutboundAttemptElapsed", now - lastOutboundAttempt.load());
    ret.pushKV("lastOutboundSuccess", lastOutboundSuccess.load());
    ret.pushKV("lastOutboundSuccessElapsed", now - lastOutboundSuccess.load());
    {
        LOCK(cs);
        ret.pushKV("is_platform_banned", m_platform_ban);
        ret.pushKV("platform_ban_height_updated", m_platform_ban_updated);
    }

    return ret;
}

void CMasternodeMetaInfo::AddGovernanceVote(const uint256& nGovernanceObjectHash)
{
    LOCK(cs);
    // Insert a zero value, or not. Then increment the value regardless. This
    // ensures the value is in the map.
    const auto& pair = mapGovernanceObjectsVotedOn.emplace(nGovernanceObjectHash, 0);
    pair.first->second++;
}

void CMasternodeMetaInfo::RemoveGovernanceObject(const uint256& nGovernanceObjectHash)
{
    LOCK(cs);
    // Whether or not the govobj hash exists in the map first is irrelevant.
    mapGovernanceObjectsVotedOn.erase(nGovernanceObjectHash);
}

CMasternodeMetaInfo CMasternodeMetaMan::GetInfo(const uint256& proTxHash)
{
    const auto info = GetMetaInfo(proTxHash, false);
    if (info == nullptr) return CMasternodeMetaInfo{};

    return *info;
}

CMasternodeMetaInfoPtr CMasternodeMetaMan::GetMetaInfo(const uint256& proTxHash, bool fCreate)
{
    LOCK(cs);
    auto it = metaInfos.find(proTxHash);
    if (it != metaInfos.end()) {
        return it->second;
    }
    if (!fCreate) {
        return nullptr;
    }
    it = metaInfos.emplace(proTxHash, std::make_shared<CMasternodeMetaInfo>(proTxHash)).first;
    return it->second;
}

bool CMasternodeMetaMan::IsDsqOver(const uint256& protx_hash, int mn_count) const
{
    LOCK(cs);
    auto it = metaInfos.find(protx_hash);
    if (it != metaInfos.end()) {
        LogPrint(BCLog::COINJOIN, "DSQUEUE -- node %s is logged\n", protx_hash.ToString());
        return false;
    }
    const auto& meta_info = *it->second;
    int64_t last_dsq = meta_info.GetLastDsq();
    int64_t threshold = last_dsq + mn_count / 5;

    LogPrint(BCLog::COINJOIN, "DSQUEUE -- mn: %s last_dsq: %d  dsq_threshold: %d  nDsqCount: %d\n",
             protx_hash.ToString(), last_dsq, threshold, nDsqCount);
    return last_dsq != 0 && threshold > nDsqCount;
}

void CMasternodeMetaMan::AllowMixing(const uint256& proTxHash)
{
    auto mm = GetMetaInfo(proTxHash);
    nDsqCount++;
    mm->nLastDsq = nDsqCount.load();
    mm->nMixingTxCount = 0;
}

void CMasternodeMetaMan::DisallowMixing(const uint256& proTxHash)
{
    auto mm = GetMetaInfo(proTxHash);
    mm->nMixingTxCount++;
}

bool CMasternodeMetaMan::AddGovernanceVote(const uint256& proTxHash, const uint256& nGovernanceObjectHash)
{
    auto mm = GetMetaInfo(proTxHash);
    mm->AddGovernanceVote(nGovernanceObjectHash);
    return true;
}

void CMasternodeMetaMan::RemoveGovernanceObject(const uint256& nGovernanceObjectHash)
{
    LOCK(cs);
    for(const auto& p : metaInfos) {
        p.second->RemoveGovernanceObject(nGovernanceObjectHash);
    }
}

std::vector<uint256> CMasternodeMetaMan::GetAndClearDirtyGovernanceObjectHashes()
{
    std::vector<uint256> vecTmp;
    WITH_LOCK(cs, vecTmp.swap(vecDirtyGovernanceObjectHashes));
    return vecTmp;
}

bool CMasternodeMetaMan::OutboundFailedTooManyTimes(const uint256& protx_hash) const
{
    LOCK(cs);

    auto it = metaInfos.find(protx_hash);
    if (it == metaInfos.end()) return false;

    return it->second->OutboundFailedTooManyTimes();
}

bool CMasternodeMetaMan::IsPlatformBanned(const uint256& protx_hash) const
{
    LOCK(cs);

    auto it = metaInfos.find(protx_hash);
    if (it == metaInfos.end()) return false;

    return it->second->IsPlatformBanned();
}

bool CMasternodeMetaMan::ResetPlatformBan(const uint256& protx_hash, int height)
{
    LOCK(cs);

    auto it = metaInfos.find(protx_hash);
    if (it == metaInfos.end()) return false;

    return it->second->SetPlatformBan(false, height);
}

bool CMasternodeMetaMan::AlreadyHavePlatformBan(const uint256& inv_hash) const
{
    LOCK(cs);
    return m_seen_platform_bans.exists(inv_hash);
}

std::optional<PlatformBanMessage> CMasternodeMetaMan::GetPlatformBan(const uint256& inv_hash) const
{
    LOCK(cs);
    PlatformBanMessage ret;
    if (!m_seen_platform_bans.get(inv_hash, ret)) {
        return std::nullopt;
    }

    return ret;
}

void CMasternodeMetaMan::RememberPlatformBan(const uint256& inv_hash, PlatformBanMessage&& msg)
{
    LOCK(cs);
    m_seen_platform_bans.insert(inv_hash, std::move(msg));
}

std::string MasternodeMetaStore::ToString() const
{
    LOCK(cs);
    return strprintf("Masternodes: meta infos object count: %d, nDsqCount: %d", metaInfos.size(), nDsqCount);
}

uint256 PlatformBanMessage::GetHash() const { return ::SerializeHash(*this); }
