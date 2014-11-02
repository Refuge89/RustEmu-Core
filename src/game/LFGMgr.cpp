/*
* Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "Common.h"
#include "Policies/Singleton.h"
#include "SharedDefines.h"
#include "ObjectMgr.h"
#include "ProgressBar.h"
#include "SocialMgr.h"
#include "LFGMgr.h"
#include "World.h"
#include "Group.h"
#include "Player.h"

#include <limits>

INSTANTIATE_SINGLETON_1(LFGMgr);

LFGMgr::LFGMgr()
{
    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        if (LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(i))
        {
            m_dungeonMap.insert(std::make_pair(dungeon->ID, dungeon));
        }
    }

    for (uint32 i = 0; i < sLFGDungeonExpansionStore.GetNumRows(); ++i)
    {
        if (LFGDungeonExpansionEntry const* dungeonEx = sLFGDungeonExpansionStore.LookupEntry(i))
        {
            m_dungeonExpansionMap.insert(std::make_pair(dungeonEx->dungeonID, dungeonEx));
        }
    }
    m_proposalID = 1;
}

LFGMgr::~LFGMgr()
{
    m_RewardMap.clear();
    for (uint8 i = LFG_TYPE_NONE; i < LFG_TYPE_MAX; ++i)
    {
        m_playerQueue[i].clear();
        m_groupQueue[i].clear();
    }
    m_queueInfoMap.clear();
    m_dungeonMap.clear();
    m_dungeonExpansionMap.clear();
    m_proposalMap.clear();
    m_searchMatrix.clear();
}

void LFGMgr::Update(uint32 diff)
{

    if (m_queueInfoMap.empty())
        return;

    bool isFullUpdate = false;

    if (m_updateTimer < diff)
    {
        isFullUpdate = true;
        m_updateTimer = LFG_UPDATE_INTERVAL;
    }
    else
        m_updateTimer -= diff;

    if (isFullUpdate)
    {
        CleanupSearchMatrix();
        CleanupProposals();
    }

    for (uint8 i = LFG_TYPE_NONE; i < LFG_TYPE_MAX; ++i)
    {

        if (m_playerQueue[i].empty() && m_groupQueue[i].empty())
            continue;

        LFGType type = LFGType(i);
        //        DEBUG_LOG("LFGMgr::Update type %u, player queue %u group queue %u",type,m_playerQueue[i].size(), m_groupQueue[i].size());
        switch (type)
        {
        case LFG_TYPE_DUNGEON:
        case LFG_TYPE_QUEST:
        case LFG_TYPE_ZONE:
        case LFG_TYPE_HEROIC_DUNGEON:
        case LFG_TYPE_RANDOM_DUNGEON:
        {
            TruCompleteGroups(type);
            TruCreateGroup(type);
            if (isFullUpdate)
                UpdateStatistic(type);
            break;
        }
        case LFG_TYPE_RAID:
        {
            if (sWorld.getConfig(CONFIG_BOOL_LFR_EXTEND))
            {
                UpdateLFRGroups();
                if (isFullUpdate)
                    UpdateStatistic(type);
            }
            break;
        }
        case LFG_TYPE_NONE:
        case LFG_TYPE_MAX:
        default:
            sLog.outError("LFGMgr: impossible dungeon type in queue!");
            break;
        }
    }

}

void LFGMgr::LoadRewards()
{
    // (c) TrinityCore, 2010. Rewrited for MaNGOS by /dev/rsa

    m_RewardMap.clear();

    uint32 count = 0;
    // ORDER BY is very important for GetRandomDungeonReward!
    QueryResult* result = WorldDatabase.Query("SELECT dungeonId, maxLevel, firstQuestId, firstMoneyVar, firstXPVar, otherQuestId, otherMoneyVar, otherXPVar FROM lfg_dungeon_rewards ORDER BY dungeonId, maxLevel ASC");

    if (!result)
    {
        BarGoLink bar(1);
        bar.step();

        sLog.outString();
        sLog.outErrorDb(">> Loaded 0 LFG dungeon rewards. DB table `lfg_dungeon_rewards` is empty!");
        return;
    }

    BarGoLink bar(result->GetRowCount());

    Field* fields = NULL;
    do
    {
        bar.step();
        fields = result->Fetch();
        uint32 dungeonId = fields[0].GetUInt32();
        uint32 maxLevel = fields[1].GetUInt8();
        uint32 firstQuestId = fields[2].GetUInt32();
        uint32 firstMoneyVar = fields[3].GetUInt32();
        uint32 firstXPVar = fields[4].GetUInt32();
        uint32 otherQuestId = fields[5].GetUInt32();
        uint32 otherMoneyVar = fields[6].GetUInt32();
        uint32 otherXPVar = fields[7].GetUInt32();

        if (!sLFGDungeonStore.LookupEntry(dungeonId))
        {
            sLog.outErrorDb("LFGMgr: Dungeon %u specified in table `lfg_dungeon_rewards` does not exist!", dungeonId);
            continue;
        }

        if (!maxLevel || maxLevel > sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL))
        {
            sLog.outErrorDb("LFGMgr: Level %u specified for dungeon %u in table `lfg_dungeon_rewards` can never be reached!", maxLevel, dungeonId);
            maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
        }

        if (firstQuestId && !sObjectMgr.GetQuestTemplate(firstQuestId))
        {
            sLog.outErrorDb("LFGMgr: First quest %u specified for dungeon %u in table `lfg_dungeon_rewards` does not exist!", firstQuestId, dungeonId);
            firstQuestId = 0;
        }

        if (otherQuestId && !sObjectMgr.GetQuestTemplate(otherQuestId))
        {
            sLog.outErrorDb("LFGMgr: Other quest %u specified for dungeon %u in table `lfg_dungeon_rewards` does not exist!", otherQuestId, dungeonId);
            otherQuestId = 0;
        }
        LFGReward reward = LFGReward(maxLevel, firstQuestId, firstMoneyVar, firstXPVar, otherQuestId, otherMoneyVar, otherXPVar);
        m_RewardMap.insert(LFGRewardMap::value_type(dungeonId, reward));
        ++count;
    } while (result->NextRow());

    sLog.outString();
    sLog.outString(">> Loaded %u LFG dungeon rewards.", count);
}

LFGReward const* LFGMgr::GetRandomDungeonReward(LFGDungeonEntry const* dungeon, Player* player)
{
    LFGReward const* rew = NULL;
    if (player)
    {
        LFGRewardMapBounds bounds = m_RewardMap.equal_range(dungeon->ID);
        for (LFGRewardMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
        {
            // Difficulty check TODO
            rew = &itr->second;
            // ordered properly at loading
            if (itr->second.maxLevel >= player->getLevel())
                break;
        }
    }
    return rew;
}

bool LFGMgr::IsRandomDungeon(LFGDungeonEntry const*  dungeon)
{
    if (!dungeon)
        return false;

    return dungeon->type == LFG_TYPE_RANDOM_DUNGEON;
}

void LFGMgr::Join(Player* player)
{
    //    LfgDungeonSet* dungeons = NULL;

    if (!sWorld.getConfig(CONFIG_BOOL_LFG_ENABLE) && !sWorld.getConfig(CONFIG_BOOL_LFR_ENABLE))
        return;

    ObjectGuid guid;
    Group* group = player->GetGroup();

    if (group)
    {
        if (player->GetObjectGuid() != group->GetLeaderGuid())
        {
            DEBUG_LOG("LFGMgr::Join: %u trying to join in group, but not group leader. Aborting.", guid.GetCounter());
            player->GetSession()->SendLfgJoinResult(ERR_LFG_NO_SLOTS_PLAYER);
            return;
        }
        else
            guid = group->GetObjectGuid();
    }
    else
        guid = player->GetObjectGuid();

    if (guid.IsEmpty())
        return;

    LFGType type = player->GetLFGState()->GetType();

    if (type == LFG_TYPE_NONE)
    {
        DEBUG_LOG("LFGMgr::Join: %u trying to join without dungeon type. Aborting.", guid.GetCounter());
        player->GetSession()->SendLfgJoinResult(ERR_LFG_INVALID_SLOT);
        return;
    }

    LFGQueueInfo* queue = GetQueueInfo(guid);

    if (queue)
    {
        DEBUG_LOG("LFGMgr::Join: %u trying to join but is already in queue!", guid.GetCounter());
        //        player->GetSession()->SendLfgJoinResult(ERR_LFG_NO_LFG_OBJECT);
        RemoveFromQueue(guid);
        //        return;
    }

    LFGJoinResult result = guid.IsGroup() ? GetGroupJoinResult(group) : GetPlayerJoinResult(player);

    if (result != ERR_LFG_OK)                              // Someone can't join. Clear all stuf
    {
        DEBUG_LOG("LFGMgr::Join: %s %u joining with %u members. result: %u", guid.IsGroup() ? "Group" : "Player", guid.GetCounter(), group ? group->GetMembersCount() : 1, result);
        player->GetLFGState()->Clear();
        player->GetSession()->SendLfgJoinResult(result);
        return;
    }

    if (!guid.IsGroup() && player->GetLFGState()->GetRoles() == LFG_ROLE_MASK_NONE)
    {
        DEBUG_LOG("LFGMgr::Join:Error: %u has no roles! continued...", guid.GetCounter());
    }

    // Joining process
    if (guid.IsGroup())
    {
        for (GroupReference *itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            if (Player* player = itr->getSource())
                RemoveFromQueue(player->GetObjectGuid());
        }
        AddToQueue(guid, type, false);
        group->GetLFGState()->SetState((type == LFG_TYPE_RAID) ? LFG_STATE_LFR : LFG_STATE_LFG);
        if (type == LFG_TYPE_RAID && sWorld.getConfig(CONFIG_BOOL_LFR_EXTEND))
            group->ConvertToLFG(type);
    }
    else
    {
        RemoveFromQueue(guid);
        AddToQueue(guid, type, false);
        AddToSearchMatrix(guid);
    }

    player->GetLFGState()->SetState((type == LFG_TYPE_RAID) ? LFG_STATE_LFR : LFG_STATE_LFG);
    player->GetLFGState()->SetJoined();

    player->GetSession()->SendLfgJoinResult(ERR_LFG_OK, 0);

    if (group)
        player->GetSession()->SendLfgUpdateParty(LFG_UPDATETYPE_JOIN_PROPOSAL, type);
    else
        player->GetSession()->SendLfgUpdatePlayer(LFG_UPDATETYPE_JOIN_PROPOSAL, type);

    if (sWorld.getConfig(CONFIG_BOOL_RESTRICTED_LFG_CHANNEL))
        player->JoinLFGChannel();

}

void LFGMgr::Leave(Group* group)
{
    if (!group)
        return;

    Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());

    if (!leader)
        return;

    Leave(leader);
}

void LFGMgr::Leave(Player* player)
{

    if (!sWorld.getConfig(CONFIG_BOOL_LFG_ENABLE) && !sWorld.getConfig(CONFIG_BOOL_LFR_ENABLE))
        return;

    ObjectGuid guid;
    Group* group = player->GetGroup();

    if (group)
    {
        if (player->GetObjectGuid() != group->GetLeaderGuid())
            return;
        else
            guid = group->GetObjectGuid();
    }
    else
        guid = player->GetObjectGuid();

    if (guid.IsEmpty())
        return;

    LFGType type = player->GetLFGState()->GetType();

    RemoveFromQueue(guid);
    RemoveFromSearchMatrix(guid);

    player->GetLFGState()->Clear();
    if (group)
    {
        group->GetLFGState()->Clear();
        player->GetSession()->SendLfgUpdateParty(LFG_UPDATETYPE_REMOVED_FROM_QUEUE, type);
    }
    player->GetSession()->SendLfgUpdatePlayer(LFG_UPDATETYPE_REMOVED_FROM_QUEUE, type);

    if (sWorld.getConfig(CONFIG_BOOL_RESTRICTED_LFG_CHANNEL) && player->GetSession()->GetSecurity() == SEC_PLAYER)
        player->LeaveLFGChannel();
}

LFGQueueInfo* LFGMgr::GetQueueInfo(ObjectGuid guid)
{
    ReadGuard Guard(GetLock());

    LFGQueueInfoMap::iterator queue = m_queueInfoMap.find(guid);
    if (queue == m_queueInfoMap.end())
        return NULL;
    else
        return &queue->second;
}

void LFGMgr::AddToQueue(ObjectGuid guid, LFGType type, bool inBegin)
{
    if (guid.IsEmpty())
        return;

    // Joining process

    LFGQueueInfo qInfo = LFGQueueInfo(guid, type);

    LFGQueueInfoMap::iterator queue = m_queueInfoMap.find(guid);

    if (queue == m_queueInfoMap.end())
    {
        WriteGuard Guard(GetLock());
        m_queueInfoMap.insert(std::make_pair(guid, qInfo));
    }
    else
    {
        WriteGuard Guard(GetLock());
        m_queueInfoMap.erase(guid);
        m_queueInfoMap.insert(std::make_pair(guid, qInfo));
    }

    LFGQueueInfo* pqInfo = GetQueueInfo(guid);
    MANGOS_ASSERT(pqInfo);

    if (type != LFG_TYPE_NONE)
    {
        WriteGuard Guard(GetLock());
        if (guid.IsGroup())
            m_groupQueue[type].insert((inBegin ? m_groupQueue[type].begin() : m_groupQueue[type].end()), pqInfo);
        else
            m_playerQueue[type].insert((inBegin ? m_playerQueue[type].begin() : m_playerQueue[type].end()), pqInfo);
    }
    DEBUG_LOG("LFGMgr::AddToQueue: %s %u joined, type %u", (guid.IsGroup() ? "group" : "player"), guid.GetCounter(), type);
}

void LFGMgr::RemoveFromQueue(ObjectGuid guid)
{
    WriteGuard Guard(GetLock());
    LFGQueueInfoMap::iterator queue = m_queueInfoMap.find(guid);
    if (queue != m_queueInfoMap.end())
    {
        LFGType type = queue->second.GetDungeonType();

        DEBUG_LOG("LFGMgr::RemoveFromQueue: %s %u removed, type %u", (guid.IsGroup() ? "group" : "player"), guid.GetCounter(), type);

        if (type != LFG_TYPE_NONE)
        {
            if (guid.IsGroup())
            {
                if (m_groupQueue[type].find(&queue->second) != m_groupQueue[type].end())
                    m_groupQueue[type].erase(&queue->second);
            }
            else
            {
                if (m_playerQueue[type].find(&queue->second) != m_playerQueue[type].end())
                    m_playerQueue[type].erase(&queue->second);
            }
        }
        m_queueInfoMap.erase(guid);
    }
}

LFGJoinResult LFGMgr::GetPlayerJoinResult(Player* player)
{

    if (player->InBattleGround() || player->InArena() || player->InBattleGroundQueue())
        return ERR_LFG_CANT_USE_DUNGEONS;

    if (player->HasAura(LFG_SPELL_DUNGEON_DESERTER))
        return  ERR_LFG_DESERTER_PLAYER;

    if (player->HasAura(LFG_SPELL_DUNGEON_COOLDOWN))
        return ERR_LFG_RANDOM_COOLDOWN_PLAYER;

    LFGDungeonSet* dungeons = player->GetLFGState()->GetDungeons();
    // TODO - Check if all dungeons are valid

    // must be last check - ignored in party
    if (!dungeons || !dungeons->size())
        return ERR_LFG_INVALID_SLOT;

    return ERR_LFG_OK;
}

LFGJoinResult LFGMgr::GetGroupJoinResult(Group* group)
{
    if (!group)
        return ERR_LFG_GET_INFO_TIMEOUT;

    if (!group->isRaidGroup() && (group->GetMembersCount() > MAX_GROUP_SIZE))
        return ERR_LFG_TOO_MANY_MEMBERS;

    if (group->isRaidGroup() && group->GetLFGState()->GetDungeonType() != LFG_TYPE_RAID)
    {
        DEBUG_LOG("LFGMgr::Join: Group %u trying to join as raid, but not to raid finder. Aborting.", group->GetObjectGuid().GetCounter());
        return ERR_LFG_MISMATCHED_SLOTS;
    }

    for (GroupReference *itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();

        if (!player->IsInWorld())
            return ERR_LFG_MEMBERS_NOT_PRESENT;

        LFGJoinResult result = GetPlayerJoinResult(player);

        if (result == ERR_LFG_INVALID_SLOT)
            continue;

        if (result != ERR_LFG_OK)
            return result;
    }

    return ERR_LFG_OK;
}

LFGLockStatusMap LFGMgr::GetPlayerLockMap(Player* player)
{
    LFGLockStatusMap tmpMap;
    tmpMap.clear();

    if (!player)
        return tmpMap;

    for (uint32 i = 1; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        if (LFGDungeonEntry const* entry = sLFGDungeonStore.LookupEntry(i))
            if (LFGLockStatusType status = GetPlayerLockStatus(player, entry))
                if (status != LFG_LOCKSTATUS_OK)
                    tmpMap.insert(std::make_pair(entry, status));
    }

    return tmpMap;
}

LFGLockStatusType LFGMgr::GetPlayerLockStatus(Player* player, LFGDungeonEntry const* dungeon)
{
    if (!player || !player->IsInWorld())
        return LFG_LOCKSTATUS_RAID_LOCKED;

    if (dungeon->expansion > player->GetSession()->Expansion())
        return LFG_LOCKSTATUS_INSUFFICIENT_EXPANSION;

    if (dungeon->difficulty > DUNGEON_DIFFICULTY_NORMAL
        && player->GetBoundInstance(dungeon->map, Difficulty(dungeon->difficulty)))
        return  LFG_LOCKSTATUS_RAID_LOCKED;

    if (dungeon->minlevel > player->getLevel())
        return  LFG_LOCKSTATUS_TOO_LOW_LEVEL;

    if (dungeon->maxlevel < player->getLevel())
        return LFG_LOCKSTATUS_TOO_HIGH_LEVEL;

    switch (player->GetAreaLockStatus(dungeon->map, Difficulty(dungeon->difficulty)))
    {
    case AREA_LOCKSTATUS_OK:
        break;
    case AREA_LOCKSTATUS_TOO_LOW_LEVEL:
        return  LFG_LOCKSTATUS_TOO_LOW_LEVEL;
    case AREA_LOCKSTATUS_QUEST_NOT_COMPLETED:
        return LFG_LOCKSTATUS_QUEST_NOT_COMPLETED;
    case AREA_LOCKSTATUS_MISSING_ITEM:
        return LFG_LOCKSTATUS_MISSING_ITEM;
    case AREA_LOCKSTATUS_MISSING_DIFFICULTY:
        return LFG_LOCKSTATUS_RAID_LOCKED;
    case AREA_LOCKSTATUS_INSUFFICIENT_EXPANSION:
        return LFG_LOCKSTATUS_INSUFFICIENT_EXPANSION;
    case AREA_LOCKSTATUS_NOT_ALLOWED:
        return LFG_LOCKSTATUS_RAID_LOCKED;
    case AREA_LOCKSTATUS_RAID_LOCKED:
    case AREA_LOCKSTATUS_UNKNOWN_ERROR:
    default:
        return LFG_LOCKSTATUS_RAID_LOCKED;
    }

    if (dungeon->difficulty > DUNGEON_DIFFICULTY_NORMAL)
    {
        if (AreaTrigger const* at = sObjectMgr.GetMapEntranceTrigger(dungeon->map))
        {
            uint32 gs = player->GetEquipGearScore(true, true);

            if (at->minGS > 0 && gs < at->minGS)
                return LFG_LOCKSTATUS_TOO_LOW_GEAR_SCORE;
            else if (at->maxGS > 0 && gs > at->maxGS)
                return LFG_LOCKSTATUS_TOO_HIGH_GEAR_SCORE;
        }
        else
            return LFG_LOCKSTATUS_RAID_LOCKED;
    }

    if (InstancePlayerBind* bind = player->GetBoundInstance(dungeon->map, Difficulty(dungeon->difficulty)))
    {
        if (DungeonPersistentState* state = bind->state)
            if (state->IsCompleted())
                return LFG_LOCKSTATUS_RAID_LOCKED;
    }

    /* TODO
    LFG_LOCKSTATUS_ATTUNEMENT_TOO_LOW_LEVEL;
    LFG_LOCKSTATUS_ATTUNEMENT_TOO_HIGH_LEVEL;
    LFG_LOCKSTATUS_NOT_IN_SEASON;
    */

    return LFG_LOCKSTATUS_OK;
}

