// Copyright (c) 2021 Bitcoin Association
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "safe_mode.h"
#include "validation.h"
#include "config.h"
#include "rpc/http_request.h"
#include "rpc/http_response.h"
#include "rpc/webhook_client.h"

SafeMode safeMode;

bool SafeMode::IsBlockPartOfExistingSafeModeFork(const CBlockIndex* pindexNew) const
{
    //AssertLockHeld(cs_safeModeLevelForks);

    // if we received only header then block is not yet part of the fork 
    // so check only for blocks with data
    for (auto const& fork : safeModeForks)
    {
        auto pindexWalk = fork.first;
        while (pindexWalk && pindexWalk != fork.second)
        {
            if (pindexWalk == pindexNew)
                return true;
            pindexWalk = pindexWalk->GetPrev();
        }
    }
    return false;
}

SafeModeLevel SafeMode::ShouldForkTriggerSafeMode(const Config& config, const CBlockIndex* pindexForkTip, const CBlockIndex* pindexForkBase) const
{
    AssertLockHeld(cs_main);
    //AssertLockHeld(cs_safeModeLevelForks);

    if (!pindexForkTip || !pindexForkBase)
    {
        return SafeModeLevel::NONE;
    }

    if (chainActive.Contains(pindexForkTip))
    {
        return SafeModeLevel::NONE;
    }


    // check if the fork is long enough
    assert(pindexForkTip->GetHeight() >= pindexForkBase->GetHeight());
    int64_t forkLength = pindexForkTip->GetHeight() - pindexForkBase->GetHeight() + 1;

    if (forkLength < config.GetSafeModeMinForkLength())
    {
        return SafeModeLevel::NONE; // not long enough
    }

    // check if the fork is close enough
    assert(chainActive.Tip()->GetHeight() >= (pindexForkBase->GetHeight() - 1));
    int64_t forkBaseDistance = chainActive.Tip()->GetHeight() - (pindexForkBase->GetHeight() - 1);

    if (forkBaseDistance > config.GetSafeModeMaxForkDistance())
    {
        return SafeModeLevel::NONE; // not close enough
    }
    
    // check if the fork has enough proof-of-work
    auto absPowDifference = GetBlockProof(*chainActive.Tip()) * abs(config.GetSafeModeMinForkHeightDifference());
    auto forkMinPow = config.GetSafeModeMinForkHeightDifference() > 0
        ? chainActive.Tip()->GetChainWork() + absPowDifference
        : chainActive.Tip()->GetChainWork() - std::min(chainActive.Tip()->GetChainWork()*1, absPowDifference);

    if (pindexForkTip->GetChainWork() < forkMinPow)
    {
        return SafeModeLevel::NONE; // not enough POW (height)
    }

    BlockStatus forkTipStatus = pindexForkTip->getStatus();

    if (forkTipStatus.isInvalid())
    {
        return SafeModeLevel::INVALID;
    }

    if (forkTipStatus.isValid() && pindexForkTip->GetChainTx())
    {
        return SafeModeLevel::VALID;
    }

    return SafeModeLevel::UNKNOWN;
}

int64_t SafeMode::GetMinimumRelevantBlockHeight(const Config& config) const
{
    AssertLockHeld(cs_main);
    auto tipHeight = chainActive.Tip() ? chainActive.Tip()->GetHeight() : 0;
    if(tipHeight < config.GetSafeModeMaxForkDistance())
    {
        return 0;
    }
    return tipHeight - config.GetSafeModeMaxForkDistance();
}

