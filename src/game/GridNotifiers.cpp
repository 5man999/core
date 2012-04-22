/*
 * Copyright (C) 2005-2008 MaNGOS <http://www.mangosproject.org/>
 *
 * Copyright (C) 2008 Trinity <http://www.trinitycore.org/>
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

#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "UpdateData.h"
#include "Item.h"
#include "Map.h"
#include "MapManager.h"
#include "Transports.h"
#include "ObjectAccessor.h"
#include "CellImpl.h"
#include "SpellAuras.h"

using namespace Trinity;

void VisibleNotifier::SendToSelf()
{
    // at this moment i_clientGUIDs have guids that not iterate at grid level checks
    // but exist one case when this possible and object not out of range: transports
    if (Transport* transport = i_player.GetTransport())
        for (Transport::PlayerSet::const_iterator itr = transport->GetPassengers().begin();itr!=transport->GetPassengers().end();++itr)
        {
            if (vis_guids.find((*itr)->GetGUID()) != vis_guids.end())
            {
                vis_guids.erase((*itr)->GetGUID());

                i_player.UpdateVisibilityOf((*itr), i_data, i_visibleNow);

                if (!(*itr)->isNeedNotify(NOTIFY_VISIBILITY_CHANGED))
                    (*itr)->UpdateVisibilityOf(&i_player);
            }
        }

    for (Player::ClientGUIDs::const_iterator it = vis_guids.begin();it != vis_guids.end(); ++it)
    {
        i_player.m_clientGUIDs.erase(*it);
        i_data.AddOutOfRangeGUID(*it);
        if (IS_PLAYER_GUID(*it))
        {
            Player* plr = ObjectAccessor::FindPlayer(*it);
            if (plr && plr->IsInWorld() && !plr->isNeedNotify(NOTIFY_VISIBILITY_CHANGED))
                plr->UpdateVisibilityOf(&i_player);
        }
    }

    if (!i_data.HasData())
        return;

    WorldPacket packet;
    i_data.BuildPacket(&packet);
    i_player.GetSession()->SendPacket(&packet);

    for (std::set<Unit*>::const_iterator it = i_visibleNow.begin(); it != i_visibleNow.end(); ++it)
        i_player.SendInitialVisiblePackets(*it);
}

void VisibleChangesNotifier::Visit(PlayerMapType &m)
{
    for (PlayerMapType::iterator iter=m.begin(); iter != m.end(); ++iter)
    {
        if (iter->getSource() == &i_object)
            continue;

        iter->getSource()->UpdateVisibilityOf(&i_object);

        if (!iter->getSource()->GetSharedVisionList().empty())
            for (SharedVisionList::const_iterator i = iter->getSource()->GetSharedVisionList().begin(); i != iter->getSource()->GetSharedVisionList().end(); ++i)
                if ((*i)->HasFarsightVision() && (*i)->GetFarsightTarget() == iter->getSource())
                    (*i)->UpdateVisibilityOf(&i_object);
    }
}

void VisibleChangesNotifier::Visit(CreatureMapType &m)
{
    for (CreatureMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
        if(!iter->getSource()->GetSharedVisionList().empty())
            for(SharedVisionList::const_iterator i = iter->getSource()->GetSharedVisionList().begin(); i != iter->getSource()->GetSharedVisionList().end(); ++i)
                if ((*i)->HasFarsightVision() && (*i)->GetFarsightTarget() == iter->getSource())
                    (*i)->UpdateVisibilityOf(&i_object);
}

void VisibleChangesNotifier::Visit(DynamicObjectMapType &m)
{
    for (DynamicObjectMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
        if(IS_PLAYER_GUID(iter->getSource()->GetCasterGUID()))
            if(Player* caster = (Player*)iter->getSource()->GetCaster())
                caster->UpdateVisibilityOf(&i_object);
}

inline void CreatureUnitRelocationWorker(Creature* c, Unit* u)
{
    if (!u->isAlive() || !c->isAlive() || c == u)
        return;

    if (u->IsTaxiFlying() && !c->CanReactToPlayerOnTaxi())
        return;

    if (c->HasReactState(REACT_AGGRESSIVE) && !c->hasUnitState(UNIT_STAT_SIGHTLESS))
        if (c->_IsWithinDist(u, DEFAULT_VISIBILITY_DISTANCE, true) && c->IsAIEnabled)
            c->AI()->MoveInLineOfSight_Safe(u);
}

void PlayerRelocationNotifier::Visit(PlayerMapType &m)
{
    for (PlayerMapType::iterator iter=m.begin(); iter != m.end(); ++iter)
    {
        Player* plr = iter->getSource();

        vis_guids.erase(plr->GetGUID());

        i_player.UpdateVisibilityOf(plr,i_data,i_visibleNow);

        WorldObject const* viewPoint = plr->GetFarsightTarget();
        if (!viewPoint || !plr->HasFarsightVision()) viewPoint = plr;

        if (viewPoint->isNeedNotify(NOTIFY_VISIBILITY_CHANGED))
            continue;

        plr->UpdateVisibilityOf(&i_player);
    }
}

void PlayerRelocationNotifier::Visit(CreatureMapType &m)
{
    bool relocated_for_ai = (i_player.GetFarsightTarget() == NULL);
    for (CreatureMapType::iterator iter=m.begin(); iter != m.end(); ++iter)
    {
        Creature * c = iter->getSource();

        vis_guids.erase(c->GetGUID());

        i_player.UpdateVisibilityOf(c,i_data,i_visibleNow);

        if (relocated_for_ai && !c->isNeedNotify(NOTIFY_VISIBILITY_CHANGED))
            CreatureUnitRelocationWorker(c, &i_player);
    }
}

void CreatureRelocationNotifier::Visit(PlayerMapType &m)
{
    for (PlayerMapType::iterator iter=m.begin(); iter != m.end(); ++iter)
    {
        Player * pl = iter->getSource();

        WorldObject const* viewPoint = pl->GetFarsightTarget();
        if (!viewPoint || !pl->HasFarsightVision()) viewPoint = pl;

        if (!viewPoint->isNeedNotify(NOTIFY_VISIBILITY_CHANGED))
            pl->UpdateVisibilityOf(&i_creature);

        CreatureUnitRelocationWorker(&i_creature, pl);
    }
}

void CreatureRelocationNotifier::Visit(CreatureMapType &m)
{
    if (!i_creature.isAlive())
        return;

    for (CreatureMapType::iterator iter=m.begin(); iter != m.end(); ++iter)
    {
        Creature* c = iter->getSource();
        CreatureUnitRelocationWorker(&i_creature, c);

        if (!c->isNeedNotify(NOTIFY_VISIBILITY_CHANGED))
            CreatureUnitRelocationWorker(c, &i_creature);
    }
}

void DelayedUnitRelocation::Visit(CreatureMapType &m)
{
    for (CreatureMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        Creature * unit = iter->getSource();
        if (!unit->isNeedNotify(NOTIFY_VISIBILITY_CHANGED))
            continue;

        CreatureRelocationNotifier relocate(*unit);

        TypeContainerVisitor<CreatureRelocationNotifier, WorldTypeMapContainer > c2world_relocation(relocate);
        TypeContainerVisitor<CreatureRelocationNotifier, GridTypeMapContainer >  c2grid_relocation(relocate);

        cell.Visit(p, c2world_relocation, i_map, *unit, i_radius);
        cell.Visit(p, c2grid_relocation, i_map, *unit, i_radius);
    }
}

void DelayedUnitRelocation::Visit(PlayerMapType &m)
{
    for (PlayerMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        Player * player = iter->getSource();

        WorldObject const* viewPoint = player->GetFarsightTarget();
        if (!viewPoint || !player->HasFarsightVision()) viewPoint = player;

        if (!viewPoint->isNeedNotify(NOTIFY_VISIBILITY_CHANGED))
            continue;

        if (player != viewPoint && !viewPoint->IsPositionValid())
            continue;

        CellPair pair2(Trinity::ComputeCellPair(viewPoint->GetPositionX(), viewPoint->GetPositionY()));
        Cell cell2(pair2);

        PlayerRelocationNotifier relocate(*player);
        TypeContainerVisitor<PlayerRelocationNotifier, WorldTypeMapContainer > c2world_relocation(relocate);
        TypeContainerVisitor<PlayerRelocationNotifier, GridTypeMapContainer >  c2grid_relocation(relocate);

        cell2.Visit(pair2, c2world_relocation, i_map, *viewPoint, i_radius);
        cell2.Visit(pair2, c2grid_relocation, i_map, *viewPoint, i_radius);

        relocate.SendToSelf();
    }
}

void AIRelocationNotifier::Visit(CreatureMapType &m)
{
    for (CreatureMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        Creature *c = iter->getSource();
        CreatureUnitRelocationWorker(c, &i_unit);
        if (isCreature)
            CreatureUnitRelocationWorker((Creature*)&i_unit, c);
    }
}

void DynamicObjectUpdater::VisitHelper(Unit* target)
{
    if (!target->isAlive() || target->IsTaxiFlying())
        return;

    if (target->GetTypeId() == TYPEID_UNIT && ((Creature*)target)->isTotem())
        return;

    if (!i_dynobject.IsWithinDistInMap(target, i_dynobject.GetRadius()))
        return;

    if (!i_dynobject.IsWithinLOSInMap(target))
        return;

    //Check targets for not_selectable unit flag and remove
    if (target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE))
        return;

    // Evade target
    if (target->GetTypeId() == TYPEID_UNIT && ((Creature*)target)->IsInEvadeMode())
        return;

    //Check player targets and remove if in GM mode or GM invisibility (for not self casting case)
    if (target->GetTypeId() == TYPEID_PLAYER && target != i_check && (((Player*)target)->isGameMaster() || ((Player*)target)->GetVisibility() == VISIBILITY_OFF))
        return;

    if (i_dynobject.IsAffecting(target))
        return;

    SpellEntry const *spellInfo = sSpellStore.LookupEntry(i_dynobject.GetSpellId());
    uint32 eff_index  = i_dynobject.GetEffIndex();
    if (spellInfo->EffectImplicitTargetB[eff_index] == TARGET_DEST_DYNOBJ_ALLY
        || spellInfo->EffectImplicitTargetB[eff_index] == TARGET_UNIT_AREA_ALLY_DST)
    {
        if (!i_check->IsFriendlyTo(target))
            return;
    }
    else if (i_check->GetTypeId() == TYPEID_PLAYER)
    {
        if (i_check->IsFriendlyTo(target))
            return;

        i_check->CombatStart(target);
    }
    else
    {
        if (!i_check->IsHostileTo(target))
            return;

        i_check->CombatStart(target);
    }

    // Check target immune to spell or aura
    if (target->IsImmunedToSpell(spellInfo) || target->IsImmunedToSpellEffect(spellInfo->Effect[eff_index], spellInfo->EffectMechanic[eff_index]))
        return;

    if (target->preventApplyPersistentAA(spellInfo, eff_index))
        return;

    // Apply PersistentAreaAura on target
    PersistentAreaAura* Aur = new PersistentAreaAura(spellInfo, eff_index, NULL, target, i_dynobject.GetCaster(), NULL, i_dynobject.GetGUID());

    target->AddAura(Aur);
    i_dynobject.AddAffected(target);
}

void DynamicObjectUpdater::Visit(CreatureMapType &m)
{
    for (CreatureMapType::iterator itr=m.begin(); itr != m.end(); ++itr)
        VisitHelper(itr->getSource());
}

void DynamicObjectUpdater::Visit(PlayerMapType &m)
{
    for (PlayerMapType::iterator itr=m.begin(); itr != m.end(); ++itr)
        VisitHelper(itr->getSource());
}

void Deliverer::Visit(PlayerMapType &m)
{
    for (PlayerMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        if (!i_dist || iter->getSource()->GetDistance(&i_source) <= i_dist)
        {
            // Send packet to all who are sharing the player's vision
            if (!iter->getSource()->GetSharedVisionList().empty())
            {
                SharedVisionList::const_iterator it = iter->getSource()->GetSharedVisionList().begin();
                for (; it != iter->getSource()->GetSharedVisionList().end(); ++it)
                    SendPacket(*it);
            }

            VisitObject(iter->getSource());
        }
    }
}

void Deliverer::Visit(CreatureMapType &m)
{
    for (CreatureMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        if (!i_dist || iter->getSource()->GetDistance(&i_source) <= i_dist)
        {
            // Send packet to all who are sharing the creature's vision
            if (!iter->getSource()->GetSharedVisionList().empty())
            {
                SharedVisionList::const_iterator it = iter->getSource()->GetSharedVisionList().begin();
                for (; it != iter->getSource()->GetSharedVisionList().end(); ++it)
                    SendPacket(*it);
            }
        }
    }
}

void Deliverer::Visit(DynamicObjectMapType &m)
{
    for (DynamicObjectMapType::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        if (IS_PLAYER_GUID(iter->getSource()->GetCasterGUID()))
        {
            // Send packet back to the caster if the caster has vision of dynamic object
            Player* caster = (Player*)iter->getSource()->GetCaster();
            if (caster && caster->GetUInt64Value(PLAYER_FARSIGHT) == iter->getSource()->GetGUID() &&
                (!i_dist || iter->getSource()->GetDistance(&i_source) <= i_dist))
                SendPacket(caster);
        }
    }
}

void Deliverer::SendPacket(Player* plr)
{
    if (!plr)
        return;

    // Don't send the packet to self if not supposed to
    if (!i_toSelf && plr == &i_source)
        return;

    // Don't send the packet to possesor if not supposed to
    if (!i_toPossessor && plr->isPossessing() && plr->GetCharmGUID() == i_source.GetGUID())
        return;

    if (plr_list.find(plr->GetGUID()) == plr_list.end())
    {
        if (WorldSession* session = plr->GetSession())
            session->SendPacket(i_message);
        plr_list.insert(plr->GetGUID());
    }
}

void MessageDeliverer::VisitObject(Player* plr)
{
    SendPacket(plr);
}

void MessageDistDeliverer::VisitObject(Player* plr)
{
    if (!i_ownTeamOnly || (i_source.GetTypeId() == TYPEID_PLAYER && plr->GetTeam() == ((Player&)i_source).GetTeam()))
    {
        SendPacket(plr);
    }
}

template<class T> void
ObjectUpdater::Visit(GridRefManager<T> &m)
{
    for (typename GridRefManager<T>::iterator iter = m.begin(); iter != m.end(); ++iter)
    {
        if (iter->getSource()->IsInWorld())
        {
            WorldObject::UpdateHelper helper(iter->getSource());
            helper.Update(i_timeDiff); 
        }
    }
}

bool CannibalizeObjectCheck::operator()(Corpse* u)
{
    // ignore bones
    if (u->GetType()==CORPSE_BONES)
        return false;

    Player* owner = ObjectAccessor::FindPlayer(u->GetOwnerGUID());

    if (!owner || i_funit->IsFriendlyTo(owner))
        return false;

    if (i_funit->IsWithinDistInMap(u, i_range))
        return true;

    return false;
}

template void ObjectUpdater::Visit<GameObject>(GameObjectMapType &);
template void ObjectUpdater::Visit<DynamicObject>(DynamicObjectMapType &);