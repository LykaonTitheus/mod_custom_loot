#include "ScriptMgr.h"
#include "Player.h"
#include "GameObject.h"
#include "ScriptedGossip.h"
#include "Creature.h"
#include "Chat.h"
#include "Spell.h"
#include "LootMgr.h"
#include "Group.h"
#include <unordered_map>
#include <set>

#define ITEM_LOOT_TOKEN 59000
#define GAMEOBJECT_LOOT_CHEST 181061 

class mod_boss_reward : public PlayerScript
{
public:
    mod_boss_reward() : PlayerScript("mod_boss_reward") {}

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!killer || !killed)
            return;

        uint32 bossEntry = killed->GetEntry();

        // REFINED BOSS DETECTION LOGIC
        bool isBoss = false;

        // 1. Check if it's a World Boss (like Kazzak)
        if (killed->isWorldBoss())
            isBoss = true;
        // 2. Check if the Core identifies it as a Dungeon Boss
        else if (killed->IsDungeonBoss())
            isBoss = true;
        // 3. If in a Raid, only accept Rank 3 (Boss) or higher
        else if (killed->GetMap()->IsRaid() && killed->GetCreatureTemplate()->rank >= 3)
            isBoss = true;
        // 4. Manual Overrides (Specific bosses that might not be flagged correctly)
        else if (bossEntry == 10440) // Baron Rivendare
            isBoss = true;

        if (isBoss)
        {
            if (Group* group = killer->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (member && member->IsAtGroupRewardDistance(killed))
                        RewardPlayer(member, bossEntry, killed->GetName());
                }
            }
            else
                RewardPlayer(killer, bossEntry, killed->GetName());
        }
    }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        // Delete save boss kills from teh database dor deleted characters
        CharacterDatabase.Execute("DELETE FROM character_boss_tokens WHERE player_guid = {}", guid.GetCounter());
    }

private:
    void RewardPlayer(Player* player, uint32 bossEntry, std::string bossName)
    {
        // SQL: Save kill
        CharacterDatabase.Execute("INSERT INTO character_boss_tokens (player_guid, boss_id) VALUES ({}, {})",
            player->GetGUID().GetCounter(), bossEntry);

        uint32 itemId = ITEM_LOOT_TOKEN;
        uint32 count = 1;
        ItemPosCountVec dest;
        uint32 noSpaceForCount = 0;

        InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count, &noSpaceForCount);

        if (msg == EQUIP_ERR_OK)
        {
            player->AddItem(itemId, count);
            std::string message = "A mighty creature has fallen: " + bossName + "! Token received.";
            ChatHandler(player->GetSession()).SendSysMessage(message.c_str());
        }
        else
        {
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            MailDraft draft("Boss-Loot-Token: " + bossName, "I could not fit in your pockets. This is for defeating " + bossName + ".");
            if (Item* item = Item::CreateItem(itemId, count, player))
            {
                item->SaveToDB(trans);
                draft.AddItem(item);
                draft.SendMailTo(trans, MailReceiver(player), MailSender(MAIL_CREATURE, 0));
                CharacterDatabase.CommitTransaction(trans);
                player->GetSession()->SendAreaTriggerMessage("Your inventory is full. The token will find its way to you.");
            }
        }
    }
};

class mod_loot_item : public ItemScript
{
public:
    mod_loot_item() : ItemScript("mod_loot_item") {}