LFGLockStatusType LFGMgr::GetPlayerExpansionLockStatus(Player* player, LFGDungeonEntry const* dungeon)
{
    if (!player || !player->IsInWorld())
        return LFG_LOCKSTATUS_RAID_LOCKED;

    return LFG_LOCKSTATUS_OK;
}

LFGLockStatusType LFGMgr::GetGroupLockStatus(Group* group, LFGDungeonEntry const* dungeon)
{
    if (!group)
        return LFG_LOCKSTATUS_RAID_LOCKED;

    for (GroupReference *itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();

        LFGLockStatusType result = GetPlayerLockStatus(player, dungeon);

        if (result != LFG_LOCKSTATUS_OK)
            return result;
    }
    return LFG_LOCKSTATUS_OK;
}

LFGDungeonSet LFGMgr::GetRandomDungeonsForPlayer(Player* player)
{
    LFGDungeonSet list;

    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        if (LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(i))
        {
            if (dungeon &&
                dungeon->type == LFG_TYPE_RANDOM_DUNGEON &&
                GetPlayerLockStatus(player, dungeon) == LFG_LOCKSTATUS_OK)
                list.insert(dungeon);
        }
    }
    return list;
}

LFGDungeonSet LFGMgr::ExpandRandomDungeonsForGroup(LFGDungeonEntry const* randomDungeon, LFGQueueSet playerGuids)
{
    LFGDungeonSet list;
    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        if (LFGDungeonEntry const* dungeonEx = sLFGDungeonStore.LookupEntry(i))
        {
            if ((dungeonEx->type == LFG_TYPE_DUNGEON ||
                dungeonEx->type == LFG_TYPE_HEROIC_DUNGEON)
                && dungeonEx->difficulty == randomDungeon->difficulty)
            {
                bool checkPassed = true;
                for (LFGQueueSet::const_iterator itr = playerGuids.begin(); itr != playerGuids.end(); ++itr)
                {
                    Player* player = sObjectMgr.GetPlayer(*itr);

                    // Additional checks for expansion there!

                    if (!dungeonEx || GetPlayerLockStatus(player, dungeonEx) != LFG_LOCKSTATUS_OK)
                        checkPassed = false;
                }
                if (checkPassed)
                    list.insert(dungeonEx);
            }
        }
    }
    return list;
}

