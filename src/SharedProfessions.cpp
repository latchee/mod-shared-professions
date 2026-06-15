#include "AccountMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellMgr.h"

#include <unordered_set>
#include <mutex>

namespace
{
    bool enabled = true;
    bool syncSecondary = true;

    // Anti-recursion guard: tracks player GUIDs currently being synced
    std::mutex syncMutex;
    std::unordered_set<uint32> syncingPlayers;

    struct SyncGuard
    {
        uint32 guid;
        bool active;

        SyncGuard(uint32 g) : guid(g), active(false)
        {
            std::lock_guard<std::mutex> lock(syncMutex);
            if (syncingPlayers.find(guid) == syncingPlayers.end())
            {
                syncingPlayers.insert(guid);
                active = true;
            }
        }

        ~SyncGuard()
        {
            if (active)
            {
                std::lock_guard<std::mutex> lock(syncMutex);
                syncingPlayers.erase(guid);
            }
        }

        explicit operator bool() const { return active; }
    };

    bool IsSyncing(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(syncMutex);
        return syncingPlayers.find(guid) != syncingPlayers.end();
    }

    bool IsRandomBot(Player* player)
    {
        if (!player)
            return false;
        uint32 accountId = player->GetSession()->GetAccountId();
        std::string username;
        if (!sAccountMgr->GetName(accountId, username))
            return false;
        if (username.size() < 6)
            return false;
        std::string prefix = username.substr(0, 6);
        for (char& c : prefix)
            c = std::tolower(static_cast<unsigned char>(c));
        return prefix == "rndbot";
    }

    bool IsSyncedProfessionSkill(uint32 skillId)
    {
        if (IsPrimaryProfessionSkill(skillId))
            return true;

        if (syncSecondary)
            return skillId == SKILL_FISHING || skillId == SKILL_COOKING || skillId == SKILL_FIRST_AID;

        return false;
    }

    uint32 GetProfessionSkillForSpell(uint32 spellId)
    {
        auto bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            uint32 skillId = itr->second->SkillLine;
            if (IsSyncedProfessionSkill(skillId))
                return skillId;
        }
        return 0;
    }

    // Returns the passive skill bonus granted by a race-specific profession aura.
    // These modifiers are always active while the player is in the world, so we
    // must strip them before persisting to the account store; otherwise the bonus
    // stacks with itself on every relog.
    //
    //   Gnome     – Engineering Specialization : +15 Engineering  (spell 20574)
    //   Draenei   – Gemcutting                 : +5  Jewelcrafting (spell 28677)
    //   Blood Elf – Arcane Affinity            : +10 Enchanting   (spell 28877)
    //   Tauren    – Cultivation                : +15 Herbalism    (spell 20552)
    uint32 GetRacialProfessionBonus(uint8 race, uint32 skillId)
    {
        switch (race)
        {
            case RACE_GNOME:
                if (skillId == SKILL_ENGINEERING)    return 15;
                break;
            case RACE_DRAENEI:
                if (skillId == SKILL_JEWELCRAFTING)  return 5;
                break;
            case RACE_BLOOD_ELF:
                if (skillId == SKILL_ENCHANTING)     return 10;
                break;
            case RACE_TAUREN:
                if (skillId == SKILL_HERBALISM)      return 15;
                break;
            default:
                break;
        }
        return 0;
    }
}

class SharedProfessionsWorldScript : public WorldScript
{
public:
    SharedProfessionsWorldScript() : WorldScript("SharedProfessionsWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        enabled = sConfigMgr->GetOption<bool>("SharedProfessions.Enable", true);
        syncSecondary = sConfigMgr->GetOption<bool>("SharedProfessions.SyncSecondary", true);
    }
};

class SharedProfessionsPlayerScript : public PlayerScript
{
public:
    SharedProfessionsPlayerScript() : PlayerScript("SharedProfessionsPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LEARN_SPELL,
        PLAYERHOOK_ON_UPDATE_SKILL
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!enabled)
            return;