    bool OnUse(Player* player, Item* /*item*/, SpellCastTargets const& /*targets*/) override
    {
        // Check if the player is currently in combat
        if (player->IsInCombat())
        {
            player->GetSession()->SendAreaTriggerMessage("You cannot use this token while in combat!");
            return true; // Prevents the item from being used/consumed
        }

        // SPAM-guard: Check if there is a chest already within 15 m
        bool hasOwnChestNearby = false;
        std::list<GameObject*> chestList;
        player->GetGameObjectListWithEntryInGrid(chestList, GAMEOBJECT_LOOT_CHEST, 15.0f);

        for (GameObject* chest : chestList)
        {
            if (chest->GetOwnerGUID() == player->GetGUID())
            {
                hasOwnChestNearby = true;
                break;
            }
        }

        if (hasOwnChestNearby)
        {
            player->GetSession()->SendAreaTriggerMessage("You already have a chest nearby. Wait until it vanishes!");
            return true;
        }

        // SQL: Check for a kill in list (oldest first)
        QueryResult result = CharacterDatabase.Query("SELECT boss_id FROM character_boss_tokens WHERE player_guid = {} ORDER BY kill_time ASC LIMIT 1",
            player->GetGUID().GetCounter());

        if (!result)
        {
            player->GetSession()->SendAreaTriggerMessage("You need to defeat a mighty foe to use this token.");
            return true;
        }

        float x, y, z, o;
        player->GetPosition(x, y, z, o);
        float spawnX, spawnY, spawnZ;
        player->GetClosePoint(spawnX, spawnY, spawnZ, player->GetObjectSize(), 2.0f);

        // 1. Spawn chest
        if (GameObject* chest = player->SummonGameObject(GAMEOBJECT_LOOT_CHEST, spawnX, spawnY, spawnZ, o, 0, 0, 0, 0, 60))
        {
            chest->SetOwnerGUID(player->GetGUID());
            ObjectGuid chestGuid = chest->GetGUID(); // remember the GUID, not the pointer!

            // 2. Spawn NPC
            if (Creature* trigger = player->SummonCreature(13280, spawnX, spawnY, spawnZ, o, TEMPSUMMON_TIMED_OR_CORPSE_DESPAWN, 60000))
            {
                trigger->SetDisplayId(11686);
                trigger->SetReactState(REACT_PASSIVE);
                trigger->SetFaction(35);
                trigger->SetPhaseMask(player->GetPhaseMask(), true);
                trigger->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_NON_ATTACKABLE);

                trigger->AddAura(70571, trigger);
                trigger->CastSpell(trigger, 70571, true);

                // --- CHECK-LOOP ---
                // Create an event that restarts every 1 second
                struct ChestCheckEvent : public BasicEvent
                {
                    Creature* _trigger;
                    ObjectGuid _chestGuid;
                    ChestCheckEvent(Creature* t, ObjectGuid g) : _trigger(t), _chestGuid(g) {}

                    bool Execute(uint64 /*e_time*/, uint32 /*p_time*/) override
                    {
                        // If NPC is gone, stop event
                        if (!_trigger->IsInWorld())
                            return true;

                        // Search for the chest in the world via GUID
                        GameObject* chest = _trigger->GetMap()->GetGameObject(_chestGuid);

                        // If chest is gone OR in "opened/empty" state (GO_STATE_ACTIVE)
                        if (!chest || !chest->IsInWorld() || chest->GetGoState() != GO_STATE_READY)
                        {
                            _trigger->DespawnOrUnsummon();
                            return true; // Event stoppen
                        }

                        // Chest still there? Check again in 1 second.
                        _trigger->m_Events.AddEvent(this, _trigger->m_Events.CalculateTime(1000));
                        return false;
                    }
                };

                trigger->m_Events.AddEvent(new ChestCheckEvent(trigger, chestGuid), trigger->m_Events.CalculateTime(1000));
            }

            chest->PlayDistanceSound(6076);
            player->GetSession()->SendAreaTriggerMessage("|cff00ff00Your reward awaits!|r");

            // --- Manual cooldown ---
            // 60000 = 60 seconds cooldown, preventing chest spam.
            // 59000 is the token ID.
            player->AddSpellCooldown(59000, 0, 60000);
        }

        // Return true prevents deeltion of the item.
        // If your core still deletes it, set item-flag "no_consume" in the database.
        return true;
    }
};

class mod_loot_chest : public GameObjectScript
{
public:
    mod_loot_chest() : GameObjectScript("mod_loot_chest") {}