LFGDungeonEntry const* SelectDungeonFromList(LFGDungeonSet* dungeons)
{
    if (!dungeons || dungeons->empty())
        return NULL;
    if (dungeons->size() == 1)
        return *dungeons->begin();

    return NULL;
}

LFGDungeonEntry const* LFGMgr::GetDungeon(uint32 dungeonID)
{
    LFGDungeonMap::const_iterator itr = m_dungeonMap.find(dungeonID);
    return itr != m_dungeonMap.end() ? itr->second : NULL;
}

LFGDungeonExpansionEntry const* LFGMgr::GetDungeonExpansion(uint32 dungeonID)
{
    LFGDungeonExpansionMap::const_iterator itr = m_dungeonExpansionMap.find(dungeonID);
    return itr != m_dungeonExpansionMap.end() ? itr->second : NULL;
}

void LFGMgr::ClearLFRList(Player* player)
{
    if (!sWorld.getConfig(CONFIG_BOOL_LFG_ENABLE) && !sWorld.getConfig(CONFIG_BOOL_LFR_ENABLE))
        return;

    if (!player)
        return;

    LFGDungeonSet* dungeons = player->GetLFGState()->GetDungeons();
    dungeons->clear();
    DEBUG_LOG("LFGMgr::LFR List cleared, player %u leaving LFG queue", player->GetObjectGuid().GetCounter());
    RemoveFromQueue(player->GetObjectGuid());

}

LFGQueueSet LFGMgr::GetDungeonPlayerQueue(LFGDungeonEntry const* dungeon, Team team)
{
    ReadGuard Guard(GetLock());
    LFGQueueSet tmpSet;

    if (!dungeon)
        return tmpSet;

    LFGQueueSet* players = GetSearchVector(dungeon);
    if (!players || players->empty())
        return tmpSet;

    for (LFGQueueSet::const_iterator itr = players->begin(); itr != players->end(); ++itr)
    {
        ObjectGuid guid = (*itr);
        Player* player = sObjectMgr.GetPlayer(guid);
        if (!player)
            continue;

        if (team && player->GetTeam() != team)
            continue;

        if (player->GetLFGState()->GetState() < LFG_STATE_LFR ||
            player->GetLFGState()->GetState() > LFG_STATE_PROPOSAL)
            continue;

        tmpSet.insert(guid);
    }
    return tmpSet;
}

LFGQueueSet LFGMgr::GetDungeonPlayerQueue(LFGType type)
{
    ReadGuard Guard(GetLock());
    LFGQueueSet tmpSet;

    for (LFGQueue::const_iterator itr = m_playerQueue[type].begin(); itr != m_playerQueue[type].end(); ++itr)
    {
        ObjectGuid guid = (*itr)->guid;
        Player* player = sObjectMgr.GetPlayer(guid);
        if (!player)
            continue;

        if (player->GetLFGState()->GetState() < LFG_STATE_LFR ||
            player->GetLFGState()->GetState() > LFG_STATE_PROPOSAL)
            continue;

        tmpSet.insert(guid);
    }
    return tmpSet;
}

LFGQueueSet LFGMgr::GetDungeonGroupQueue(LFGDungeonEntry const* dungeon, Team team)
{
    ReadGuard Guard(GetLock());
    LFGQueueSet tmpSet;
    tmpSet.clear();
    LFGType type = LFG_TYPE_NONE;
    uint32 dungeonID = 0;
    uint8 searchEnd = LFG_TYPE_MAX;
    if (dungeon)
    {
        type = LFGType(dungeon->type);
        dungeonID = dungeon->ID;
        searchEnd = type + 1;
    }

    for (uint8 i = type; i < searchEnd; ++i)
    {
        for (LFGQueue::const_iterator itr = m_groupQueue->begin(); itr != m_groupQueue->end(); ++itr)
        {
            ObjectGuid guid = (*itr)->guid;
            Group* group = sObjectMgr.GetGroup(guid);
            if (!group)
                continue;

            Player* player = sObjectMgr.GetPlayer(group->GetLeaderGuid());
            if (!player)
                continue;

            if (team && player->GetTeam() != team)
                continue;

            if (player->GetLFGState()->GetState() < LFG_STATE_LFR ||
                player->GetLFGState()->GetState() > LFG_STATE_PROPOSAL)
                continue;

            if (player->GetLFGState()->GetDungeons()->find(dungeon) == player->GetLFGState()->GetDungeons()->end())
                continue;

            tmpSet.insert(guid);
        }
    }
    return tmpSet;
}

