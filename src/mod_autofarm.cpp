/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 *
 * Server-side material farming coordinator for mod-playerbots.
 */

#include "Chat.h"
#include "AttackAction.h"
#include "CommandScript.h"
#include "Config.h"
#include "Corpse.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Event.h"
#include "GameObject.h"
#include "Log.h"
#include "LootMgr.h"
#include "LootObjectStack.h"
#include "LootValues.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MovementActions.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr uint8 SOURCE_GAMEOBJECT = 0x01;
    constexpr uint8 SOURCE_CREATURE_LOOT = 0x02;
    constexpr uint8 SOURCE_CREATURE_SKIN = 0x04;
    constexpr float AUTOFARM_INTERACTION_DISTANCE = INTERACTION_DISTANCE - 2.0f;
    constexpr float AUTOFARM_PROGRESS_DISTANCE = 1.0f;
    constexpr float AUTOFARM_FLIGHT_APPROACH_DISTANCE = 28.0f;
    constexpr float AUTOFARM_FLIGHT_LANDING_HEIGHT = 3.0f;
    constexpr uint32 AUTOFARM_STUCK_TIMEOUT_MS = 8 * IN_MILLISECONDS;
    constexpr uint8 AUTOFARM_RETRIES_BEFORE_TELEPORT = 2;

    enum class FlightStage : uint8
    {
        None,
        Takeoff,
        Cruise,
        Landing,
        Evading
    };

    struct AutofarmConfig
    {
        bool enabled = true;
        bool teleportToRoute = true;
        bool returnOnStop = true;
        bool useFlyingMounts = true;
        bool debug = false;
        float flightHeight = 35.0f;
        float clusterSize = 1800.0f;
        float clusterRadius = 2800.0f;
        uint32 maxRoutePoints = 300;
        uint32 maxCreatureLevelsAboveBot = 3;
        uint32 updateIntervalMs = 1000;
        uint32 interactionTimeoutMs = 15000;
        uint32 routePointTimeoutMs = 120000;
        uint32 flightCombatEscapeMs = 6000;
        std::unordered_set<uint32> allowedMaps = {0, 1, 530, 571};
    };

    struct SourceSpawn
    {
        ObjectGuid::LowType spawnId = 0;
        uint32 entry = 0;
        uint32 mapId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float orientation = 0.0f;
        uint8 sourceMask = 0;
    };

    class AutofarmDestination final : public TravelDestination
    {
    public:
        AutofarmDestination(WorldPosition* position, std::string title)
            : TravelDestination({position}, 7.0f, 60.0f), _title(std::move(title))
        {
            setExpireDelay(HOUR * IN_MILLISECONDS);
            setCooldownDelay(1);
        }

        bool isActive(Player* /*bot*/) override { return true; }
        std::string const getName() override { return "AutofarmTravelDestination"; }
        std::string const getTitle() override { return _title; }

    private:
        std::string _title;
    };

    class AutofarmAttackAction final : public AttackAction
    {
    public:
        explicit AutofarmAttackAction(PlayerbotAI* botAI) : AttackAction(botAI, "autofarm attack") { }

        bool AttackTarget(Unit* target) { return Attack(target); }
    };

    class AutofarmMovementAction final : public MovementAction
    {
    public:
        explicit AutofarmMovementAction(PlayerbotAI* botAI) : MovementAction(botAI, "autofarm movement") { }

        bool MoveToTarget(WorldObject* target)
        {
            return MoveNear(target, 0.5f, MovementPriority::MOVEMENT_FORCED);
        }

        bool MoveToPoint(SourceSpawn const& source)
        {
            return MoveTo(source.mapId, source.x, source.y, source.z, false, false, false, true,
                MovementPriority::MOVEMENT_FORCED, true);
        }

        bool FlyTo(uint32 mapId, float x, float y, float z)
        {
            return MoveTo(mapId, x, y, z, false, false, false, true,
                MovementPriority::MOVEMENT_FORCED, true);
        }
    };

    struct RoutePoint
    {
        SourceSpawn source;
        std::unique_ptr<WorldPosition> position;
        std::unique_ptr<AutofarmDestination> destination;
    };

    struct StrategyState
    {
        std::string name;
        BotState state = BOT_STATE_NON_COMBAT;
        bool wasEnabled = false;
    };

    struct FarmSession
    {
        ObjectGuid botGuid;
        ObjectGuid ownerGuid;
        uint32 itemId = 0;
        std::string itemName;
        uint32 goalAmount = 0;
        uint32 startingCount = 0;
        uint32 startedAtMs = 0;
        uint32 pointStartedAtMs = 0;
        uint32 interactionStartedAtMs = 0;
        uint32 completedLoops = 0;
        size_t routeIndex = 0;
        float lastRouteDistance = std::numeric_limits<float>::max();
        uint32 lastRouteProgressAtMs = 0;
        uint8 stuckRecoveryAttempts = 0;
        bool waitingForInitialTeleport = false;
        bool itemWasAlwaysLooted = false;
        bool recoveringFromDeath = false;
        bool combatStrategiesSuppressed = false;
        FlightStage flightStage = FlightStage::None;
        float flightCruiseZ = std::numeric_limits<float>::lowest();
        uint32 flightCombatStartedAtMs = 0;
        WorldLocation returnLocation;
        std::vector<StrategyState> strategies;
        std::vector<std::string> combatStrategies;
        std::vector<RoutePoint> route;
    };

    struct ClusterKey
    {
        uint32 mapId = 0;
        int32 x = 0;
        int32 y = 0;

        bool operator==(ClusterKey const& other) const
        {
            return mapId == other.mapId && x == other.x && y == other.y;
        }
    };

    struct ClusterKeyHash
    {
        size_t operator()(ClusterKey const& key) const
        {
            size_t seed = std::hash<uint32>{}(key.mapId);
            seed ^= std::hash<int32>{}(key.x) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= std::hash<int32>{}(key.y) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    std::string Trim(std::string_view text)
    {
        size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return {};

        size_t last = text.find_last_not_of(" \t\r\n");
        return std::string(text.substr(first, last - first + 1));
    }

    std::string ToLower(std::string_view text)
    {
        std::string result(text);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        return result;
    }

    std::optional<uint32> ParseUInt(std::string_view text)
    {
        uint32 value = 0;
        auto const result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size())
            return std::nullopt;

        return value;
    }

    std::optional<uint32> ExtractItemId(std::string_view text)
    {
        size_t link = text.find("Hitem:");
        if (link != std::string_view::npos)
        {
            size_t first = link + 6;
            size_t last = first;
            while (last < text.size() && std::isdigit(static_cast<unsigned char>(text[last])))
                ++last;

            if (last > first)
                return ParseUInt(text.substr(first, last - first));
        }

        std::string trimmed = Trim(text);
        if (!trimmed.empty())
            return ParseUInt(trimmed);

        return std::nullopt;
    }

    bool ExtractCountOption(std::string& itemText, uint32& count)
    {
        std::string lower = ToLower(itemText);
        size_t option = lower.rfind(" --count ");
        if (option == std::string::npos)
            return true;

        std::string countText = Trim(std::string_view(itemText).substr(option + 9));
        std::optional<uint32> parsed = ParseUInt(countText);
        if (!parsed)
            return false;

        count = *parsed;
        itemText = Trim(std::string_view(itemText).substr(0, option));
        return !itemText.empty();
    }

    std::string SourceDescription(uint8 sourceMask)
    {
        std::vector<std::string> names;
        if (sourceMask & SOURCE_GAMEOBJECT)
            names.emplace_back("gameobjects");
        if (sourceMask & SOURCE_CREATURE_LOOT)
            names.emplace_back("creature loot");
        if (sourceMask & SOURCE_CREATURE_SKIN)
            names.emplace_back("skinning");

        std::ostringstream output;
        for (size_t index = 0; index < names.size(); ++index)
        {
            if (index)
                output << ", ";
            output << names[index];
        }
        return output.str();
    }

    bool LootTemplateContainsItem(LootTemplateAccess const* lootTemplate, uint32 itemId,
        std::unordered_set<uint32>& visitedReferences, uint8 depth = 0)
    {
        if (!lootTemplate || depth > 12)
            return false;

        for (LootStoreItem const* lootItem : lootTemplate->Entries)
        {
            if (!lootItem || lootItem->needs_quest)
                continue;

            if (lootItem->itemid == itemId)
                return true;

            if (!lootItem->reference)
                continue;

            uint32 referenceId = static_cast<uint32>(std::abs(lootItem->reference));
            if (!visitedReferences.insert(referenceId).second)
                continue;

            LootTemplate const* referenced = LootTemplates_Reference.GetLootFor(referenceId);
            LootTemplateAccess const* accessible = reinterpret_cast<LootTemplateAccess const*>(referenced);
            if (LootTemplateContainsItem(accessible, itemId, visitedReferences, depth + 1))
                return true;
        }

        return false;
    }

    bool LootTemplateContainsItem(LootTemplateAccess const* lootTemplate, uint32 itemId)
    {
        std::unordered_set<uint32> visitedReferences;
        return LootTemplateContainsItem(lootTemplate, itemId, visitedReferences);
    }

    bool MapAllowed(AutofarmConfig const& config, uint32 mapId)
    {
        if (!config.allowedMaps.contains(mapId))
            return false;

        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
        return mapEntry && !mapEntry->Instanceable();
    }

    bool CanUseGameObject(Player const* bot, GameObjectTemplate const* gameObjectTemplate)
    {
        if (!bot || !gameObjectTemplate)
            return false;

        uint32 lockId = gameObjectTemplate->GetLockId();
        if (!lockId)
            return true;

        LockEntry const* lock = sLockStore.LookupEntry(lockId);
        if (!lock)
            return true;

        bool hasSkillRequirement = false;
        for (uint8 index = 0; index < 8; ++index)
        {
            if (lock->Type[index] != LOCK_KEY_SKILL)
                continue;

            SkillType skill = SkillByLockType(LockType(lock->Index[index]));
            if (skill == SKILL_NONE)
                continue;

            hasSkillRequirement = true;
            if (bot->HasSkill(skill) && bot->GetSkillValue(skill) >= std::max<uint32>(1, lock->Skill[index]))
                return true;
        }

        return !hasSkillRequirement;
    }

    bool CanSkinCreature(Player const* bot, CreatureTemplate const* creatureTemplate)
    {
        if (!bot || !creatureTemplate || !creatureTemplate->SkinLootId)
            return false;

        SkillType skill = creatureTemplate->GetRequiredLootSkill();
        if (skill == SKILL_NONE || !bot->HasSkill(skill))
            return false;

        uint32 level = creatureTemplate->maxlevel;
        uint32 requiredSkill = level < 10 ? 1 : level < 20 ? (level - 10) * 10 : level * 5;
        return bot->GetSkillValue(skill) >= requiredSkill;
    }

    double SquaredDistance(float firstX, float firstY, float secondX, float secondY)
    {
        double x = static_cast<double>(firstX) - secondX;
        double y = static_cast<double>(firstY) - secondY;
        return x * x + y * y;
    }

    double FactionAnchorDistance(Player const* bot, uint32 mapId, float x, float y)
    {
        struct Anchor
        {
            uint32 mapId;
            float x;
            float y;
        };

        static std::vector<Anchor> const allianceAnchors =
        {
            {0, -8833.0f, 628.0f},
            {1, 9951.0f, 2280.0f},
            {1, -3965.0f, -11653.0f}
        };
        static std::vector<Anchor> const hordeAnchors =
        {
            {0, 1586.0f, 239.0f},
            {0, 9484.0f, -7279.0f},
            {1, 1500.0f, -4415.0f}
        };

        std::vector<Anchor> const& anchors = bot->GetTeamId() == TEAM_ALLIANCE ? allianceAnchors : hordeAnchors;
        double best = std::numeric_limits<double>::max();
        for (Anchor const& anchor : anchors)
        {
            if (anchor.mapId != mapId)
                continue;

            best = std::min(best, std::sqrt(SquaredDistance(x, y, anchor.x, anchor.y)));
        }

        return best == std::numeric_limits<double>::max() ? 0.0 : best;
    }

    ItemTemplate const* ResolveItem(std::string_view query, std::vector<ItemTemplate const*>& matches)
    {
        if (std::optional<uint32> itemId = ExtractItemId(query))
            return sObjectMgr->GetItemTemplate(*itemId);

        std::string needle = ToLower(Trim(query));
        if (needle.empty())
            return nullptr;

        ItemTemplate const* exact = nullptr;
        for (auto const& [entry, itemTemplate] : *sObjectMgr->GetItemTemplateStore())
        {
            (void)entry;
            std::string name = ToLower(itemTemplate.Name1);
            if (name == needle)
                exact = &itemTemplate;
            if (name.find(needle) != std::string::npos && matches.size() < 20)
                matches.push_back(&itemTemplate);
        }

        if (exact)
            return exact;
        if (matches.size() == 1)
            return matches.front();
        return nullptr;
    }

    class AutofarmMgr
    {
    public:
        static AutofarmMgr& Instance()
        {
            static AutofarmMgr instance;
            return instance;
        }

        void LoadConfig()
        {
            _config.enabled = sConfigMgr->GetOption<bool>("Autofarm.Enable", true);
            _config.teleportToRoute = sConfigMgr->GetOption<bool>("Autofarm.TeleportToRoute", true);
            _config.returnOnStop = sConfigMgr->GetOption<bool>("Autofarm.ReturnOnStop", true);
            _config.useFlyingMounts = sConfigMgr->GetOption<bool>("Autofarm.UseFlyingMounts", true);
            _config.debug = sConfigMgr->GetOption<bool>("Autofarm.Debug", false);
            _config.flightHeight = std::clamp(
                sConfigMgr->GetOption<float>("Autofarm.FlightHeight", 35.0f), 15.0f, 100.0f);
            _config.clusterSize = std::max(200.0f,
                sConfigMgr->GetOption<float>("Autofarm.ClusterSize", 1800.0f));
            _config.clusterRadius = std::max(_config.clusterSize,
                sConfigMgr->GetOption<float>("Autofarm.ClusterRadius", 2800.0f));
            _config.maxRoutePoints = std::max<uint32>(1,
                sConfigMgr->GetOption<uint32>("Autofarm.MaxRoutePoints", 300));
            _config.maxCreatureLevelsAboveBot =
                sConfigMgr->GetOption<uint32>("Autofarm.MaxCreatureLevelsAboveBot", 3);
            _config.updateIntervalMs = std::max<uint32>(250,
                sConfigMgr->GetOption<uint32>("Autofarm.UpdateIntervalMs", 1000));
            _config.interactionTimeoutMs = std::max<uint32>(3000,
                sConfigMgr->GetOption<uint32>("Autofarm.InteractionTimeoutMs", 15000));
            _config.routePointTimeoutMs = std::max<uint32>(10000,
                sConfigMgr->GetOption<uint32>("Autofarm.RoutePointTimeoutMs", 120000));
            _config.flightCombatEscapeMs = std::max<uint32>(2000,
                sConfigMgr->GetOption<uint32>("Autofarm.FlightCombatEscapeMs", 6000));

            _config.allowedMaps.clear();
            std::string allowedMaps = sConfigMgr->GetOption<std::string>("Autofarm.AllowedMaps", "0,1,530,571");
            std::replace(allowedMaps.begin(), allowedMaps.end(), ',', ' ');
            std::istringstream mapStream(allowedMaps);
            std::string mapText;
            while (mapStream >> mapText)
            {
                if (std::optional<uint32> mapId = ParseUInt(mapText))
                    _config.allowedMaps.insert(*mapId);
            }
            if (_config.allowedMaps.empty())
                _config.allowedMaps = {0, 1, 530, 571};

            LOG_INFO("module.autofarm", "mod-autofarm {} with {} allowed outdoor maps",
                _config.enabled ? "enabled" : "disabled", _config.allowedMaps.size());
        }

        bool Start(ChatHandler* handler, Player* bot, std::string itemText)
        {
            if (!_config.enabled)
            {
                handler->SendErrorMessage("Autofarm is disabled in the worldserver configuration.");
                return false;
            }

            PlayerbotAI* botAI = ValidateBot(handler, bot);
            if (!botAI)
                return false;

            if (_sessions.contains(bot->GetGUID()))
            {
                handler->SendErrorMessage("That playerbot already has an autofarm session. Stop it first.");
                return false;
            }

            if (!ValidateStartState(handler, bot))
                return false;

            uint32 goalAmount = 0;
            if (!ExtractCountOption(itemText, goalAmount))
            {
                handler->SendErrorMessage("Invalid quantity. Use: --count <non-negative number>.");
                return false;
            }

            std::vector<ItemTemplate const*> matches;
            ItemTemplate const* itemTemplate = ResolveItem(itemText, matches);
            if (!itemTemplate)
            {
                if (matches.empty())
                {
                    handler->SendErrorMessage(
                        "No item matched '{}'. Use an exact name, item ID, or item link.", itemText);
                }
                else
                {
                    handler->SendErrorMessage("'{}' is ambiguous. Matching items include:", itemText);
                    SendItemMatches(handler, matches, 10);
                }
                return false;
            }

            std::vector<SourceSpawn> sources = FindSources(bot, itemTemplate->ItemId);
            if (sources.empty())
            {
                handler->SendErrorMessage(
                    "No usable outdoor source was found for [{}] ({}). The item may be crafted, sold, open-water "
                    "fished, container-only, profession-locked, or above this bot's safe creature level.",
                    itemTemplate->Name1, itemTemplate->ItemId);
                return false;
            }

            std::vector<SourceSpawn> selectedSources = SelectCluster(bot, sources);
            if (selectedSources.empty())
            {
                handler->SendErrorMessage(
                    "No productive source cluster could be selected for [{}].", itemTemplate->Name1);
                return false;
            }

            auto session = std::make_unique<FarmSession>();
            session->botGuid = bot->GetGUID();
            session->ownerGuid = handler->GetPlayer()->GetGUID();
            session->itemId = itemTemplate->ItemId;
            session->itemName = itemTemplate->Name1;
            session->goalAmount = goalAmount;
            session->startingCount = bot->GetItemCount(itemTemplate->ItemId);
            session->startedAtMs = getMSTime();
            session->returnLocation = WorldLocation(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                bot->GetPositionZ(), bot->GetOrientation());
            BuildRoute(*session, std::move(selectedSources));

            CaptureAndApplyStrategies(botAI, *session);

            RoutePoint const& firstPoint = session->route.front();
            if (_config.teleportToRoute)
            {
                session->waitingForInitialTeleport = true;
                if (!bot->TeleportTo(firstPoint.source.mapId, firstPoint.source.x, firstPoint.source.y,
                    firstPoint.source.z, firstPoint.source.orientation))
                {
                    RestoreStrategies(botAI, *session);
                    handler->SendErrorMessage("The playerbot could not teleport to the selected farming route.");
                    return false;
                }
            }
            else
            {
                SetTravelTarget(botAI, *session);
            }

            uint8 sourceMask = 0;
            for (RoutePoint const& point : session->route)
                sourceMask |= point.source.sourceMask;

            uint32 routeSize = session->route.size();
            uint32 routeMap = firstPoint.source.mapId;
            _sessions.emplace(bot->GetGUID(), std::move(session));

            handler->PSendSysMessage(
                "Autofarm started for {}: [{}] ({}), {} route points on map {}, sources: {}, goal: {}.",
                bot->GetName(), itemTemplate->Name1, itemTemplate->ItemId, routeSize, routeMap,
                SourceDescription(sourceMask), goalAmount ? std::to_string(goalAmount) : std::string("unlimited"));
            handler->SendSysMessage("Playerbots combat, recovery, looting, gathering, and skinning are active. "
                "Usable incidental profession nodes will also be gathered.");
            return true;
        }

        bool Stop(ChatHandler* handler, Player* bot)
        {
            PlayerbotAI* botAI = ValidateBot(handler, bot);
            if (!botAI)
                return false;

            if (!_sessions.contains(bot->GetGUID()))
            {
                handler->SendErrorMessage("That playerbot does not have an active autofarm session.");
                return false;
            }

            std::string botName = bot->GetName();
            StopSession(bot->GetGUID(), "stopped by owner", _config.returnOnStop, false);
            handler->PSendSysMessage("Autofarm stopped for {}{}.", botName,
                _config.returnOnStop ? " and the bot was returned" : "");
            return true;
        }

        bool StopAll(ChatHandler* handler)
        {
            ObjectGuid ownerGuid = handler->GetPlayer()->GetGUID();
            bool administrator = handler->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
            std::vector<ObjectGuid> sessions;
            for (auto const& [botGuid, session] : _sessions)
            {
                if (administrator || session->ownerGuid == ownerGuid)
                    sessions.push_back(botGuid);
            }

            for (ObjectGuid const& botGuid : sessions)
                StopSession(botGuid, "stopped by owner", _config.returnOnStop, false);

            handler->PSendSysMessage("Stopped {} autofarm session(s).", sessions.size());
            return true;
        }

        bool Status(ChatHandler* handler, Player* bot, bool uiStatus = false)
        {
            PlayerbotAI* botAI = ValidateBot(handler, bot);
            if (!botAI)
                return false;

            auto sessionIterator = _sessions.find(bot->GetGUID());
            if (sessionIterator == _sessions.end())
            {
                handler->SendErrorMessage("That playerbot does not have an active autofarm session.");
                return false;
            }

            FarmSession const& session = *sessionIterator->second;
            uint32 currentCount = bot->GetItemCount(session.itemId);
            uint32 gained = currentCount > session.startingCount ? currentCount - session.startingCount : 0;
            uint32 elapsedSeconds = getMSTimeDiff(session.startedAtMs, getMSTime()) / IN_MILLISECONDS;
            if (uiStatus)
            {
                RoutePoint const& point = session.route[session.routeIndex];
                float distance = bot->GetMapId() == point.source.mapId
                    ? bot->GetExactDist(point.source.x, point.source.y, point.source.z)
                    : -1.0f;
                uint32 ratePerHour = elapsedSeconds
                    ? static_cast<uint32>(static_cast<uint64>(gained) * HOUR / elapsedSeconds)
                    : 0;
                uint32 stalledSeconds = session.lastRouteProgressAtMs
                    ? getMSTimeDiff(session.lastRouteProgressAtMs, getMSTime()) / IN_MILLISECONDS
                    : 0;

                std::string state = "Waiting for movement";
                if (session.recoveringFromDeath || !bot->IsAlive() || bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                    state = "Corpse recovery";
                else if (bot->IsBeingTeleported())
                    state = "Teleporting";
                else if (session.flightStage == FlightStage::Takeoff)
                    state = "Taking off";
                else if (session.flightStage == FlightStage::Cruise)
                    state = "Flying to source";
                else if (session.flightStage == FlightStage::Landing)
                    state = "Landing at source";
                else if (session.flightStage == FlightStage::Evading)
                    state = "Evading attackers";
                else if (bot->IsInCombat())
                    state = "Combat";
                else if (session.interactionStartedAtMs)
                    state = point.source.sourceMask & SOURCE_GAMEOBJECT ? "Gathering" : "Looting";
                else if (session.stuckRecoveryAttempts)
                    state = "Path recovery";
                else if (bot->isMoving())
                    state = distance >= 0.0f && distance <= 35.0f ? "Approaching source" : "Traveling";

                AreaTableEntry const* area = GetAreaEntryByAreaID(bot->GetAreaId());
                std::string areaName = area ? PlayerbotAI::GetLocalizedAreaName(area) : "Unknown area";
                std::string sourceName = "Material source";
                if (point.source.sourceMask & SOURCE_GAMEOBJECT)
                {
                    if (GameObjectTemplate const* source = sObjectMgr->GetGameObjectTemplate(point.source.entry))
                        sourceName = source->name;
                }
                else if (CreatureTemplate const* source = sObjectMgr->GetCreatureTemplate(point.source.entry))
                    sourceName = source->Name;

                auto cleanField = [](std::string value)
                {
                    std::replace(value.begin(), value.end(), '|', '/');
                    std::replace(value.begin(), value.end(), '=', '-');
                    std::replace(value.begin(), value.end(), '\n', ' ');
                    std::replace(value.begin(), value.end(), '\r', ' ');
                    return value;
                };

                handler->PSendSysMessage(
                    "AFSTATUS|bot={}|state={}|level={}|health={:.0f}|area={}|map={}|x={:.0f}|y={:.0f}|"
                    "item={}|itemid={}|gained={}|goal={}|inventory={}|free={}|route={}|routes={}|loops={}|"
                    "source={}|distance={:.1f}|moving={}|recoveries={}|stalled={}|elapsed={}|rate={}",
                    cleanField(bot->GetName()), cleanField(state), bot->GetLevel(), bot->GetHealthPct(),
                    cleanField(areaName), bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(),
                    cleanField(session.itemName), session.itemId, gained, session.goalAmount, currentCount,
                    bot->GetFreeInventorySpace(), session.routeIndex + 1, session.route.size(),
                    session.completedLoops, cleanField(sourceName), distance, bot->isMoving() ? 1 : 0,
                    session.stuckRecoveryAttempts, stalledSeconds, elapsedSeconds, ratePerHour);
                return true;
            }

            handler->PSendSysMessage(
                "{} is farming [{}] ({}): gained {}/{}, inventory {}, route {}/{}, loops {}, elapsed {}m {}s.",
                bot->GetName(), session.itemName, session.itemId, gained,
                session.goalAmount ? std::to_string(session.goalAmount) : std::string("unlimited"), currentCount,
                session.routeIndex + 1, session.route.size(), session.completedLoops, elapsedSeconds / 60,
                elapsedSeconds % 60);
            return true;
        }

        void Search(ChatHandler* handler, std::string_view query)
        {
            std::string needle = ToLower(Trim(query));
            if (needle.empty())
            {
                handler->SendErrorMessage("Usage: .autofarm search <partial item name>");
                return;
            }

            std::vector<ItemTemplate const*> matches;
            for (auto const& [entry, itemTemplate] : *sObjectMgr->GetItemTemplateStore())
            {
                (void)entry;
                if (ToLower(itemTemplate.Name1).find(needle) != std::string::npos)
                    matches.push_back(&itemTemplate);
                if (matches.size() == 20)
                    break;
            }

            if (matches.empty())
            {
                handler->SendErrorMessage("No item names contain '{}'.", query);
                return;
            }

            handler->PSendSysMessage("Item matches for '{}':", query);
            SendItemMatches(handler, matches, 20);
        }

        Player* ResolveBotForCommand(ChatHandler* handler, std::string_view requestedName, bool allowOwnedFallback)
        {
            if (!requestedName.empty())
                return ObjectAccessor::FindPlayerByName(Trim(requestedName), false);

            Player* selected = handler->getSelectedPlayer();
            if (selected && sPlayerbotsMgr.GetPlayerbotAI(selected))
                return selected;

            if (!allowOwnedFallback)
                return nullptr;

            ObjectGuid ownerGuid = handler->GetPlayer()->GetGUID();
            Player* onlyBot = nullptr;
            for (auto const& [botGuid, session] : _sessions)
            {
                if (session->ownerGuid != ownerGuid)
                    continue;

                if (onlyBot)
                    return nullptr;
                onlyBot = ObjectAccessor::FindConnectedPlayer(botGuid);
            }
            return onlyBot;
        }

        void Update(uint32 diff)
        {
            if (!_config.enabled || _sessions.empty())
                return;

            _updateAccumulator += diff;
            if (_updateAccumulator < _config.updateIntervalMs)
                return;
            _updateAccumulator = 0;

            struct PendingStop
            {
                ObjectGuid botGuid;
                std::string reason;
            };
            std::vector<PendingStop> pendingStops;

            for (auto const& [botGuid, session] : _sessions)
            {
                if (std::optional<std::string> reason = UpdateSession(*session))
                    pendingStops.push_back({botGuid, *reason});
            }

            for (PendingStop const& pending : pendingStops)
                StopSession(pending.botGuid, pending.reason, _config.returnOnStop, true);
        }

        void OnPlayerLogout(Player* player)
        {
            if (!player)
                return;

            if (_sessions.contains(player->GetGUID()))
                StopSession(player->GetGUID(), "bot logged out", false, false);

            std::vector<ObjectGuid> ownedSessions;
            for (auto const& [botGuid, session] : _sessions)
                if (session->ownerGuid == player->GetGUID())
                    ownedSessions.push_back(botGuid);

            for (ObjectGuid const& botGuid : ownedSessions)
                StopSession(botGuid, "owner logged out", false, false);
        }

        void Shutdown()
        {
            std::vector<ObjectGuid> sessions;
            sessions.reserve(_sessions.size());
            for (auto const& [botGuid, session] : _sessions)
            {
                (void)session;
                sessions.push_back(botGuid);
            }

            for (ObjectGuid const& botGuid : sessions)
                StopSession(botGuid, "worldserver shutdown", false, false);
        }

    private:
        PlayerbotAI* ValidateBot(ChatHandler* handler, Player* bot) const
        {
            if (!bot)
            {
                handler->SendErrorMessage("Select an online playerbot or specify its character name.");
                return nullptr;
            }

            PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(bot);
            if (!botAI)
            {
                handler->SendErrorMessage("{} is not an online playerbot.", bot->GetName());
                return nullptr;
            }

            bool administrator = handler->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
            if (!administrator && botAI->GetMaster() != handler->GetPlayer())
            {
                handler->SendErrorMessage("You may only control your own added playerbots.");
                return nullptr;
            }

            return botAI;
        }

        bool ValidateStartState(ChatHandler* handler, Player const* bot) const
        {
            if (!bot->IsInWorld() || bot->IsBeingTeleported())
            {
                handler->SendErrorMessage("The playerbot must be fully in the world and not teleporting.");
                return false;
            }
            if (!bot->IsAlive())
            {
                handler->SendErrorMessage("Revive the playerbot before starting autofarm.");
                return false;
            }
            if (bot->IsInCombat())
            {
                handler->SendErrorMessage("The playerbot cannot start autofarm while in combat.");
                return false;
            }
            if (bot->IsInFlight() || bot->HasUnitState(UNIT_STATE_IN_FLIGHT))
            {
                handler->SendErrorMessage("The playerbot cannot start autofarm while using a taxi.");
                return false;
            }
            if (bot->InBattleground() || bot->InArena() || (bot->FindMap() && bot->FindMap()->Instanceable()))
            {
                handler->SendErrorMessage("Autofarm may only start from an outdoor, non-instanced map.");
                return false;
            }
            if (bot->duel)
            {
                handler->SendErrorMessage("The playerbot cannot start autofarm during a duel.");
                return false;
            }
            return true;
        }

        static void SendItemMatches(ChatHandler* handler, std::vector<ItemTemplate const*> const& matches, size_t limit)
        {
            size_t count = std::min(limit, matches.size());
            for (size_t index = 0; index < count; ++index)
                handler->PSendSysMessage("  {} - {}", matches[index]->ItemId, matches[index]->Name1);
        }

        std::vector<SourceSpawn> FindSources(Player* bot, uint32 itemId) const
        {
            std::unordered_map<uint32, uint8> creatureEntries;
            std::unordered_set<uint32> gameObjectEntries;

            if (CreatureTemplateContainer const* creatures = sObjectMgr->GetCreatureTemplates())
            {
                for (auto const& [entry, creatureTemplate] : *creatures)
                {
                    if (creatureTemplate.rank > CREATURE_ELITE_NORMAL)
                        continue;
                    if (creatureTemplate.maxlevel > bot->GetLevel() + _config.maxCreatureLevelsAboveBot)
                        continue;

                    ObjectGuid guid = ObjectGuid::Create<HighGuid::Unit>(entry, uint32(1));
                    LootTemplateAccess const* corpseLoot = DropMapValue::GetLootTemplate(guid, LOOT_CORPSE);
                    if (LootTemplateContainsItem(corpseLoot, itemId))
                        creatureEntries[entry] |= SOURCE_CREATURE_LOOT;

                    LootTemplateAccess const* skinLoot = DropMapValue::GetLootTemplate(guid, LOOT_SKINNING);
                    if (LootTemplateContainsItem(skinLoot, itemId) && CanSkinCreature(bot, &creatureTemplate))
                        creatureEntries[entry] |= SOURCE_CREATURE_SKIN;
                }
            }

            if (GameObjectTemplateContainer const* gameObjects = sObjectMgr->GetGameObjectTemplates())
            {
                for (auto const& [entry, gameObjectTemplate] : *gameObjects)
                {
                    ObjectGuid guid = ObjectGuid::Create<HighGuid::GameObject>(entry, uint32(1));
                    LootTemplateAccess const* loot = DropMapValue::GetLootTemplate(guid, LOOT_CORPSE);
                    if (LootTemplateContainsItem(loot, itemId) && CanUseGameObject(bot, &gameObjectTemplate))
                        gameObjectEntries.insert(entry);
                }
            }

            std::vector<SourceSpawn> sources;
            sources.reserve(sObjectMgr->GetAllCreatureData().size() / 20 + sObjectMgr->GetAllGOData().size() / 20);

            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                auto entry = creatureEntries.find(data.id);
                if (entry == creatureEntries.end() || !entry->second || !MapAllowed(_config, data.mapid))
                    continue;
                if (!(data.phaseMask & PHASEMASK_NORMAL))
                    continue;

                sources.push_back({spawnId, data.id, data.mapid, data.posX, data.posY, data.posZ, data.orientation,
                    entry->second});
            }

            for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
            {
                if (!gameObjectEntries.contains(data.id) || !MapAllowed(_config, data.mapid))
                    continue;
                if (!(data.phaseMask & PHASEMASK_NORMAL))
                    continue;

                sources.push_back({spawnId, data.id, data.mapid, data.posX, data.posY, data.posZ, data.orientation,
                    SOURCE_GAMEOBJECT});
            }

            return sources;
        }

        std::vector<SourceSpawn> SelectCluster(Player* bot, std::vector<SourceSpawn> const& sources) const
        {
            std::unordered_map<ClusterKey, std::vector<size_t>, ClusterKeyHash> clusters;
            for (size_t index = 0; index < sources.size(); ++index)
            {
                SourceSpawn const& source = sources[index];
                ClusterKey key;
                key.mapId = source.mapId;
                key.x = static_cast<int32>(std::floor(source.x / _config.clusterSize));
                key.y = static_cast<int32>(std::floor(source.y / _config.clusterSize));
                clusters[key].push_back(index);
            }

            ClusterKey bestKey;
            bool found = false;
            double bestScore = -std::numeric_limits<double>::max();
            for (auto const& [key, indices] : clusters)
            {
                float centerX = (static_cast<float>(key.x) + 0.5f) * _config.clusterSize;
                float centerY = (static_cast<float>(key.y) + 0.5f) * _config.clusterSize;
                double anchorPenalty = FactionAnchorDistance(bot, key.mapId, centerX, centerY);
                double score = static_cast<double>(indices.size()) * 10000.0 - anchorPenalty * 5.0;

                if (!found || score > bestScore)
                {
                    found = true;
                    bestKey = key;
                    bestScore = score;
                }
            }

            if (!found)
                return {};

            float centerX = (static_cast<float>(bestKey.x) + 0.5f) * _config.clusterSize;
            float centerY = (static_cast<float>(bestKey.y) + 0.5f) * _config.clusterSize;
            double radiusSquared = static_cast<double>(_config.clusterRadius) * _config.clusterRadius;

            std::vector<SourceSpawn> selected;
            for (SourceSpawn const& source : sources)
            {
                if (source.mapId != bestKey.mapId)
                    continue;
                if (SquaredDistance(source.x, source.y, centerX, centerY) <= radiusSquared)
                    selected.push_back(source);
            }

            if (selected.size() > _config.maxRoutePoints)
            {
                std::sort(selected.begin(), selected.end(), [centerX, centerY](SourceSpawn const& left,
                    SourceSpawn const& right)
                {
                    return SquaredDistance(left.x, left.y, centerX, centerY) <
                        SquaredDistance(right.x, right.y, centerX, centerY);
                });
                selected.resize(_config.maxRoutePoints);
            }

            std::vector<SourceSpawn> ordered;
            ordered.reserve(selected.size());
            float currentX = centerX;
            float currentY = centerY;
            while (!selected.empty())
            {
                auto nearest = std::min_element(selected.begin(), selected.end(), [currentX, currentY](
                    SourceSpawn const& left, SourceSpawn const& right)
                {
                    return SquaredDistance(left.x, left.y, currentX, currentY) <
                        SquaredDistance(right.x, right.y, currentX, currentY);
                });

                currentX = nearest->x;
                currentY = nearest->y;
                ordered.push_back(*nearest);
                selected.erase(nearest);
            }

            return ordered;
        }

        static void BuildRoute(FarmSession& session, std::vector<SourceSpawn> sources)
        {
            session.route.reserve(sources.size());
            for (SourceSpawn const& source : sources)
            {
                RoutePoint point;
                point.source = source;
                point.position = std::make_unique<WorldPosition>(source.mapId, source.x, source.y, source.z,
                    source.orientation);
                point.destination = std::make_unique<AutofarmDestination>(point.position.get(), session.itemName);
                session.route.push_back(std::move(point));
            }
        }

        static void SetStrategy(PlayerbotAI* botAI, std::string const& name, BotState state, bool enable)
        {
            if (botAI->HasStrategy(name, state) == enable)
                return;
            botAI->ChangeStrategy(std::string(enable ? "+" : "-") + name, state);
        }

        static void SuppressFlightCombatStrategies(PlayerbotAI* botAI, FarmSession& session)
        {
            if (session.combatStrategiesSuppressed)
                return;

            botAI->ClearStrategies(BOT_STATE_COMBAT);
            session.combatStrategiesSuppressed = true;
        }

        static void RestoreFlightCombatStrategies(PlayerbotAI* botAI, FarmSession& session)
        {
            if (!botAI || !session.combatStrategiesSuppressed)
                return;

            botAI->ClearStrategies(BOT_STATE_COMBAT);
            for (std::string const& strategy : session.combatStrategies)
                botAI->ChangeStrategy("+" + strategy, BOT_STATE_COMBAT);
            session.combatStrategiesSuppressed = false;
        }

        static bool IsValidHeight(float height)
        {
            return std::isfinite(height) && height != INVALID_HEIGHT && height != VMAP_INVALID_HEIGHT_VALUE;
        }

        static float GetSurfaceHeight(Player* bot, float x, float y, float fallback)
        {
            Map* map = bot->GetMap();
            if (!map)
                return fallback;

            float surface = fallback;
            float ground = map->GetHeight(x, y, MAX_HEIGHT, false);
            if (IsValidHeight(ground))
                surface = std::max(surface, ground);

            float water = map->GetWaterLevel(x, y);
            if (IsValidHeight(water))
                surface = std::max(surface, water);
            return surface;
        }

        float CalculateFlightCruiseZ(Player* bot, SourceSpawn const& source) const
        {
            float startX = bot->GetPositionX();
            float startY = bot->GetPositionY();
            float deltaX = source.x - startX;
            float deltaY = source.y - startY;
            float distance = std::hypot(deltaX, deltaY);
            uint32 samples = std::clamp<uint32>(static_cast<uint32>(std::ceil(distance / 100.0f)), 1, 40);
            float highestSurface = source.z;

            for (uint32 index = 0; index <= samples; ++index)
            {
                float ratio = static_cast<float>(index) / static_cast<float>(samples);
                float x = startX + deltaX * ratio;
                float y = startY + deltaY * ratio;
                highestSurface = std::max(highestSurface, GetSurfaceHeight(bot, x, y, source.z));
            }

            return std::max(bot->GetPositionZ(), highestSurface + _config.flightHeight);
        }

        static bool IsFarmCreatureEngaged(Player* bot, RoutePoint const& point)
        {
            if (point.source.sourceMask & SOURCE_GAMEOBJECT)
                return false;

            Creature* creature = ObjectAccessor::GetSpawnedCreatureByDBGUID(point.source.mapId,
                point.source.spawnId);
            if (!creature || !creature->IsAlive())
                return false;

            Unit* victim = creature->GetVictim();
            return bot->GetVictim() == creature || victim == bot ||
                (victim && victim->GetOwnerGUID() == bot->GetGUID());
        }

        static void ClearActiveFlight(Player* bot)
        {
            if (!bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_ASCENDING |
                MOVEMENTFLAG_DESCENDING))
                return;

            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_ASCENDING |
                MOVEMENTFLAG_DESCENDING);
            if (!bot->IsRooted())
                bot->SendMovementFlagUpdate();
        }

        static bool StartFlightMove(PlayerbotAI* botAI, FarmSession& session, FlightStage stage,
            float x, float y, float z, uint32 now)
        {
            Player* bot = botAI->GetBot();
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
            botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().clear();

            if (!bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING))
            {
                bot->AddUnitMovementFlag(MOVEMENTFLAG_FLYING);
                if (!bot->IsRooted())
                    bot->SendMovementFlagUpdate();
            }

            AutofarmMovementAction movement(botAI);
            if (!movement.FlyTo(bot->GetMapId(), x, y, z))
                return false;

            session.flightStage = stage;
            session.lastRouteProgressAtMs = now;
            return true;
        }

        bool IsFlightTravelReady(Player* bot) const
        {
            if (!_config.useFlyingMounts || (bot->GetMapId() != 530 && bot->GetMapId() != 571))
                return false;

            bool mountedFlight = bot->IsMounted() && bot->HasIncreaseMountedFlightSpeedAura();
            ShapeshiftForm form = bot->GetShapeshiftForm();
            bool flightForm = bot->HasFlyAura() && (form == FORM_FLIGHT || form == FORM_FLIGHT_EPIC);
            return mountedFlight || flightForm;
        }

        bool UpdateFlightTravel(PlayerbotAI* botAI, FarmSession& session, RoutePoint const& point, uint32 now)
        {
            Player* bot = botAI->GetBot();
            SuppressFlightCombatStrategies(botAI, session);

            bool farmCreatureEngaged = IsFarmCreatureEngaged(bot, point);
            bool evadingUnrelatedCombat = bot->IsInCombat() && !farmCreatureEngaged;
            if (evadingUnrelatedCombat)
            {
                if (!session.flightCombatStartedAtMs)
                    session.flightCombatStartedAtMs = now;
                else if (getMSTimeDiff(session.flightCombatStartedAtMs, now) >= _config.flightCombatEscapeMs)
                {
                    if (_config.debug)
                    {
                        LOG_DEBUG("module.autofarm",
                            "Bot {} remained in unrelated combat while flying; skipped route point {}/{}",
                            bot->GetName(), session.routeIndex + 1, session.route.size());
                    }
                    AdvanceRoute(botAI, session);
                    return true;
                }
            }
            else
                session.flightCombatStartedAtMs = 0;

            float horizontalDistance = bot->GetExactDist2d(point.source.x, point.source.y);
            if (session.lastRouteDistance == std::numeric_limits<float>::max() ||
                horizontalDistance + AUTOFARM_PROGRESS_DISTANCE < session.lastRouteDistance)
            {
                session.lastRouteDistance = horizontalDistance;
                session.lastRouteProgressAtMs = now;
                session.stuckRecoveryAttempts = 0;
            }

            if (session.flightCruiseZ == std::numeric_limits<float>::lowest())
                session.flightCruiseZ = CalculateFlightCruiseZ(bot, point.source);

            if (horizontalDistance > AUTOFARM_FLIGHT_APPROACH_DISTANCE)
            {
                if (bot->GetPositionZ() + AUTOFARM_FLIGHT_LANDING_HEIGHT < session.flightCruiseZ)
                {
                    if (session.flightStage != FlightStage::Takeoff || !bot->isMoving())
                    {
                        StartFlightMove(botAI, session, FlightStage::Takeoff, bot->GetPositionX(),
                            bot->GetPositionY(), session.flightCruiseZ, now);
                    }
                }
                else if (session.flightStage != FlightStage::Cruise || !bot->isMoving())
                {
                    StartFlightMove(botAI, session, FlightStage::Cruise, point.source.x, point.source.y,
                        session.flightCruiseZ, now);
                }
                return true;
            }

            if (evadingUnrelatedCombat)
            {
                float surface = GetSurfaceHeight(bot, bot->GetPositionX(), bot->GetPositionY(), point.source.z);
                float escapeZ = std::max(bot->GetPositionZ(), surface + _config.flightHeight);
                if (session.flightStage != FlightStage::Evading || !bot->isMoving())
                {
                    StartFlightMove(botAI, session, FlightStage::Evading, bot->GetPositionX(),
                        bot->GetPositionY(), escapeZ, now);
                }
                return true;
            }

            if (bot->GetPositionZ() > point.source.z + AUTOFARM_FLIGHT_LANDING_HEIGHT)
            {
                if (session.flightStage != FlightStage::Landing || !bot->isMoving())
                {
                    StartFlightMove(botAI, session, FlightStage::Landing, point.source.x, point.source.y,
                        point.source.z + 1.0f, now);
                }
                return true;
            }

            ClearActiveFlight(bot);
            session.flightStage = FlightStage::None;
            session.flightCruiseZ = std::numeric_limits<float>::lowest();
            return false;
        }

        static void CaptureAndApplyStrategies(PlayerbotAI* botAI, FarmSession& session)
        {
            static std::vector<std::pair<std::string, bool>> const desiredStrategies =
            {
                {"loot", true},
                {"gather", true},
                {"grind", true},
                {"travel", true},
                {"follow", false},
                {"stay", false},
                {"rpg", false},
                {"move random", false}
            };

            session.combatStrategies = botAI->GetStrategies(BOT_STATE_COMBAT);
            for (auto const& [name, enabled] : desiredStrategies)
            {
                bool wasEnabled = botAI->HasStrategy(name, BOT_STATE_NON_COMBAT);
                session.strategies.push_back({name, BOT_STATE_NON_COMBAT, wasEnabled});
                SetStrategy(botAI, name, BOT_STATE_NON_COMBAT, enabled);
            }

            auto& alwaysLoot = botAI->GetAiObjectContext()
                ->GetValue<std::set<uint32>&>("always loot list")->Get();
            session.itemWasAlwaysLooted = alwaysLoot.contains(session.itemId);
            alwaysLoot.insert(session.itemId);
        }

        static void RestoreStrategies(PlayerbotAI* botAI, FarmSession& session)
        {
            if (!botAI)
                return;

            RestoreFlightCombatStrategies(botAI, session);
            TravelMgr::instance().setNullTravelTarget(botAI->GetBot());
            botAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Reset();
            botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({});

            for (StrategyState const& strategy : session.strategies)
                SetStrategy(botAI, strategy.name, strategy.state, strategy.wasEnabled);

            if (!session.itemWasAlwaysLooted)
            {
                auto& alwaysLoot = botAI->GetAiObjectContext()
                    ->GetValue<std::set<uint32>&>("always loot list")->Get();
                alwaysLoot.erase(session.itemId);
            }
        }

        static void SetTravelTarget(PlayerbotAI* botAI, FarmSession& session)
        {
            if (!botAI || session.route.empty())
                return;

            RoutePoint& point = session.route[session.routeIndex];
            TravelTarget* target = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
            if (!target)
                return;

            target->setTarget(point.destination.get(), point.position.get());
            target->setForced(true);
            target->setExpireIn(HOUR * IN_MILLISECONDS);
            session.pointStartedAtMs = getMSTime();
            session.interactionStartedAtMs = 0;
            session.lastRouteDistance = std::numeric_limits<float>::max();
            session.lastRouteProgressAtMs = session.pointStartedAtMs;
            session.stuckRecoveryAttempts = 0;
            session.flightStage = FlightStage::None;
            session.flightCruiseZ = std::numeric_limits<float>::lowest();
            session.flightCombatStartedAtMs = 0;

            if (AutofarmMgr::Instance()._config.debug)
            {
                LOG_DEBUG("module.autofarm", "Bot {} route point {}/{}: map {} ({}, {}, {}), entry {}, spawn {}",
                    botAI->GetBot()->GetName(), session.routeIndex + 1, session.route.size(), point.source.mapId,
                    point.source.x, point.source.y, point.source.z, point.source.entry, point.source.spawnId);
            }
        }

        void AdvanceRoute(PlayerbotAI* botAI, FarmSession& session)
        {
            botAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Reset();
            botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({});

            ++session.routeIndex;
            if (session.routeIndex >= session.route.size())
            {
                session.routeIndex = 0;
                ++session.completedLoops;
            }

            SetTravelTarget(botAI, session);
        }

        static void RecoverFromDeath(PlayerbotAI* botAI, FarmSession& session)
        {
            Player* bot = botAI->GetBot();
            if (!session.recoveringFromDeath)
            {
                session.recoveringFromDeath = true;
                session.interactionStartedAtMs = 0;
                bot->GetMotionMaster()->Clear();
                bot->StopMoving();
                botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().clear();
                botAI->GetAiObjectContext()->GetValue<LootObjectStack*>("available loot")->Get()->Clear();
                botAI->GetAiObjectContext()->GetValue<LootObject>("loot target")->Set(LootObject());
                botAI->GetAiObjectContext()->GetValue<ObjectGuid>("pull target")->Reset();
                botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({});
                LOG_INFO("module.autofarm", "Autofarm bot {} died; beginning corpse recovery", bot->GetName());
            }

            if (!bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            {
                botAI->DoSpecificAction("auto release", Event(), true);
                return;
            }

            Corpse* corpse = bot->GetCorpse();
            if (!corpse)
            {
                botAI->DoSpecificAction("repop", Event(), true);
                return;
            }

            bool corpseIsNear = corpse->GetMapId() == bot->GetMapId() &&
                bot->GetDistance(corpse) < CORPSE_RECLAIM_RADIUS - 5.0f;
            if (corpseIsNear)
                botAI->DoSpecificAction("revive from corpse", Event(), true);
            else
                botAI->DoSpecificAction("find corpse", Event(), true);
        }

        bool RecoverStalledRoute(PlayerbotAI* botAI, FarmSession& session, RoutePoint const& point,
            float distance, uint32 now)
        {
            if (session.lastRouteDistance == std::numeric_limits<float>::max() ||
                distance + AUTOFARM_PROGRESS_DISTANCE < session.lastRouteDistance)
            {
                session.lastRouteDistance = distance;
                session.lastRouteProgressAtMs = now;
                session.stuckRecoveryAttempts = 0;
                return false;
            }

            if (getMSTimeDiff(session.lastRouteProgressAtMs, now) < AUTOFARM_STUCK_TIMEOUT_MS)
                return false;

            Player* bot = botAI->GetBot();
            session.lastRouteDistance = distance;
            session.lastRouteProgressAtMs = now;
            ++session.stuckRecoveryAttempts;

            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
            botAI->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().clear();

            if (session.stuckRecoveryAttempts >= AUTOFARM_RETRIES_BEFORE_TELEPORT)
            {
                if (_config.teleportToRoute && bot->TeleportTo(point.source.mapId, point.source.x, point.source.y,
                    point.source.z, point.source.orientation))
                {
                    LOG_WARN("module.autofarm",
                        "Bot {} remained stalled at route point {}/{}; teleported to the source on map {}",
                        bot->GetName(), session.routeIndex + 1, session.route.size(), point.source.mapId);
                    session.stuckRecoveryAttempts = 0;
                    return true;
                }

                LOG_WARN("module.autofarm", "Bot {} could not recover route point {}/{}; skipping it",
                    bot->GetName(), session.routeIndex + 1, session.route.size());
                AdvanceRoute(botAI, session);
                return true;
            }

            AutofarmMovementAction movement(botAI);
            bool movementStarted = movement.MoveToPoint(point.source);
            LOG_WARN("module.autofarm", "Bot {} stalled {:.1f} yards from route point {}/{}; forced path {}",
                bot->GetName(), distance, session.routeIndex + 1, session.route.size(),
                movementStarted ? "started" : "could not start");
            return true;
        }

        std::optional<std::string> UpdateSession(FarmSession& session)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(session.botGuid);
            if (!bot || !bot->IsInWorld())
                return "bot is no longer online";

            PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(bot);
            if (!botAI)
                return "playerbot AI is no longer available";

            if (!IsFlightTravelReady(bot))
                RestoreFlightCombatStrategies(botAI, session);

            if (!bot->IsAlive() || bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            {
                RecoverFromDeath(botAI, session);
                return std::nullopt;
            }

            if (session.recoveringFromDeath)
            {
                session.recoveringFromDeath = false;
                AdvanceRoute(botAI, session);
                LOG_INFO("module.autofarm", "Autofarm bot {} recovered its corpse; route resumed at point {}/{}",
                    bot->GetName(), session.routeIndex + 1, session.route.size());
                return std::nullopt;
            }

            uint32 currentCount = bot->GetItemCount(session.itemId);
            uint32 gained = currentCount > session.startingCount ? currentCount - session.startingCount : 0;
            if (session.goalAmount && gained >= session.goalAmount)
                return Acore::StringFormat("goal reached: collected {} {}", gained, session.itemName);

            ItemPosCountVec destinations;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, destinations, session.itemId, 1) != EQUIP_ERR_OK)
                return Acore::StringFormat("inventory cannot store another {}", session.itemName);

            if (bot->IsBeingTeleported())
                return std::nullopt;

            if (session.waitingForInitialTeleport)
            {
                session.waitingForInitialTeleport = false;
                SetTravelTarget(botAI, session);
                return std::nullopt;
            }

            if (bot->InBattleground() || bot->InArena() || (bot->FindMap() && bot->FindMap()->Instanceable()))
                return "bot entered an unsupported instanced map";

            RoutePoint& point = session.route[session.routeIndex];
            if (bot->GetMapId() != point.source.mapId)
            {
                SetTravelTarget(botAI, session);
                return std::nullopt;
            }

            uint32 now = getMSTime();
            if (session.pointStartedAtMs &&
                getMSTimeDiff(session.pointStartedAtMs, now) > _config.routePointTimeoutMs)
            {
                if (_config.debug)
                    LOG_DEBUG("module.autofarm", "Bot {} skipped timed-out route point {}", bot->GetName(),
                        session.routeIndex + 1);
                AdvanceRoute(botAI, session);
                return std::nullopt;
            }

            float distance = bot->GetExactDist(point.source.x, point.source.y, point.source.z);
            if (IsFlightTravelReady(bot))
            {
                if (UpdateFlightTravel(botAI, session, point, now))
                    return std::nullopt;
                RestoreFlightCombatStrategies(botAI, session);
            }

            bool farmCreatureEngaged = IsFarmCreatureEngaged(bot, point);
            if (bot->IsInCombat() && !farmCreatureEngaged)
                return std::nullopt;

            bool needsRouteProgress = distance > 35.0f ||
                ((point.source.sourceMask & SOURCE_GAMEOBJECT) && distance > AUTOFARM_INTERACTION_DISTANCE);
            if (needsRouteProgress && RecoverStalledRoute(botAI, session, point, distance, now))
                return std::nullopt;

            TravelTarget* travelTarget = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
            if (distance > 35.0f)
            {
                if (!travelTarget || !travelTarget->isTraveling())
                {
                    RoutePoint& currentPoint = session.route[session.routeIndex];
                    if (travelTarget)
                    {
                        travelTarget->setTarget(currentPoint.destination.get(), currentPoint.position.get());
                        travelTarget->setForced(true);
                        travelTarget->setExpireIn(HOUR * IN_MILLISECONDS);
                    }
                }
                return std::nullopt;
            }

            if (point.source.sourceMask & SOURCE_GAMEOBJECT)
                ProcessGameObjectPoint(botAI, session, point);
            else
                ProcessCreaturePoint(botAI, session, point);

            return std::nullopt;
        }

        void ProcessGameObjectPoint(PlayerbotAI* botAI, FarmSession& session, RoutePoint const& point)
        {
            Player* bot = botAI->GetBot();
            GameObject* gameObject = ObjectAccessor::GetSpawnedGameObjectByDBGUID(point.source.mapId,
                point.source.spawnId);
            if (!gameObject || !gameObject->IsInWorld() || !gameObject->isSpawned())
            {
                AdvanceRoute(botAI, session);
                return;
            }

            if (bot->GetDistance(gameObject) > AUTOFARM_INTERACTION_DISTANCE)
            {
                session.interactionStartedAtMs = 0;
                AutofarmMovementAction movement(botAI);
                movement.MoveToTarget(gameObject);
                return;
            }

            LootObject loot(bot, gameObject->GetGUID());
            if (loot.IsEmpty() || !loot.IsLootPossible(bot))
            {
                AdvanceRoute(botAI, session);
                return;
            }

            uint32 now = getMSTime();
            if (!session.interactionStartedAtMs)
            {
                session.interactionStartedAtMs = now;
                botAI->GetAiObjectContext()->GetValue<LootObjectStack*>("available loot")->Get()
                    ->Add(gameObject->GetGUID());
                botAI->GetAiObjectContext()->GetValue<LootObject>("loot target")->Set(loot);
            }

            if (!bot->IsNonMeleeSpellCast(false))
                botAI->DoSpecificAction("open loot", Event(), true);

            if (getMSTimeDiff(session.interactionStartedAtMs, now) > _config.interactionTimeoutMs)
                AdvanceRoute(botAI, session);
        }

        void ProcessCreaturePoint(PlayerbotAI* botAI, FarmSession& session, RoutePoint const& point)
        {
            Player* bot = botAI->GetBot();
            Creature* creature = ObjectAccessor::GetSpawnedCreatureByDBGUID(point.source.mapId, point.source.spawnId);
            if (!creature || !creature->IsInWorld())
            {
                AdvanceRoute(botAI, session);
                return;
            }

            if (creature->IsAlive())
            {
                if (!bot->IsValidAttackTarget(creature) || !bot->IsHostileTo(creature))
                {
                    AdvanceRoute(botAI, session);
                    return;
                }

                if (creature->IsInCombat() && creature->GetVictim() && creature->GetVictim() != bot &&
                    creature->GetVictim()->GetOwnerGUID() != bot->GetGUID())
                {
                    AdvanceRoute(botAI, session);
                    return;
                }

                botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")
                    ->Set({creature->GetGUID()});
                AutofarmAttackAction attack(botAI);
                attack.AttackTarget(creature);
                return;
            }

            LootObject loot(bot, creature->GetGUID());
            if (loot.IsEmpty() || !loot.IsLootPossible(bot))
            {
                AdvanceRoute(botAI, session);
                return;
            }

            uint32 now = getMSTime();
            if (!session.interactionStartedAtMs)
            {
                session.interactionStartedAtMs = now;
                botAI->GetAiObjectContext()->GetValue<LootObjectStack*>("available loot")->Get()
                    ->Add(creature->GetGUID());
                botAI->DoSpecificAction("add all loot", Event(), true);
                return;
            }

            if (getMSTimeDiff(session.interactionStartedAtMs, now) > _config.interactionTimeoutMs)
                AdvanceRoute(botAI, session);
        }

        void StopSession(ObjectGuid botGuid, std::string const& reason, bool returnBot, bool notifyOwner)
        {
            auto sessionIterator = _sessions.find(botGuid);
            if (sessionIterator == _sessions.end())
                return;

            std::unique_ptr<FarmSession> session = std::move(sessionIterator->second);
            _sessions.erase(sessionIterator);

            Player* bot = ObjectAccessor::FindConnectedPlayer(botGuid);
            PlayerbotAI* botAI = bot ? sPlayerbotsMgr.GetPlayerbotAI(bot) : nullptr;
            RestoreStrategies(botAI, *session);

            bool returned = false;
            if (returnBot && bot && bot->IsInWorld() && bot->IsAlive() && !bot->IsInCombat() &&
                !bot->IsBeingTeleported())
            {
                returned = bot->TeleportTo(session->returnLocation);
            }

            if (notifyOwner)
            {
                if (Player* owner = ObjectAccessor::FindConnectedPlayer(session->ownerGuid))
                {
                    ChatHandler(owner->GetSession()).PSendSysMessage("Autofarm for {} stopped: {}{}.",
                        bot ? bot->GetName() : "offline bot", reason, returned ? "; returned to start" : "");
                }
            }

            LOG_INFO("module.autofarm", "Autofarm session for {} stopped: {}{}",
                bot ? bot->GetName() : botGuid.ToString(), reason, returned ? " (returned)" : "");
        }

        AutofarmConfig _config;
        uint32 _updateAccumulator = 0;
        std::unordered_map<ObjectGuid, std::unique_ptr<FarmSession>> _sessions;
    };

    class AutofarmWorldScript final : public WorldScript
    {
    public:
        AutofarmWorldScript() : WorldScript("AutofarmWorldScript") { }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            AutofarmMgr::Instance().LoadConfig();
        }

        void OnUpdate(uint32 diff) override
        {
            AutofarmMgr::Instance().Update(diff);
        }

        void OnShutdown() override
        {
            AutofarmMgr::Instance().Shutdown();
        }
    };

    class AutofarmPlayerScript final : public PlayerScript
    {
    public:
        AutofarmPlayerScript() : PlayerScript("AutofarmPlayerScript", {PLAYERHOOK_ON_LOGOUT}) { }

        void OnPlayerLogout(Player* player) override
        {
            AutofarmMgr::Instance().OnPlayerLogout(player);
        }
    };

    class AutofarmCommandScript final : public CommandScript
    {
    public:
        AutofarmCommandScript() : CommandScript("AutofarmCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable autofarmCommands =
            {
                {"start", HandleStart, SEC_PLAYER, Console::No},
                {"startbot", HandleStartBot, SEC_PLAYER, Console::No},
                {"stop", HandleStop, SEC_PLAYER, Console::No},
                {"stopall", HandleStopAll, SEC_PLAYER, Console::No},
                {"status", HandleStatus, SEC_PLAYER, Console::No},
                {"statusui", HandleStatusUi, SEC_PLAYER, Console::No},
                {"search", HandleSearch, SEC_PLAYER, Console::No}
            };
            static ChatCommandTable commands =
            {
                {"autofarm", autofarmCommands}
            };
            return commands;
        }

        static bool HandleStart(ChatHandler* handler, Tail itemText)
        {
            Player* bot = AutofarmMgr::Instance().ResolveBotForCommand(handler, {}, false);
            if (!bot)
            {
                handler->SendErrorMessage("Select an owned online playerbot first, then use "
                    ".autofarm start <item> [--count number].");
                return false;
            }
            return AutofarmMgr::Instance().Start(handler, bot, Trim(itemText));
        }

        static bool HandleStartBot(ChatHandler* handler, Tail arguments)
        {
            std::string text = Trim(arguments);
            size_t separator = text.find(' ');
            if (separator == std::string::npos)
            {
                handler->SendErrorMessage("Usage: .autofarm startbot <character> <item> [--count number]");
                return false;
            }

            std::string botName = text.substr(0, separator);
            std::string itemText = Trim(std::string_view(text).substr(separator + 1));
            Player* bot = AutofarmMgr::Instance().ResolveBotForCommand(handler, botName, false);
            if (!bot)
            {
                handler->SendErrorMessage("Online playerbot '{}' was not found.", botName);
                return false;
            }
            return AutofarmMgr::Instance().Start(handler, bot, itemText);
        }

        static bool HandleStop(ChatHandler* handler, Tail botName)
        {
            Player* bot = AutofarmMgr::Instance().ResolveBotForCommand(handler, Trim(botName), true);
            if (!bot)
            {
                handler->SendErrorMessage("Select the playerbot, specify its name, or use .autofarm stopall.");
                return false;
            }
            return AutofarmMgr::Instance().Stop(handler, bot);
        }

        static bool HandleStopAll(ChatHandler* handler)
        {
            return AutofarmMgr::Instance().StopAll(handler);
        }

        static bool HandleStatus(ChatHandler* handler, Tail botName)
        {
            Player* bot = AutofarmMgr::Instance().ResolveBotForCommand(handler, Trim(botName), true);
            if (!bot)
            {
                handler->SendErrorMessage("Select the playerbot or specify its character name.");
                return false;
            }
            return AutofarmMgr::Instance().Status(handler, bot);
        }

        static bool HandleStatusUi(ChatHandler* handler, Tail botName)
        {
            Player* bot = AutofarmMgr::Instance().ResolveBotForCommand(handler, Trim(botName), true);
            if (!bot)
            {
                handler->SendErrorMessage("Select the playerbot or specify its character name.");
                return false;
            }
            return AutofarmMgr::Instance().Status(handler, bot, true);
        }

        static bool HandleSearch(ChatHandler* handler, Tail itemText)
        {
            AutofarmMgr::Instance().Search(handler, itemText);
            return true;
        }
    };
}

void AddSC_mod_autofarm()
{
    new AutofarmWorldScript();
    new AutofarmPlayerScript();
    new AutofarmCommandScript();
}