        if (IsRandomBot(player))
            return;

        uint32 accountId = player->GetSession()->GetAccountId();
        uint32 guid = player->GetGUID().GetCounter();

        SyncGuard guard(guid);
        if (!guard)
            return;

        // Phase 1: Save this character's current profession data to account store
        SaveCharacterProfessions(player, accountId);

        // Phase 2: Apply account-wide best data to this character
        ApplyAccountProfessions(player, accountId);
    }

    void OnPlayerLearnSpell(Player* player, uint32 spellId) override
    {
        if (!enabled)
            return;

        if (IsRandomBot(player))
            return;

        if (IsSyncing(player->GetGUID().GetCounter()))
            return;

        uint32 skillId = GetProfessionSkillForSpell(spellId);
        if (!skillId)
            return;

        uint32 accountId = player->GetSession()->GetAccountId();
        CharacterDatabase.Execute(
            "INSERT IGNORE INTO shared_professions_account_spells (account_id, spell_id) VALUES ({}, {})",
            accountId, spellId);

        // If this spell grants/upgrades a profession skill, sync account data
        SpellLearnSkillNode const* learnSkill = sSpellMgr->GetSpellLearnSkill(spellId);
        if (learnSkill && IsSyncedProfessionSkill(learnSkill->skill))
        {
            SyncGuard guard(player->GetGUID().GetCounter());
            if (guard)
                ApplyAccountProfessions(player, accountId);
        }
    }

    void OnPlayerUpdateSkill(Player* player, uint32 skillId, uint32 /*value*/, uint32 /*max*/, uint32 /*step*/, uint32 newValue) override
    {
        if (!enabled)
            return;

        if (IsRandomBot(player))
            return;

        if (IsSyncing(player->GetGUID().GetCounter()))
            return;

        if (!IsSyncedProfessionSkill(skillId))
            return;

        uint32 accountId = player->GetSession()->GetAccountId();
        uint16 maxVal  = player->GetPureMaxSkillValue(skillId);
        uint16 stepVal = player->GetSkillStep(skillId);

        // Strip the racial passive bonus before persisting — the aura re-adds
        // it automatically on each login, so storing the inflated value would
        // cause the skill to grow by the bonus amount on every relog.
        uint32 bonus       = GetRacialProfessionBonus(player->getRace(), skillId);
        uint16 storedValue = (newValue > bonus) ? static_cast<uint16>(newValue - bonus) : 0;

        CharacterDatabase.Execute(
            "INSERT INTO shared_professions_account_skills (account_id, skill_id, value, max_value, step) "
            "VALUES ({}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE "
            "value = GREATEST(value, VALUES(value)), "
            "max_value = GREATEST(max_value, VALUES(max_value)), "
            "step = GREATEST(step, VALUES(step))",
            accountId, skillId, storedValue, maxVal, stepVal);
    }