void LFGMgr::SendLFGRewards(Player* player)
{
    if (!sWorld.getConfig(CONFIG_BOOL_LFG_ENABLE) && !sWorld.getConfig(CONFIG_BOOL_LFR_ENABLE))
        return;

    Group* group = player->GetGroup();
    if (!group || !group->isLFDGroup())
    {
        DEBUG_LOG("LFGMgr::SendLFGReward: %u is not in a group or not a LFGGroup. Ignoring", player->GetObjectGuid().GetCounter());
        return;
    }

    for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* member = itr->getSource())
            if (member->IsInWorld())
                SendLFGReward(member);
    }
}

void LFGMgr::SendLFGReward(Player* player)
{
    LFGDungeonEntry const* dungeon = *player->GetLFGState()->GetDungeons()->begin();

    if (!dungeon || dungeon->type != LFG_TYPE_RANDOM_DUNGEON)
    {
        DEBUG_LOG("LFGMgr::RewardDungeonDoneFor: %u dungeon is not random", player->GetObjectGuid().GetCounter());
        return;
    }

    // Update achievements
    //    if (dungeon->difficulty == DUNGEON_DIFFICULTY_HEROIC)
    //        player->GetAchievementMgr().UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_USE_LFD_TO_GROUP_WITH_PLAYERS, 1);

    LFGReward const* reward = GetRandomDungeonReward(dungeon, player);

    if (!reward)
        return;

    uint8 index = 0;
    Quest const* qReward = sObjectMgr.GetQuestTemplate(reward->reward[index].questId);
    if (!qReward)
        return;

    // if we can take the quest, means that we haven't done this kind of "run", IE: First Heroic Random of Day.
    if (player->CanRewardQuest(qReward, false))
        player->RewardQuest(qReward, 0, NULL, false);
    else
    {
        index = 1;
        qReward = sObjectMgr.GetQuestTemplate(reward->reward[index].questId);
        if (!qReward)
            return;
        // we give reward without informing client (retail does this)
        player->RewardQuest(qReward, 0, NULL, false);
    }

    // Give rewards
    DEBUG_LOG("LFGMgr::RewardDungeonDoneFor: %u done dungeon %u, %s previously done.", player->GetObjectGuid().GetCounter(), dungeon->ID, index > 0 ? " " : " not");
    player->GetSession()->SendLfgPlayerReward(dungeon, reward, qReward, index == 0);
}

uint32 LFGMgr::CreateProposal(LFGDungeonEntry const* dungeon, Group* group, LFGQueueSet* guids)
{
    if (!dungeon)
        return false;

    uint32 ID = 0;
    if (group)
    {
        if (LFGProposal* pProposal = group->GetLFGState()->GetProposal())
        {
            ID = pProposal->ID;
        }
    }

    LFGProposal proposal = LFGProposal(dungeon);
    proposal.SetState(LFG_PROPOSAL_INITIATING);
    proposal.SetGroup(group);
    proposal.Start();
    if (ID)
    {
        WriteGuard Guard(GetLock());
        m_proposalMap.erase(ID);
        proposal.ID = ID;
        m_proposalMap.insert(std::make_pair(ID, proposal));
    }
    else
    {
        WriteGuard Guard(GetLock());
        ID = GenerateProposalID();
        proposal.ID = ID;
        m_proposalMap.insert(std::make_pair(ID, proposal));
    }

    if (guids && !guids->empty())
    {
        for (LFGQueueSet::const_iterator itr = guids->begin(); itr != guids->end(); ++itr)
        {
            if (!SendProposal(ID, *itr))
                DEBUG_LOG("LFGMgr::CreateProposal: cannot send proposal %u, dungeon %u, %s to player %u", ID, dungeon->ID, group ? " in group" : " not in group", (*itr).GetCounter());
        }
    }

    if (group)
    {
        group->GetLFGState()->SetProposal(GetProposal(ID));
    }
    DEBUG_LOG("LFGMgr::CreateProposal: %u, dungeon %u, %s", ID, dungeon->ID, group ? " in group" : " not in group");
    return ID;
}

bool LFGMgr::SendProposal(uint32 ID, ObjectGuid guid)
{
    if (guid.IsEmpty() || !ID)
        return false;

    LFGProposal* pProposal = GetProposal(ID);

    Player* player = sObjectMgr.GetPlayer(guid);

    if (!pProposal || !player)
        return false;

    pProposal->AddMember(guid);
    player->GetLFGState()->SetState(LFG_STATE_PROPOSAL);
    player->GetLFGState()->SetAnswer(LFG_ANSWER_PENDING);
    RemoveFromSearchMatrix(guid);

    player->GetSession()->SendLfgUpdateProposal(pProposal);

    DEBUG_LOG("LFGMgr::SendProposal: proposal %u, dungeon %u, %s", ID, pProposal->GetDungeon()->ID, pProposal->GetGroup() ? " in group" : " not in group");
    return true;
}

LFGProposal* LFGMgr::GetProposal(uint32 ID)
{
    LFGProposalMap::iterator itr = m_proposalMap.find(ID);
    return itr != m_proposalMap.end() ? &itr->second : NULL;
}

void LFGMgr::RemoveProposal(uint32 ID)
{
    LFGProposalMap::iterator itr = m_proposalMap.find(ID);
    if (itr == m_proposalMap.end())
        return;

    if (Group* group = (*itr).second.GetGroup())
    {
        group->GetLFGState()->SetProposal(NULL);
        m_proposalMap.erase(itr);
    }
}

uint32 LFGMgr::GenerateProposalID()
{
    uint32 newID = m_proposalID;
    ++m_proposalID;
    return newID;
}

void LFGMgr::UpdateProposal(uint32 ID, ObjectGuid guid, bool accept)
{
    // Check if the proposal exists
    LFGProposal* pProposal = GetProposal(ID);
    if (!pProposal)
        return;

    LFGQueueSet::const_iterator itr = pProposal->playerGuids.find(guid);
    if (itr == pProposal->playerGuids.end())
        return;

    Player* _player = sObjectMgr.GetPlayer(guid);

    _player->GetLFGState()->SetAnswer(LFGAnswer(accept));

    if (!accept)
    {
        // Remove member that didn't accept
        if (accept == LFG_ANSWER_DENY)
        {
            pProposal->RemoveDecliner(guid);
            RemoveFromQueue(guid);
            DEBUG_LOG("LFGMgr::UpdateProposal: %u didn't accept. Removing from queue", guid.GetCounter());
        }
        return;
    }

    // check if all have answered and reorder players (leader first)
    bool allAnswered = true;
    for (LFGQueueSet::const_iterator itr = pProposal->playerGuids.begin(); itr != pProposal->playerGuids.end(); ++itr)
    {
        if (Player* player = sObjectMgr.GetPlayer(*itr))
        {
            Group* group = player->GetGroup();
            ObjectGuid gguid = group ? group->GetObjectGuid() : ObjectGuid();

            if (player->GetLFGState()->GetAnswer() != LFG_ANSWER_AGREE)   // No answer (-1) or not accepted (0)
                allAnswered = false;

            player->GetSession()->SendLfgUpdateProposal(pProposal);
        }

    }

    if (!allAnswered)
        return;

    DEBUG_LOG("LFGMgr::UpdateProposal: all players in proposal %u answered, make group/teleport group", pProposal->ID);
    // save waittime (group maked, save statistic)

    // Create a new group (if needed)
    Group* group = pProposal->GetGroup();
    if (!group)
    {
        Player* leader = LeaderElection(&pProposal->playerGuids);

        if (leader->GetGroup())
            leader->RemoveFromGroup();

        group = new Group();
        group->Create(leader->GetObjectGuid(), leader->GetName());
        group->ConvertToLFG(pProposal->GetType());
        group->GetLFGState()->SetProposal(pProposal);
        sObjectMgr.AddGroup(group);
        // Special case to add leader to LFD group:
        AddMemberToLFDGroup(leader->GetObjectGuid());
        DEBUG_LOG("LFGMgr::UpdateProposal: in proposal %u created group %u", pProposal->ID, group->GetObjectGuid().GetCounter());
    }
    else
        if (!group->isLFDGroup())
        {
        group->ConvertToLFG(pProposal->GetType());
        for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            if (Player* member = itr->getSource())
            {
                if (member->IsInWorld())
                {
                    AddMemberToLFDGroup(member->GetObjectGuid());
                }
            }
        }
        }

    MANGOS_ASSERT(group);
    pProposal->SetGroup(group);
    group->SendUpdate();

    for (LFGQueueSet::const_iterator itr = pProposal->playerGuids.begin(); itr != pProposal->playerGuids.end(); ++itr)
    {
        if (Player* player = sObjectMgr.GetPlayer(*itr))
        {
            if (player->GetGroup() != group)
            {
                player->RemoveFromGroup();
                player->GetSession()->SendLfgUpdatePlayer(LFG_UPDATETYPE_GROUP_FOUND, player->GetLFGState()->GetType());
                group->AddMember(player->GetObjectGuid(), player->GetName());
                //                player->GetSession()->SendLfgUpdateProposal(pProposal);
            }
        }
    }

    SetGroupRoles(group);

    // Update statistics for dungeon/roles/etc

    // Set the dungeon difficulty and real dungeon for random
    MANGOS_ASSERT(pProposal->GetDungeon());
    if (!group->GetLFGState()->GetDungeon())
    {
        if (IsRandomDungeon(pProposal->GetDungeon()))
        {
            LFGQueueSet tmpSet;
            for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
                if (Player* player = itr->getSource())
                    if (player->IsInWorld())
                        tmpSet.insert(player->GetObjectGuid());

            LFGDungeonSet randomList = ExpandRandomDungeonsForGroup(pProposal->GetDungeon(), tmpSet);
            LFGDungeonEntry const* realdungeon = SelectRandomDungeonFromList(randomList);
            if (!realdungeon)
                DEBUG_LOG("LFGMgr::UpdateProposal:%u cannot set real dungeon!", pProposal->ID);
            group->GetLFGState()->SetDungeon(realdungeon);
        }
        else
            group->GetLFGState()->SetDungeon(pProposal->GetDungeon());
    }

    MANGOS_ASSERT(group->GetLFGState()->GetDungeon());
    group->SetDungeonDifficulty(Difficulty(group->GetLFGState()->GetDungeon()->difficulty));
    group->GetLFGState()->SetStatus(LFG_STATUS_NOT_SAVED);
    group->SendUpdate();

    // Teleport group
    Teleport(group, false);
    RemoveProposal(ID);
    group->GetLFGState()->SetState(LFG_STATE_DUNGEON);
}

