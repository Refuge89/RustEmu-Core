/*
 * Copyright (C) 2011-2013 /dev/rsa for MangosR2 <http://github.com/MangosR2>
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
/* StateMgr based on idea and part of code from SilverIce (http:://github.com/SilverIce
*/
#ifndef _STATEMGR_H
#define _STATEMGR_H

#include "Common.h"
#include "MotionMaster.h"
#include "StateMgrImpl.h"
#include "MovementGenerator.h"
#include "Log.h"
#include <sstream>
#include <algorithm>
#pragma once

class Unit;
class UnitStateMgr;

class ActionInfo
{
public:
    ActionInfo(UnitActionId id, UnitAction* action, UnitActionPriority priority, bool restoreable)
        : m_id(id), m_action(action), m_priority(priority), m_flags(0), m_restoreable(restoreable) {}

    ~ActionInfo() {}

    bool operator == (ActionInfo& val);
    bool operator == (UnitAction* action);
    bool operator != (ActionInfo& val);
    bool operator != (UnitAction* action);

    void Delete();
    void Reset(UnitStateMgr* mgr);
    void Initialize(UnitStateMgr* mgr);
    void Finalize(UnitStateMgr* mgr);
    void Interrupt(UnitStateMgr* mgr);
    bool Update(UnitStateMgr* mgr, uint32 diff);
    UnitAction* Action() { return m_action; };

    const char* TypeName() const;

    UnitActionId GetId() const             { return m_id; };
    UnitActionPriority GetPriority() const { return m_priority; };

    uint32 const&  GetFlags();
    void           SetFlags(uint32 flags);
    void           AddFlag(ActionUpdateState state) { m_flags |= (1 << state); };
    void           RemoveFlag(ActionUpdateState state) { m_flags &= ~(1 << state); };
    bool           HasFlag(ActionUpdateState state) const { return (m_flags & (1 << state)); };

    UnitActionId       m_id;
    UnitAction*        m_action;
    UnitActionPriority m_priority;
    uint32             m_flags;
    bool               m_restoreable;

private:
    // Don't must be created uninitialized
    ActionInfo() {}
};

typedef std::map<UnitActionPriority, ActionInfo> UnitActionStorage;

class MANGOS_DLL_SPEC UnitStateMgr
{

protected:
    UnitStateMgr(const UnitStateMgr&);
    UnitStateMgr& operator = (const UnitStateMgr&);

public:
    explicit UnitStateMgr(Unit* owner);
    ~UnitStateMgr();

    void InitDefaults(bool immediate = true);

    void Update(uint32 diff);

    static UnitAction* CreateStandartState(UnitActionId stateId, ...);

    void DropAction(UnitActionId actionId);
    void DropAction(UnitActionId actionId, UnitActionPriority priority);
    void DropAction(UnitActionPriority priority);
    void DropActionHigherThen(UnitActionPriority priority);

    void DropAllStates();

    void PushAction(UnitActionId actionId);
    void PushAction(UnitActionId actionId, UnitActionPriority priority);
    void PushAction(UnitActionId actionId, UnitAction* state);
    void PushAction(UnitActionId actionId, UnitAction* state, UnitActionPriority priority, eActionType restoreable);

    ActionInfo* GetAction(UnitActionPriority priority);
    ActionInfo* GetAction(UnitAction* action);
    ActionInfo* GetAction(UnitActionId actionId);

    UnitActionStorage const& GetActions() { return m_actions; };

    UnitAction*   CurrentAction();
    ActionInfo*   CurrentState();

    UnitActionId  GetCurrentState()  const { return m_actions.empty() ? UNIT_ACTION_IDLE : m_actions.rbegin()->second.GetId(); };
    Unit*         GetOwner()         const { return m_owner; };

    std::string const GetOwnerStr();

    // State counters
    uint32 const&     GetCounter(UnitActionId id) { return m_stateCounter[id]; };
    void              IncreaseCounter(UnitActionId id) { ++m_stateCounter[id]; };

private:
    UnitActionStorage m_actions;
    Unit*             m_owner;
    UnitAction*       m_oldAction;
    uint32            m_stateCounter[UNIT_ACTION_END];
    bool              m_needReinit;
};

#endif