    void ShowSubMenu(Player* player, GameObject* go, bool showEpics)
    {
        QueryResult result = CharacterDatabase.Query("SELECT boss_id FROM character_boss_tokens WHERE player_guid = {} ORDER BY kill_time ASC LIMIT 1",
            player->GetGUID().GetCounter());

        if (!result)
            return;

        uint32 bossEntry = (*result)[0].Get<uint32>();
        uint32 targetQuality = showEpics ? ITEM_QUALITY_EPIC : ITEM_QUALITY_RARE;

        ClearGossipMenuFor(player);
        std::set<uint32> uniqueItems;

        for (int i = 0; i < 100; ++i)
        {
            Loot loot;
            loot.FillLoot(bossEntry, LootTemplates_Creature, player, false, false);

            for (auto& item : loot.items)
            {
                if (const ItemTemplate* temp = sObjectMgr->GetItemTemplate(item.itemid))
                {
                    bool isLegendary = (temp->Quality == ITEM_QUALITY_LEGENDARY);
                    if ((temp->Quality == targetQuality || (showEpics && isLegendary)) && uniqueItems.find(item.itemid) == uniqueItems.end())
                    {
                        if (uniqueItems.size() >= 30)
                            break;
                        uniqueItems.insert(item.itemid);
                    }
                }
            }
            if (uniqueItems.size() >= 30)
                break;
        }

        // --- Sort item list alphabetically ---
        struct SortEntry {
            uint32 id;
            std::string name;
            uint32 quality;
        };
        std::vector<SortEntry> sortedList;

        for (uint32 itemId : uniqueItems)
        {
            if (const ItemTemplate* temp = sObjectMgr->GetItemTemplate(itemId))
                sortedList.push_back({ itemId, temp->Name1, temp->Quality });
        }

        // Sort by name
        std::sort(sortedList.begin(), sortedList.end(), [](const SortEntry& a, const SortEntry& b) {
            return a.name < b.name;
            });

        // Add sorted items to gossip menu
        for (const auto& entry : sortedList)
        {
            bool isLegendary = (entry.quality == ITEM_QUALITY_LEGENDARY);
            uint32 icon = isLegendary ? GOSSIP_ICON_BATTLE : (showEpics ? GOSSIP_ICON_TABARD : GOSSIP_ICON_MONEY_BAG);
            AddGossipItemFor(player, icon, entry.name, GOSSIP_SENDER_MAIN, 2000000 + entry.id);
        }

        if (uniqueItems.empty())
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, ".. No items of this quality found ..", GOSSIP_SENDER_MAIN, 999);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "[ Back ]", GOSSIP_SENDER_MAIN, 999);
        SendGossipMenuFor(player, 1, go->GetGUID());
    }

    void ShowPreviewMenu(Player* player, GameObject* go, uint32 itemId)
    {
        const ItemTemplate* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto) return;

        ClearGossipMenuFor(player);

        // Color logic
        std::string color = (proto->Quality == ITEM_QUALITY_EPIC) ? "ffa335ee" : "ff0070dd";
        std::string itemLink = "|c" + color + "|Hitem:" + std::to_string(itemId) +
            ":0:0:0:0:0:0:0:0|h[" + proto->Name1 + "]|h|r";

        // Send the clickable link to chat
        std::string fullMsg = "Preview of: " + itemLink;
        ChatHandler(player->GetSession()).SendSysMessage(fullMsg.c_str());

        // Gossip options
        // Option A: Confirm (this sends the plain itemId)
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "CONFIRM: " + proto->Name1, GOSSIP_SENDER_MAIN, itemId);

        // Option B: Back (this sends 999 to return to Quality selection OR you can send 1000/1001)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, " <-- back to list", GOSSIP_SENDER_MAIN, 999);

        SendGossipMenuFor(player, 1, go->GetGUID());
    }

    bool OnGossipHello(Player* player, GameObject* go) override
    {
        if (go->GetOwnerGUID() != player->GetGUID())
        {
            player->GetSession()->SendAreaTriggerMessage("This chest is not yours!");
            return true;
        }

        // SQL query instead of map-check
        QueryResult result = CharacterDatabase.Query("SELECT boss_id FROM character_boss_tokens WHERE player_guid = {} ORDER BY kill_time ASC LIMIT 1",
            player->GetGUID().GetCounter());

        if (!result)
            return true;

        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_TABARD, "Show epic rewards", GOSSIP_SENDER_MAIN, 1000);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Show rare rewards", GOSSIP_SENDER_MAIN, 1001);
        SendGossipMenuFor(player, 1, go->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, GameObject* go, uint32 /*sender*/, uint32 action) override
    {
        if (action == 1000) { ShowSubMenu(player, go, true); return true; }
        if (action == 1001) { ShowSubMenu(player, go, false); return true; }
        if (action == 999) { OnGossipHello(player, go); return true; }

        // Preview logic (Action > 2,000,000)
        if (action > 2000000)
        {
            ShowPreviewMenu(player, go, action - 2000000);
            return true;
        }

        // FINAL CONFIRMATION LOGIC
        uint32 itemId = action;

        // 1. Inventory check
        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, 1);

        if (msg != EQUIP_ERR_OK)
        {
            player->SendEquipError(msg, nullptr); // Shows red "Inventory Full"

            // CRITICAL: Re-show the preview menu so the buttons stay active!
            ShowPreviewMenu(player, go, itemId);
            return true;
        }

        // 2. Add item
        if (player->AddItem(itemId, 1))
        {
            // SAFETY-CHECK: Does the player still have the token?
            if (player->HasItemCount(ITEM_LOOT_TOKEN, 1))
            {
                player->DestroyItemCount(ITEM_LOOT_TOKEN, 1, true);
            }
            else
            {
                // Optional: Error message if token is missing
                player->GetSession()->SendAreaTriggerMessage("Token missing!");
                return true;
            }

            QueryResult result = CharacterDatabase.Query("SELECT boss_id FROM character_boss_tokens WHERE player_guid = {} ORDER BY kill_time ASC LIMIT 1",
                player->GetGUID().GetCounter());

            if (result)
            {
                uint32 bossEntry = (*result)[0].Get<uint32>();
                CharacterDatabase.Execute("DELETE FROM character_boss_tokens WHERE player_guid = {} AND boss_id = {} ORDER BY kill_time ASC LIMIT 1",
                    player->GetGUID().GetCounter(), bossEntry);
            }

            go->Delete();
            CloseGossipMenuFor(player);
            player->GetSession()->SendAreaTriggerMessage("Reward claimed!");
        }

        return true;
    }
};

void Addmod_custom_lootScripts()
{
    new mod_boss_reward();
    new mod_loot_item();
    new mod_loot_chest();
}