void LFGMgr::CleanupProposals()
{
    std::set<uint32> expiredProposals;
    for (LFGProposalMap::iterator itr = m_proposalMap.begin(); itr != m_proposalMap.end(); ++itr)
    {
        if (!itr->second.IsActive())
            expiredProposals.insert(itr->second.ID);
    }
    if (!expiredProposals.empty())
    {
        for (std::set<uint32>::const_iterator itr = expiredProposals.begin(); itr != expiredProposals.end(); ++itr)
            RemoveProposal(*itr);
    }
}

void LFGMgr::OfferContinue(Group* group)
{
    if (!sWorld.getConfig(CONFIG_BOOL_LFG_ENABLE))
        return;

    if (group)
    {
        LFGDungeonEntry const* dungeon = *group->GetLFGState()->GetDungeons()->begin();

        if (!dungeon)
        {
            DEBUG_LOG("LFGMgr::OfferContinue: group %u not have selected dungeon!", group->GetObjectGuid().GetCounter());
            return;
        }
        if (Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid()))
            leader->GetSession()->SendLfgOfferContinue(dungeon);
    }
    else
        sLog.outError("LFGMgr::OfferContinue: no group!");
}

void LFGMgr::InitBoot(Player* kicker, ObjectGuid victimGuid, std::string reason)
{
    Group* group = kicker->GetGroup();
    Player* victim = sObjectMgr.GetPlayer(victimGuid);

    if (!group || !victim)
        return;

    group->GetLFGState()->SetState(LFG_STATE_BOOT);

    LFGPlayerBoot boot;
    boot.inProgress = true;
    boot.cancelTime = time_t(time(NULL)) + LFG_TIME_BOOT;
    boot.reason = reason;
    boot.victim = victimGuid;
    boot.votedNeeded = group->GetLFGState()->GetVotesNeeded();

    // Set votes
    for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* player = itr->getSource())
        {
            player->GetLFGState()->SetState(LFG_STATE_BOOT);;
            if (player == victim)
                boot.votes[victimGuid] = LFG_ANSWER_DENY;    // Victim auto vote NO
            else if (player == kicker)
                boot.votes[kicker->GetObjectGuid()] = LFG_ANSWER_AGREE;   // Kicker auto vote YES
            else
            {
                boot.votes[player->GetObjectGuid()] = LFG_ANSWER_PENDING;   // Other members need to vote
            }
        }
    }
    m_bootMap.insert(std::make_pair(group->GetObjectGuid(), boot));

    LFGPlayerBoot* pBoot = GetBoot(group->GetObjectGuid());

    if (!pBoot)
        return;

    // Notify players
    for (GroupReference *itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();

        if (player && player->IsInWorld())
            player->GetSession()->SendLfgBootPlayer(pBoot);
    }
}

LFGPlayerBoot* LFGMgr::GetBoot(ObjectGuid guid)
{
    ReadGuard Guard(GetLock());
    LFGBootMap::iterator itr = m_bootMap.find(guid);
    return itr != m_bootMap.end() ? &itr->second : NULL;
}

void LFGMgr::DeleteBoot(ObjectGuid guid)
{
    WriteGuard Guard(GetLock());
    LFGBootMap::iterator itr = m_bootMap.find(guid);
    if (itr != m_bootMap.end())
        m_bootMap.erase(itr);
}

void LFGMgr::UpdateBoot(Player* player, bool accept)
{
    Group* group = player ? player->GetGroup() : NULL;

    if (!group)
        return;

    LFGPlayerBoot* pBoot = GetBoot(group->GetObjectGuid());

    if (!pBoot)
        return;

    if (pBoot->votes[player->GetObjectGuid()] != LFG_ANSWER_PENDING)          // Cheat check: Player can't vote twice
        return;

    Player* victim = sObjectMgr.GetPlayer(pBoot->victim);
    if (!victim)
        return;

    pBoot->votes[player->GetObjectGuid()] = LFGAnswer(accept);

    uint8 votesNum = 0;
    uint8 agreeNum = 0;

    for (LFGAnswerMap::const_iterator itVotes = pBoot->votes.begin(); itVotes != pBoot->votes.end(); ++itVotes)
    {
        if (itVotes->second != LFG_ANSWER_PENDING)
        {
            ++votesNum;
            if (itVotes->second == LFG_ANSWER_AGREE)
                ++agreeNum;
        }
    }

    if (agreeNum >= pBoot->votedNeeded ||                  // Vote passed
        votesNum >= pBoot->votes.size() ||                 // All voted but not passed
        (pBoot->votes.size() - votesNum + agreeNum) < pBoot->votedNeeded) // Vote didnt passed
    {
        // Send update info to all players
        pBoot->inProgress = false;
        for (LFGAnswerMap::const_iterator itVotes = pBoot->votes.begin(); itVotes != pBoot->votes.end(); ++itVotes)
        {
            Player* pPlayer = sObjectMgr.GetPlayer(itVotes->first);
            if (pPlayer && (pPlayer != victim))
            {
                pPlayer->GetLFGState()->SetState(LFG_STATE_DUNGEON);
                pPlayer->GetSession()->SendLfgBootPlayer(pBoot);
            }
        }

        group->GetLFGState()->SetState(LFG_STATE_DUNGEON);

        if (agreeNum == pBoot->votedNeeded)                // Vote passed - Kick player
        {
            Player::RemoveFromGroup(group, victim->GetObjectGuid());
            Teleport(victim, true, false);
            victim->GetLFGState()->Clear();
            OfferContinue(group);
            group->GetLFGState()->DecreaseKicksLeft();
        }
        DeleteBoot(group->GetObjectGuid());
    }
}

void LFGMgr::Teleport(Group* group, bool out)
{
    if (!group)
        return;

    DEBUG_LOG("LFGMgr::TeleportGroup %u in dungeon!", group->GetObjectGuid().GetCounter());

    for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* member = itr->getSource())
        {
            if (member->IsInWorld())
            {
                if (!member->GetLFGState()->IsTeleported() && !out)
                    Teleport(member, out);
                else if (out)
                    Teleport(member, out);
            }
        }
    }
    if (group->GetLFGState()->GetState() == LFG_STATE_LFG
        || group->GetLFGState()->GetState() == LFG_STATE_LFR)
        group->GetLFGState()->SetState(LFG_STATE_DUNGEON);
}