void SafeMode::CreateForkData(const Config& config, const CBlockIndex* pindexNew)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(cs_safeModeLevelForks);

    // check if already known
    if (chainActive.Contains(pindexNew) || IsBlockPartOfExistingSafeModeFork(pindexNew))
    {
        return; // nothing to do...
    }

    // check if extends active chain
    if (chainActive.Tip() == pindexNew->GetPrev())
    {
        return; // nothing to do...
    }

    // check if extends existing fork
    auto existingFork = safeModeForks.find(pindexNew->GetPrev());
    if (existingFork != safeModeForks.end())
    {
        // replace fork data 
        const CBlockIndex* forkBase = existingFork->second;
        safeModeForks.erase(existingFork);
        safeModeForks.insert(std::pair<const CBlockIndex*, const CBlockIndex*>(pindexNew, forkBase));
        return;
    }

    // it is a new fork
    // we are walking back until current block parent is on the active chain and insert a new fork data
    const CBlockIndex* pindexWalk = pindexNew;
    auto minimumRelevantBlockHeight = GetMinimumRelevantBlockHeight(config);
    while (pindexWalk && pindexWalk->GetHeight() >= minimumRelevantBlockHeight)
    {
        if (!pindexWalk->GetPrev())
        {
            break;
        }

        if (chainActive.Contains(pindexWalk->GetPrev()))
        {
            safeModeForks.insert(std::pair<const CBlockIndex*, const CBlockIndex*>(pindexNew, pindexWalk));
            break;
        }
        pindexWalk = pindexWalk->GetPrev();
    }
}

void SafeMode::UpdateCurentForkData()
{
    AssertLockHeld(cs_main);
    AssertLockHeld(cs_safeModeLevelForks);

    for (auto it = safeModeForks.begin(); it != safeModeForks.end();)
    {
        // check if fork tip is part of the main chain
        if (chainActive.Contains(it->first))
        {
            // whole fork is part of the main chain, deleting
            it = safeModeForks.erase(it);            

        }
        else
        {
            // check if the fork base is part of the main chain
            if(chainActive.Contains(it->second))
            {
                // now try to find first block which is not on the main chain
                // by reversely iterating through the fork
                const CBlockIndex* pindexWalk = it->first;

                while ( !chainActive.Contains(pindexWalk->GetPrev()) )
                {
                    pindexWalk = pindexWalk->GetPrev();
                }

                it->second = pindexWalk;
            }
            it++;
        }
    }
}

void SafeMode::PruneStaleForkData(const Config& config)
{
    AssertLockHeld(cs_safeModeLevelForks);

    auto minimumRelevantBlockHeight = GetMinimumRelevantBlockHeight(config);

    for (auto it = safeModeForks.begin(); it != safeModeForks.end();)
    {
        if (it->second->GetPrev()->GetHeight() < minimumRelevantBlockHeight)
        {
            it = safeModeForks.erase(it);
        }
        else
        {
            it++;
        }
    }
}

const CBlockIndex* SafeMode::ExcludeIgnoredBlocks(const CBlockIndex* pindexForkTip, const CBlockIndex* pindexForkBase) const
{
    //AssertLockHeld(cs_safeModeLevelForks);

    const CBlockIndex* pindexWalk = pindexForkTip;
    const CBlockIndex* lastIgnored = nullptr;
    while (pindexWalk != pindexForkBase->GetPrev())
    {
        if (pindexWalk->GetIgnoredForSafeMode())
        {
            lastIgnored = pindexWalk;
        }
        pindexWalk = pindexWalk->GetPrev();
    }
    
    if(lastIgnored == nullptr)
    {
        return pindexForkTip;
    }

    if(lastIgnored == pindexForkBase)
    {
        return nullptr;
    }

    return lastIgnored->GetPrev();
}

SafeMode::SafeModeResult SafeMode::GetSafeModeResult(const Config& config)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(cs_safeModeLevelForks);


    SafeModeResult res{ chainActive.Tip(),{}, SafeModeLevel::NONE };

    for (auto [forkTip, forkBase] : safeModeForks)
    {
        const CBlockIndex* newForkTip = ExcludeIgnoredBlocks(forkTip, forkBase);
        if (newForkTip == nullptr)
        {
            continue;
        }

        auto level = ShouldForkTriggerSafeMode(config, newForkTip, forkBase);
        if (level != SafeModeLevel::NONE)
        {
            res.AddFork(forkTip, forkBase, level);
        }
    }
    return res;
}