private:
    void SaveCharacterProfessions(Player* player, uint32 accountId)
    {
        for (uint32 i = 0; i < sSkillLineStore.GetNumRows(); ++i)
        {
            SkillLineEntry const* skill = sSkillLineStore.LookupEntry(i);
            if (!skill)
                continue;

            if (!IsSyncedProfessionSkill(skill->id))
                continue;

            if (!player->HasSkill(skill->id))
                continue;

            uint16 rawValue = player->GetSkillValue(skill->id);
            uint16 maxVal   = player->GetPureMaxSkillValue(skill->id);
            uint16 step     = player->GetSkillStep(skill->id);

            // Strip the racial passive bonus so only the earned portion is stored.
            // The aura re-applies automatically on each login, so persisting the
            // inflated value would add the bonus again on every relog.
            uint32 bonus = GetRacialProfessionBonus(player->getRace(), skill->id);
            uint16 value = (rawValue > static_cast<uint16>(bonus)) ? static_cast<uint16>(rawValue - bonus) : 0;

            // UPSERT skill with GREATEST to keep the best values
            CharacterDatabase.Execute(
                "INSERT INTO shared_professions_account_skills (account_id, skill_id, value, max_value, step) "
                "VALUES ({}, {}, {}, {}, {}) "
                "ON DUPLICATE KEY UPDATE "
                "value = GREATEST(value, VALUES(value)), "
                "max_value = GREATEST(max_value, VALUES(max_value)), "
                "step = GREATEST(step, VALUES(step))",
                accountId, skill->id, value, maxVal, step);

            // Save all known recipe spells for this profession
            SaveCharacterRecipes(player, accountId, skill->id);
        }
    }

    void SaveCharacterRecipes(Player* player, uint32 accountId, uint32 skillId)
    {
        // Iterate all skill line abilities to find spells belonging to this skill
        for (uint32 j = 0; j < sSkillLineAbilityStore.GetNumRows(); ++j)
        {
            SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(j);
            if (!ability)
                continue;

            if (ability->SkillLine != skillId)
                continue;

            if (player->HasSpell(ability->Spell))
            {
                CharacterDatabase.Execute(
                    "INSERT IGNORE INTO shared_professions_account_spells (account_id, spell_id) VALUES ({}, {})",
                    accountId, ability->Spell);
            }
        }
    }

    void ApplyAccountProfessions(Player* player, uint32 accountId)
    {
        // Apply skill levels
        QueryResult skillResult = CharacterDatabase.Query(
            "SELECT skill_id, value, max_value, step FROM shared_professions_account_skills WHERE account_id = {}",
            accountId);

        if (skillResult)
        {
            do
            {
                Field* fields = skillResult->Fetch();
                uint16 skillId = fields[0].Get<uint16>();
                uint16 acctValue = fields[1].Get<uint16>();
                uint16 acctMax = fields[2].Get<uint16>();
                uint16 acctStep = fields[3].Get<uint16>();

                if (!player->HasSkill(skillId))
                    continue;

                uint16 playerStep = player->GetSkillStep(skillId);
                uint16 playerMax  = player->GetPureMaxSkillValue(skillId);

                // GetSkillValue includes any racial passive bonus.
                // acctValue is stored without that bonus, so strip it from the
                // live value before comparing to keep the comparison fair.
                uint16 playerRaw    = player->GetSkillValue(skillId);
                uint32 racialBonus  = GetRacialProfessionBonus(player->getRace(), skillId);
                uint16 playerEarned = (playerRaw > static_cast<uint16>(racialBonus))
                                      ? static_cast<uint16>(playerRaw - racialBonus)
                                      : 0;

                if (acctStep > playerStep)
                {
                    // Upgrade tier — this also sets value and max
                    player->SetSkill(skillId, acctStep, acctValue, acctMax);
                }
                else if (acctValue > playerEarned)
                {
                    // Same or lower tier, but higher earned value
                    uint16 newVal = std::min(acctValue, playerMax);
                    player->SetSkill(skillId, playerStep, newVal, playerMax);
                }
            } while (skillResult->NextRow());
        }

        // Apply recipes
        QueryResult spellResult = CharacterDatabase.Query(
            "SELECT spell_id FROM shared_professions_account_spells WHERE account_id = {}",
            accountId);

        if (spellResult)
        {
            do
            {
                Field* fields = spellResult->Fetch();
                uint32 spellId = fields[0].Get<uint32>();

                if (player->HasSpell(spellId))
                    continue;

                // Only teach if player has the associated profession
                uint32 skillId = GetProfessionSkillForSpell(spellId);
                if (skillId && player->HasSkill(skillId))
                    player->learnSpell(spellId);
            } while (spellResult->NextRow());
        }
    }
};

void AddSC_SharedProfessions()
{
    new SharedProfessionsWorldScript();
    new SharedProfessionsPlayerScript();
}