void LFGMgr::Teleport(Player* player, bool out, bool fromOpcode /*= false*/)
{
    DEBUG_LOG("LFGMgr::TeleportPlayer: %u is being teleported %s", player->GetObjectGuid().GetCounter(), out ? "from dungeon." : "in dungeon.");

    if (out)
    {
        player->RemoveAurasDueToSpell(LFG_SPELL_LUCK_OF_THE_DRAW);
        player->TeleportToBGEntryPoint();
        return;
    }

    // TODO Add support for LFG_TELEPORTERROR_FATIGUE
    LFGTeleportError error = LFG_TELEPORTERROR_OK;

    Group* group = player->GetGroup();

    if (!group)
        error = LFG_TELEPORTERROR_INVALID_LOCATION;
    else if (!player->isAlive())
        error = LFG_TELEPORTERROR_PLAYER_DEAD;
    //    else if (player->IsFalling())
    //        error = LFG_TELEPORTERROR_FALLING;

    uint32 mapid = 0;
    float x = 0;
    float y = 0;
    float z = 0;
    float orientation = 0;
    Difficulty difficulty;

    LFGDungeonEntry const* dungeon = group->GetLFGState()->GetDungeon();
    if (!dungeon)
        error = LFG_TELEPORTERROR_INVALID_LOCATION;

    if (error == LFG_TELEPORTERROR_OK)
    {
        difficulty = Difficulty(dungeon->difficulty);
        bool leaderInDungeon = false;
        Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
        if (leader && player != leader && leader->GetMapId() == uint32(dungeon->map))
            leaderInDungeon = true;

        if (group->GetDungeonDifficulty() != Difficulty(dungeon->difficulty))
        {
            error = LFG_TELEPORTERROR_UNK4;
        }
        else if (GetPlayerLockStatus(player, dungeon) != LFG_LOCKSTATUS_OK)
        {
            error = LFG_TELEPORTERROR_INVALID_LOCATION;
        }
        else if (leaderInDungeon && group->GetLFGState()->GetState() == LFG_STATE_DUNGEON)
        {
            mapid = leader->GetMapId();
            x = leader->GetPositionX();
            y = leader->GetPositionY();
            z = leader->GetPositionZ();
            orientation = leader->GetOrientation();
        }
        else if (AreaTrigger const* at = sObjectMgr.GetMapEntranceTrigger(dungeon->map))
        {
            mapid = at->target_mapId;
            x = at->target_X;
            y = at->target_Y;
            z = at->target_Z;
            orientation = at->target_Orientation;
        }
        else
            error = LFG_TELEPORTERROR_INVALID_LOCATION;
    }

    if (error == LFG_TELEPORTERROR_OK)
    {

        if (player->GetMap() && !player->GetMap()->IsDungeon() && !player->GetMap()->IsRaid() && !player->InBattleGround())
            player->SetBattleGroundEntryPoint();

        // stop taxi flight at port
        if (player->IsTaxiFlying())
        {
            player->GetMotionMaster()->MovementExpired();
            player->m_taxi.ClearTaxiDestinations();
        }

        player->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);
        player->RemoveSpellsCausingAura(SPELL_AURA_FLY);

        DETAIL_LOG("LFGMgr: Sending %s to map %u, difficulty %u X %f, Y %f, Z %f, O %f", player->GetName(), uint8(difficulty), mapid, x, y, z, orientation);

        player->TeleportTo(mapid, x, y, z, orientation);
        player->GetLFGState()->SetState(LFG_STATE_DUNGEON);
        player->GetLFGState()->SetTeleported();
    }
    else
        player->GetSession()->SendLfgTeleportError(error);
}

void LFGMgr::UpdateRoleCheck(Group* group)
{
    if (!group)
        return;

    if (!group->GetLFGState()->IsRoleCheckActive())
        return;


    LFGRoleCheckState state = group->GetLFGState()->GetRoleCheckState();
    LFGRoleCheckState newstate = LFG_ROLECHECK_NONE;

    for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* member = itr->getSource())
        {
            if (member->IsInWorld())
            {
                if (member->GetLFGState()->GetState() != LFG_STATE_ROLECHECK && member->GetLFGState()->GetState() != LFG_STATE_QUEUED)
                {
                    member->GetLFGState()->SetState(LFG_STATE_ROLECHECK);
                    member->GetSession()->SendLfgRoleCheckUpdate();
                    newstate = LFG_ROLECHECK_INITIALITING;
                }
                else if (uint8(member->GetLFGState()->GetRoles()) < LFG_ROLE_MASK_TANK)
                    newstate = LFG_ROLECHECK_MISSING_ROLE;
                else if (!member->GetLFGState()->IsSingleRole())
                    newstate = LFG_ROLECHECK_INITIALITING;
            }
        }
    }

    if (newstate == LFG_ROLECHECK_NONE && !CheckRoles(group))
        newstate == LFG_ROLECHECK_WRONG_ROLES;
    else if (newstate == LFG_ROLECHECK_NONE)
        newstate = LFG_ROLECHECK_FINISHED;

    // time query to end rolecheck
    if (newstate != LFG_ROLECHECK_FINISHED)
    {
        // time for rolecheck is up
        if (group->GetLFGState()->QueryRoleCheckTime())
        {
            group->GetLFGState()->Clear();

            for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                if (Player* member = itr->getSource())
                {
                    if (member->IsInWorld())
                    {
                        member->GetSession()->SendLfgRoleCheckUpdate();
                        if (member->GetObjectGuid() == group->GetLeaderGuid())
                            member->GetSession()->SendLfgJoinResult(ERR_LFG_ROLE_CHECK_FAILED, LFG_ROLECHECK_MISSING_ROLE);
                    }
                }
            }
            return;
        }
    }


    if (newstate == LFG_ROLECHECK_WRONG_ROLES)
        return;

    group->GetLFGState()->SetRoleCheckState(LFG_ROLECHECK_FINISHED);
    Player* leader = sObjectMgr.GetPlayer(group->GetLeaderGuid());
    if (leader && leader->IsInWorld())
        leader->GetSession()->SendLfgUpdateParty(LFG_UPDATETYPE_ADDED_TO_QUEUE, group->GetLFGState()->GetDungeonType());

}

bool LFGMgr::CheckRoles(Group* group, Player* player /*=NULL*/)
{
    if (!group)
        return false;

    if (group->isRaidGroup())
        return true;

    LFGRolesMap rolesMap;

    for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* member = itr->getSource())
            if (member->IsInWorld())
                rolesMap.insert(std::make_pair(member->GetObjectGuid(), member->GetLFGState()->GetRoles()));
    }

    if (player && player->IsInWorld())
        rolesMap.insert(std::make_pair(player->GetObjectGuid(), player->GetLFGState()->GetRoles()));

    return CheckRoles(&rolesMap);
}

bool LFGMgr::CheckRoles(LFGRolesMap* rolesMap)
{
    if (!rolesMap || rolesMap->size())
        return false;

    if (sWorld.getConfig(CONFIG_BOOL_LFG_DEBUG_ENABLE))
        return true;

    if (rolesMap->size() > MAX_GROUP_SIZE)
        return false;

    uint8 tanks = LFG_TANKS_NEEDED;
    uint8 healers = LFG_HEALERS_NEEDED;
    uint8 dps = LFG_DPS_NEEDED;


    for (LFGRolesMap::const_iterator itr = rolesMap->begin(); itr != rolesMap->end(); ++itr)
    {
        Player* member = sObjectMgr.GetPlayer((*itr).first);
        if (member->IsInWorld())
        {
            if ((*itr).second & LFG_ROLE_MASK_TANK && tanks > 0)
                --tanks;
            else if ((*itr).second & LFG_ROLE_MASK_HEALER && healers > 0)
                --healers;
            else if ((*itr).second & LFG_ROLE_MASK_DAMAGE && dps > 0)
                --dps;
        }
    }

    if ((healers + tanks + dps) > (MAX_GROUP_SIZE - rolesMap->size()))
        return false;

    return true;
}

bool LFGMgr::RoleChanged(Player* player, uint8 roles)
{
    uint8 oldRoles = player->GetLFGState()->GetRoles();
    player->GetLFGState()->SetRoles(roles);

    if (CheckRoles(player->GetGroup()))
    {
        player->GetSession()->SendLfgRoleChosen(player->GetObjectGuid(), roles);
        player->GetLFGState()->SetState(LFG_STATE_QUEUED);
        return true;
    }
    else
    {
        player->GetSession()->SendLfgUpdatePlayer(LFG_UPDATETYPE_ROLECHECK_FAILED, player->GetGroup()->GetLFGState()->GetDungeonType());
        player->GetLFGState()->SetRoles(oldRoles);
        player->GetSession()->SendLfgRoleCheckUpdate();
        return false;
    }
}

Player* LFGMgr::LeaderElection(LFGQueueSet* playerGuids)
{
    std::set<Player*> leaders;
    Player* leader = NULL;
    uint32 GS = 0;

    for (LFGQueueSet::const_iterator itr = playerGuids->begin(); itr != playerGuids->end(); ++itr)
    {
        Player* member = sObjectMgr.GetPlayer(*itr);
        if (member->IsInWorld())
        {
            if (member->GetLFGState()->GetRoles() & LFG_ROLE_MASK_LEADER)
                leaders.insert(member);

            member->GetLFGState()->RemoveRole(ROLE_LEADER);

            if (member->GetEquipGearScore() > GS)
            {
                GS = member->GetEquipGearScore();
                leader = member;
            }
        }
    }

    GS = 0;
    if (!leaders.empty())
    {
        for (std::set<Player*>::const_iterator itr = leaders.begin(); itr != leaders.end(); ++itr)
        {
            if ((*itr)->GetEquipGearScore() > GS)
            {
                GS = (*itr)->GetEquipGearScore();
                leader = (*itr);
            }
        }
    }
    MANGOS_ASSERT(leader);
    leader->GetLFGState()->AddRole(ROLE_LEADER);
    return leader;
}