void SafeMode::NotifyUsingWebhooks(const Config& config, const SafeMode::SafeModeResult& result)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(cs_safeModeLevelForks);

    if(!webhooks)
    {
        webhooks = std::make_unique<rpc::client::WebhookClient>(config);
        webhookConfig = std::make_unique<rpc::client::RPCClientConfig>(rpc::client::RPCClientConfig::CreateForSafeModeWebhook(config));
    }

    auto request = 
        std::make_shared<rpc::client::HTTPRequest>(
            rpc::client::HTTPRequest::CreateJSONPostRequest(*webhookConfig, result.ToJson(false) + "\r\n")
        );
    auto response = std::make_shared<rpc::client::StringHTTPResponse>();
    webhooks->SubmitRequest(*webhookConfig, std::move(request), std::move(response));
}


void SafeMode::CheckSafeModeParameters(const Config& config, const CBlockIndex* pindexNew)
{
    AssertLockHeld(cs_main);

    if (pindexNew && pindexNew->IsGenesis())
        return;

    LOCK(cs_safeModeLevelForks);

    // Old tip is not on the active chain any more. This means that the reorg happen in meanwhile.
    bool reorgHappened = oldTip && !chainActive.Contains(oldTip);

    if(reorgHappened || (pindexNew == nullptr)) 
    {
        // A reorg happened or we have unspecified change;
        // lets recalculate fork data for all forks.
        safeModeForks.clear();
        for (const CBlockIndex* tip: GetForkTips())
        {
            CreateForkData(config, tip);
        }
    }
    else
    {
        CreateForkData(config, pindexNew);
    }

    UpdateCurentForkData();
    PruneStaleForkData(config);
    auto newResults = GetSafeModeResult(config);

    if(!(newResults == currentResult) && config.GetSafeModeWebhookAddress() != "")
    {
        NotifyUsingWebhooks(config, newResults);
        LogPrintf("WARNING: Safe mode: " + newResults.ToJson(false) + "\n");
    }

    currentResult = newResults;
    oldTip = chainActive.Tip();

    // If we have any forks trigger safe mode
    if (GetSafeModeLevel() != newResults.maxLevel)
    {
        SetSafeModeLevel(newResults.maxLevel);
        static std::map<SafeModeLevel, std::string> levelLookup = {
            {SafeModeLevel::NONE, "NONE"}, 
            {SafeModeLevel::VALID, "VALID"}, 
            {SafeModeLevel::INVALID, "INVALID"}, 
            {SafeModeLevel::UNKNOWN, "UNKNOWN"} 
        };
        LogPrintf("WARNING: Safe mode level changed to " + levelLookup[newResults.maxLevel] + "\n");
        if( newResults.maxLevel == SafeModeLevel::VALID)
        {
            std::string warning = "'Warning: Large-work fork detected, forking after block:";
            for (const auto& f: newResults.forks)
            {
                warning += " " + f.second.base->GetPrev()->GetBlockHash().ToString();
            }
            AlertNotify(warning);
        }
    }
}

void SafeMode::Clear() 
{
    LOCK(cs_safeModeLevelForks);
    oldTip = nullptr;
    safeModeForks.clear();
}

void SafeMode::GetStatus(CJSONWriter& writer)
{
    AssertLockHeld(cs_main);
    LOCK(cs_safeModeLevelForks);
    return currentResult.ToJson(writer);
}

std::string SafeMode::GetStatus()
{
    AssertLockHeld(cs_main);
    LOCK(cs_safeModeLevelForks);
    return currentResult.ToJson();
}

void SafeMode::SafeModeResult::AddFork(const CBlockIndex* forkTip, const CBlockIndex* forkBase, SafeModeLevel level)
{
    maxLevel = std::max(level, maxLevel);

    auto forkIt = forks.find(forkBase);

    if(forkIt != forks.end())
    {
        forkIt->second.tips.insert(forkTip);
    }
    else
    {
        forks[forkBase] = SafeModeFork{ { forkTip }, forkBase };
    }
}