void LFGMgr::SetGroupRoles(Group* group)
{
    if (!group)
        return;

    LFGRolesMap rolesMap;
    bool hasMultiRoles = false;

    for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* member = itr->getSource())
        {
            if (member->IsInWorld())
            {
                rolesMap.insert(std::make_pair(member->GetObjectGuid(), member->GetLFGState()->GetRoles()));
                if (!member->GetLFGState()->IsSingleRole())
                    hasMultiRoles = true;
            }
        }
    }

    if (!hasMultiRoles)
        return;

    LFGRoleMask oldRoles;

    // strip double roles
    for (LFGRolesMap::iterator itr = rolesMap.begin(); itr != rolesMap.end(); ++itr)
    {
        if ((*itr).second & LFG_ROLE_MASK_TANK)
        {
            oldRoles = (*itr).second;
            (*itr).second = LFGRoleMask((*itr).second & ~LFG_ROLE_MASK_HD);
            if (!CheckRoles(&rolesMap))
                (*itr).second = oldRoles;
        }

        if ((*itr).second & LFG_ROLE_MASK_HEALER)
        {
            oldRoles = (*itr).second;
            (*itr).second = LFGRoleMask((*itr).second & ~LFG_ROLE_MASK_TD);
            if (!CheckRoles(&rolesMap))
                (*itr).second = oldRoles;
        }

        if ((*itr).second & LFG_ROLE_MASK_DAMAGE)
        {
            oldRoles = (*itr).second;
            (*itr).second = LFGRoleMask((*itr).second & ~LFG_ROLE_MASK_TH);
            if (!CheckRoles(&rolesMap))
                (*itr).second = oldRoles;
        }
    }

    for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* member = itr->getSource())
        {
            if (member->IsInWorld())
            {
                LFGRolesMap::iterator itr = rolesMap.find(member->GetObjectGuid());
                member->GetLFGState()->SetRoles((*itr).second);
                group->SetGroupRoles(member->GetObjectGuid(), (*itr).second);
            }
        }
    }

}

void LFGMgr::TruCompleteGroups(LFGType type)
{
    if (m_groupQueue[type].empty())
        return;

    bool isGroupCompleted = false;  // we make only one group for iterations! not more!

    for (LFGQueue::iterator itr = m_groupQueue[type].begin(); itr != m_groupQueue[type].end(); ++itr)
    {
        Group* group = sObjectMgr.GetGroup((*itr)->guid);
        if (!group)
            continue;

        LFGDungeonSet* groupDungeons = group->GetLFGState()->GetDungeons();
        for (LFGQueue::iterator itr2 = m_playerQueue[type].begin(); itr2 != m_playerQueue[type].end(); ++itr2)
        {
            Player* player = sObjectMgr.GetPlayer((*itr)->guid);
            if (!player)
                continue;

            LFGDungeonSet* playerDungeons = player->GetLFGState()->GetDungeons();
            LFGDungeonSet  intersection;
            std::set_intersection(groupDungeons->begin(), groupDungeons->end(), playerDungeons->begin(), playerDungeons->end(), std::inserter(intersection, intersection.end()));

            if (intersection.size() < 1)
                continue;

            LFGDungeonEntry const* dungeon = *intersection.begin();
            // may be random?


            if (TruCompleteGroup(group, player))
            {
                switch (type)
                {
                case LFG_TYPE_DUNGEON:
                case LFG_TYPE_QUEST:
                case LFG_TYPE_ZONE:
                case LFG_TYPE_HEROIC_DUNGEON:
                case LFG_TYPE_RANDOM_DUNGEON:
                {
                    if (LFGProposal* pProposal = group->GetLFGState()->GetProposal())
                    {
                        if (!pProposal->IsDecliner(player->GetObjectGuid()))
                            pProposal->AddMember(player->GetObjectGuid());
                    }
                    else
                    {
                        uint32 ID = CreateProposal(dungeon, group);
                        if (LFGProposal* pProposal = GetProposal(ID))
                            pProposal->AddMember(player->GetObjectGuid());
                    }
                    if (group->GetMembersCount() + 1 == MAX_GROUP_SIZE)
                        isGroupCompleted = true;
                    break;
                }
                case LFG_TYPE_RAID:
                {
                    switch (dungeon->difficulty)
                    {
                    case RAID_DIFFICULTY_10MAN_NORMAL:
                    case RAID_DIFFICULTY_10MAN_HEROIC:
                        if (group->GetMembersCount() + 1 == 10)
                            isGroupCompleted = true;
                        break;
                    case RAID_DIFFICULTY_25MAN_NORMAL:
                    case RAID_DIFFICULTY_25MAN_HEROIC:
                        if (group->GetMembersCount() + 1 == 25)
                            isGroupCompleted = true;
                    default:
                        break;
                    }
                    break;
                }
                default:
                    break;
                }
            }
            if (isGroupCompleted)
                break;
        }
        if (isGroupCompleted)
            break;
    }
}

bool LFGMgr::TruCompleteGroup(Group* group, Player* player)
{
    if (!CheckRoles(group, player))
        return false;

    if (HasIgnoreState(group, player->GetObjectGuid()))
        return false;

    if (LFGProposal* pProposal = group->GetLFGState()->GetProposal())
    {
        SendProposal(pProposal->ID, player->GetObjectGuid());
    }
    else
    {
        LFGQueueSet tmpSet;
        tmpSet.insert(player->GetObjectGuid());
        LFGDungeonEntry const* dungeon = SelectRandomDungeonFromList(*group->GetLFGState()->GetDungeons());
        uint32 ID = CreateProposal(dungeon, group, &tmpSet);
    }

    return true;
}

bool LFGMgr::TruCreateGroup(LFGType type)
{
    bool groupCreated = false;
    for (LFGSearchMap::const_iterator itr = m_searchMatrix.begin(); itr != m_searchMatrix.end(); ++itr)
    {
        if (itr->first->type != type)
            continue;

        if (itr->second.empty())
            continue;

        if (itr->second.size() < MAX_GROUP_SIZE && !sWorld.getConfig(CONFIG_BOOL_LFG_DEBUG_ENABLE))
            continue;

        LFGDungeonSet intersection;
        LFGQueueSet newGroup;
        LFGQueueSet const* applicants = &itr->second;
        for (LFGQueueSet::const_iterator itr1 = applicants->begin(); itr1 != applicants->end(); ++itr1)
        {
            ObjectGuid guid = *itr1;
            bool checkPassed = true;
            for (LFGQueueSet::const_iterator itr2 = newGroup.begin(); itr2 != newGroup.end(); ++itr2)
            {
                if (HasIgnoreState(guid, *itr2))
                    checkPassed = false;
            }
            if (!checkPassed)
                continue;
            // rolecheck there!
            newGroup.insert(guid);
            if (newGroup.size() == 1)
                intersection = *sObjectMgr.GetPlayer(guid)->GetLFGState()->GetDungeons();
            else
            {
                LFGDungeonSet groupDungeons = intersection;
                intersection.clear();
                LFGDungeonSet* playerDungeons = sObjectMgr.GetPlayer(guid)->GetLFGState()->GetDungeons();
                std::set_intersection(groupDungeons.begin(), groupDungeons.end(), playerDungeons->begin(), playerDungeons->end(), std::inserter(intersection, intersection.end()));
            }

            if (newGroup.size() == MAX_GROUP_SIZE ||
                (newGroup.size() >1 && sWorld.getConfig(CONFIG_BOOL_LFG_DEBUG_ENABLE)))
                groupCreated = true;

            if (groupCreated)
                break;
        }
        //        DEBUG_LOG("LFGMgr:TryCreateGroup: Try create group to dungeon %u from %u players. result is %u", itr->first->ID, itr->second.size(), uint8(groupCreated));
        if (groupCreated)
        {
            LFGDungeonEntry const* dungeon = SelectRandomDungeonFromList(intersection);
            uint32 ID = CreateProposal(dungeon, NULL, &newGroup);
            return true;
        }
    }
    return false;
}

LFGQueueStatus* LFGMgr::GetDungeonQueueStatus(LFGType type)
{
    return &m_queueStatus[type];
}

void LFGMgr::SetDungeonQueueStatus(LFGType type)
{
    LFGQueueStatus* status = GetDungeonQueueStatus(type);

    LFGQueueSet players = GetDungeonPlayerQueue(type);

    if (players.empty())
        return;

    uint32 damagers = 0;
    uint32 damagersTime = 0;

    uint32 healers = 0;
    uint32 healersTime = 0;

    uint32 tanks = 0;
    uint32 tanksTime = 0;

    for (LFGQueueSet::iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* player = sObjectMgr.GetPlayer(*itr);
        if (player && player->IsInWorld())
        {
            if (player->GetLFGState()->GetRoles() & LFG_ROLE_MASK_TANK)
            {
                ++tanks;
                tanksTime += uint32(time(NULL) - player->GetLFGState()->GetJoinTime());
            }

            if (player->GetLFGState()->GetRoles() & LFG_ROLE_MASK_HEALER)
            {
                ++healers;
                healersTime += uint32(time(NULL) - player->GetLFGState()->GetJoinTime());
            }

            if (player->GetLFGState()->GetRoles() & LFG_ROLE_MASK_DAMAGE)
            {
                ++damagers;
                damagersTime += uint32(time(NULL) - player->GetLFGState()->GetJoinTime());
            }
        }
    }
    status->dps = damagers;
    status->tanks = tanks;
    status->healers = healers;

    status->waitTimeTanks = tanks ? time_t(tanksTime / tanks) : 0;
    status->waitTimeHealer = healers ? time_t(healersTime / healers) : 0;
    status->waitTimeDps = damagers ? time_t(damagersTime / damagers) : 0;
}