void SafeMode::SafeModeResult::ToJson(CJSONWriter& writer) const
{
    auto GetStatusString = [](const CBlockIndex* block) -> std::string
    {
        std::string status;
        if (chainActive.Contains(block)) {
            // This block is part of the currently active chain.
            status = "active";
        } else if (block->getStatus().isInvalid()) {
            // This block or one of its ancestors is invalid.
            status = "invalid";
        } else if (block->GetChainTx() == 0) {
            // This block cannot be connected because full block data for it or
            // one of its parents is missing.
            status = "headers-only";
        } else if (block->IsValid(BlockValidity::SCRIPTS)) {
            // This block is fully validated, but no longer part of the active
            // chain. It was probably the active block once, but was
            // reorganized.
            status = "valid-fork";
        } else if (block->IsValid(BlockValidity::TREE)) {
            // The headers for this block are valid, but it has not been
            // validated. It was probably never part of the most-work chain.
            status = "valid-headers";
        } else {
            // No clue.
            status = "unknown";
        }
        return status;
    };

    auto WriteBlock = [&](CJSONWriter& writer, std::string jsonObjectName, const CBlockIndex* block )
    {
        writer.writeBeginObject(jsonObjectName);
        if(block)
        {
            writer.pushKV("hash", block->GetBlockHash().ToString());
            writer.pushKV("height", block->GetHeight());
            writer.pushKV("blocktime", DateTimeStrFormat("%Y-%m-%dT%H:%M:%SZ", block->GetBlockTime()));
            writer.pushKV("firstseentime", DateTimeStrFormat("%Y-%m-%dT%H:%M:%SZ", block->GetHeaderReceivedTime()));
            writer.pushKV("status", GetStatusString(block));
        }
        writer.writeEndObject();
    };


    
    writer.writeBeginObject();
        writer.pushKV("safemodeenabled", maxLevel != SafeModeLevel::NONE);
        WriteBlock(writer, "activetip", chainActive.Tip());
        writer.pushKV("timeutc", DateTimeStrFormat("%Y-%m-%dT%H:%M:%SZ", std::time(nullptr)));

        std::vector<std::reference_wrapper<const SafeModeFork>> sortedForks;
        for(const auto& f: forks)
        {
            sortedForks.emplace_back(f.second);
        }
        std::sort(sortedForks.begin(), sortedForks.end(), 
            [](std::reference_wrapper<const SafeModeFork> lhs, std::reference_wrapper<const SafeModeFork> rhs) 
            { 
                return SafeModeFork::CompareBlockIndex(lhs.get().base, rhs.get().base);
            }
        );

        writer.writeBeginArray("forks");            
            for(const auto& fork: sortedForks)
            {
                writer.writeBeginObject();

                    WriteBlock(writer, "forkfirstblock", fork.get().base);

                    std::vector<const CBlockIndex*> sortedTips{fork.get().tips.begin(), fork.get().tips.end()};
                    std::sort(sortedTips.begin(), sortedTips.end(), SafeModeFork::CompareBlockIndex);
                    writer.writeBeginArray("tips");
                        for(const CBlockIndex* tip: sortedTips)
                        {
                            WriteBlock(writer, "", tip);
                        }
                    writer.writeEndArray();

                    WriteBlock(writer, "lastcommonblock", fork.get().base->GetPrev());
                    WriteBlock(writer, "activechainfirstblock", chainActive.Next(fork.get().base->GetPrev()));

                writer.writeEndObject();
            }
        writer.writeEndArray();
    writer.writeEndObject();
}

std::string SafeMode::SafeModeResult::ToJson(bool pretty) const
{
    CStringWriter strWriter;
    CJSONWriter writer(strWriter, pretty);
    ToJson(writer);
    strWriter.Flush();
    return strWriter.MoveOutString();
}



void SafeModeClear()
{
    AssertLockHeld(cs_main);
    safeMode.Clear();
}

void CheckSafeModeParameters(const Config& conf, const CBlockIndex* pindexNew)
{
    AssertLockHeld(cs_main);
    safeMode.CheckSafeModeParameters(conf, pindexNew);
}

void SafeModeGetStatus(CJSONWriter& writer)
{
    AssertLockHeld(cs_main);
    return safeMode.GetStatus(writer);
}

std::string SafeModeGetStatus()
{
    AssertLockHeld(cs_main);
    return safeMode.GetStatus();
}