void LFGMgr::UpdateQueueStatus(Player* player)
{
}

void LFGMgr::UpdateStatistic(LFGType type)
{
    for (LFGSearchMap::const_iterator itr = m_searchMatrix.begin(); itr != m_searchMatrix.end(); ++itr)
    {
        if (itr->first->type != type)
            continue;

        if (itr->second.empty())
            continue;
    }
}

void LFGMgr::AddToSearchMatrix(ObjectGuid guid)
{
    if (!guid.IsPlayer())
        return;

    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player)
        return;

    LFGDungeonSet* dungeons = player->GetLFGState()->GetDungeons();

    DEBUG_LOG("LFGMgr::AddToSearchMatrix %u added, dungeons size %u", guid.GetCounter(), dungeons->size());

    if (dungeons->empty())
        return;

    for (LFGDungeonSet::const_iterator itr = dungeons->begin(); itr != dungeons->end(); ++itr)
    {
        LFGDungeonEntry const* dungeon = *itr;

        if (!dungeon)
            continue;

        LFGQueueSet* players = GetSearchVector(dungeon);
        if (!players || players->empty())
        {
            WriteGuard Guard(GetLock());
            LFGQueueSet _players;
            _players.insert(guid);
            m_searchMatrix.insert(std::make_pair(dungeon, _players));
        }
        else
        {
            WriteGuard Guard(GetLock());
            players->insert(guid);
        }
    }
}

void LFGMgr::RemoveFromSearchMatrix(ObjectGuid guid)
{
    if (!guid.IsPlayer())
        return;

    Player* player = sObjectMgr.GetPlayer(guid);
    if (!player)
        return;


    LFGDungeonSet* dungeons = player->GetLFGState()->GetDungeons();

    DEBUG_LOG("LFGMgr::RemoveFromSearchMatrix %u removed, dungeons size %u", guid.GetCounter(), dungeons->size());

    if (dungeons->empty())
        return;

    for (LFGDungeonSet::const_iterator itr = dungeons->begin(); itr != dungeons->end(); ++itr)
    {
        LFGDungeonEntry const* dungeon = *itr;

        if (!dungeon)
            continue;

        LFGQueueSet* players = GetSearchVector(dungeon);
        if (players && !players->empty())
        {
            WriteGuard Guard(GetLock());
            LFGQueueSet _players;
            players->erase(guid);
            if (players->empty())
            {
                WriteGuard Guard(GetLock());
                m_searchMatrix.erase(dungeon);
            }
        }
    }
}

LFGQueueSet* LFGMgr::GetSearchVector(LFGDungeonEntry const* dungeon)
{
    ReadGuard Guard(GetLock());
    LFGSearchMap::iterator itr = m_searchMatrix.find(dungeon);
    return itr != m_searchMatrix.end() ? &itr->second : NULL;
}

bool LFGMgr::IsInSearchFor(LFGDungeonEntry const* dungeon, ObjectGuid guid)
{
    LFGQueueSet* players = GetSearchVector(dungeon);
    if (!dungeon)
        return false;

    if (players->find(guid) != players->end())
        return true;
    else return false;
}

void LFGMgr::CleanupSearchMatrix()
{
    WriteGuard Guard(GetLock());
    for (LFGSearchMap::iterator itr = m_searchMatrix.begin(); itr != m_searchMatrix.end(); itr++)
    {
        LFGQueueSet players = itr->second;
        for (LFGQueueSet::iterator itr2 = players.begin(); itr2 != players.end();)
        {
            Player* player = sObjectMgr.GetPlayer(*itr2);
            if (!player || !player->IsInWorld())
                players.erase(itr2);
            else
                ++itr2;
        }
    }
}

bool LFGMgr::HasIgnoreState(ObjectGuid guid1, ObjectGuid guid2)
{
    Player* player = sObjectMgr.GetPlayer(guid1);
    if (!player || !player->IsInWorld())
        return false;

    if (player->GetSocial()->HasIgnore(guid2))
        return true;

    return false;
}

bool LFGMgr::HasIgnoreState(Group* group, ObjectGuid guid)
{
    if (!group)
        return false;

    for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
    {
        if (Player* member = itr->getSource())
            if (HasIgnoreState(member->GetObjectGuid(), guid))
                return true;
    }

    return false;
}

LFGDungeonEntry const* LFGMgr::SelectRandomDungeonFromList(LFGDungeonSet dungeons)
{
    if (dungeons.empty())
    {
        DEBUG_LOG("LFGMgr::SelectRandomDungeonFromList cannot select dungeons from empty list!");
        return NULL;
    }

    if (dungeons.size() == 1)
        return *dungeons.begin();
    else
    {
        uint32 rand = urand(0, dungeons.size() - 1);
        uint32 _key = 0;
        for (LFGDungeonSet::const_iterator itr = dungeons.begin(); itr != dungeons.end(); ++itr)
        {
            LFGDungeonEntry const* dungeon = *itr;
            if (!dungeon)
                continue;
            if (_key == rand)
                return dungeon;
            else ++_key;
        }
    }
    return NULL;
}

void LFGMgr::UpdateLFRGroups()
{
    LFGType type = LFG_TYPE_RAID;

    LFGQueue tmpQueue = m_groupQueue[type];

    for (LFGQueue::const_iterator itr = tmpQueue.begin(); itr != tmpQueue.end(); ++itr)
    {
        ObjectGuid guid = (*itr)->guid;
        Group* group = sObjectMgr.GetGroup(guid);
        if (!group || !group->isLFRGroup())
            continue;

        if (!IsGroupCompleted(group))
            continue;

        if (!group->GetLFGState()->GetDungeon())
        {
            LFGDungeonEntry const* realdungeon = SelectRandomDungeonFromList(*group->GetLFGState()->GetDungeons());
            if (!realdungeon)
            {
                DEBUG_LOG("LFGMgr::UpdateLFRGroup:%u cannot set real dungeon! Try dungeon from leader list.", group->GetObjectGuid().GetCounter());
                LFGDungeonEntry const* realdungeon = SelectRandomDungeonFromList(*sObjectMgr.GetPlayer(group->GetLeaderGuid())->GetLFGState()->GetDungeons());
            }
            group->GetLFGState()->SetDungeon(realdungeon);
        }

        for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            if (Player* member = itr->getSource())
            {
                if (member->IsInWorld())
                {
                    if (member->GetLFGState()->GetState() < LFG_STATE_DUNGEON)
                    {
                        AddMemberToLFDGroup(member->GetObjectGuid());
                        member->GetLFGState()->SetState(LFG_STATE_DUNGEON);
                    }
                }
            }
        }
        Teleport(group, false);
    }
}

bool LFGMgr::IsGroupCompleted(Group* group, uint8 addMembers)
{
    if (!group)
        return false;

    if (sWorld.getConfig(CONFIG_BOOL_LFG_DEBUG_ENABLE) && (group->GetMembersCount() + addMembers > 1))
        return true;

    if (group->isRaidGroup())
    {
        switch (group->GetDifficulty(true))
        {
        case RAID_DIFFICULTY_10MAN_NORMAL:
        case RAID_DIFFICULTY_10MAN_HEROIC:
            if (group->GetMembersCount() + addMembers >= 10)
                return true;
            break;
        case RAID_DIFFICULTY_25MAN_NORMAL:
        case RAID_DIFFICULTY_25MAN_HEROIC:
            if (group->GetMembersCount() + addMembers >= 25)
                return true;
            break;
        default:
            return false;
            break;
        }
    }
    else if (group->GetMembersCount() + addMembers >= MAX_GROUP_SIZE)
        return true;

    return false;
}

void LFGMgr::AddMemberToLFDGroup(ObjectGuid guid)
{
    Player* player = sObjectMgr.GetPlayer(guid);

    if (!player || !player->IsInWorld())
        return;

    Group* group = player->GetGroup();

    if (!group)
        return;

    Leave(player);
    player->CastSpell(player, LFG_SPELL_LUCK_OF_THE_DRAW, true);
    player->CastSpell(player, LFG_SPELL_DUNGEON_COOLDOWN, true);

    player->GetLFGState()->SetState(group->isRaidGroup() ? LFG_STATE_LFR : LFG_STATE_LFG);
}

void LFGMgr::RemoveMemberFromLFDGroup(ObjectGuid guid)
{
    Player* player = sObjectMgr.GetPlayer(guid);

    if (!player || !player->IsInWorld())
        return;

    if (player->HasAura(LFG_SPELL_DUNGEON_COOLDOWN))
        player->CastSpell(player, LFG_SPELL_DUNGEON_DESERTER, true);
    player->RemoveAurasDueToSpell(LFG_SPELL_LUCK_OF_THE_DRAW);

    if (player->GetLFGState()->GetState() == LFG_STATE_DUNGEON
        || player->GetLFGState()->GetState() == LFG_STATE_FINISHED_DUNGEON)
        Teleport(player, true);

    player->GetLFGState()->SetState(LFG_STATE_NONE);
}