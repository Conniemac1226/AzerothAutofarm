# mod-autofarm

`mod-autofarm` lets an owned playerbot farm a selected raw material without hardcoded zone or item lists. It uses the
live AzerothCore item, loot, creature, gameobject, and spawn stores to locate outdoor sources, chooses a productive
navigation-friendly single-zone route, teleports to it, and lets `mod-playerbots` handle movement, combat, looting,
gathering, skinning, and death recovery. While a session is active, food/drink downtime and repetitive buff refreshes
are suppressed; health and mana are restored between fights instead.

It can also farm Vanilla reputation that is awarded directly by outdoor creature kills. Reputation targets, team
restrictions, reward values, and maximum standings come from the live `creature_onkill_reputation` data rather than a
hardcoded faction or creature list.

## Requirements

- AzerothCore WotLK
- `mod-playerbots`, enabled and configured to let the account add its own characters
- For another character: a bot logged in through playerbots, normally with `.playerbots bot add <name>`
- For the currently played character: selfbot permission through `AiPlayerbot.SelfBotLevel`
- The required gathering profession and skill for ore, herbs, leather, or profession-skinned elementals
- Required tools such as a mining pick or skinning knife

This module deliberately uses playerbots for combat. Azerocombat is a client-side addon and cannot control a character
that is logged in by the server as a playerbot, so it is neither required nor used.

## Optional client addon

The WotLK 3.3.5a `AzerothAutofarm` addon in `client/AzerothAutofarm/` provides a standalone graphical control panel for
the commands below. It includes material presets, custom item/link input, favorites, quantity goals, bot selection,
reputation starts, session controls, an activity log, help, and a minimap button. Reputation has its own tab containing
the available Vanilla factions; selecting one changes the normal Start Farming workflow into Start Reputation. The
Activity window adapts its progress, standing, and rate fields for reputation sessions. It has no external addon
dependencies and does not perform movement or combat; it sends requests to this server module. Its main, Activity, and
Help windows automatically scale to fit the current display resolution and UI scale.

## Commands

These commands always farm with the character you are currently playing. Autofarm enables selfbot after validating the
material and route, then disables that automatically enabled selfbot when farming stops:

```text
.autofarm start copper ore
.autofarm start copper ore --count 200
.autofarm start 2770 --count 200
.autofarm start <shift-clicked item link> --count 200
.autofarm status
.autofarm stop
```

For a bot that is no longer selected or for multiple simultaneous farming characters:

```text
.autofarm startbot CharacterName frostweave cloth --count 400
.autofarm status CharacterName
.autofarm stop CharacterName
.autofarm stopall
```

Find item IDs when a partial name is ambiguous:

```text
.autofarm search saronite
```

The quantity is the number newly collected during that session. Omit `--count` or use zero for an unlimited session.

Farm Vanilla mob-kill reputation with the current character or an added playerbot:

```text
.autofarm rep Argent Dawn
.autofarm rep Timbermaw Hold --standing revered
.autofarm repbot CharacterName Cenarion Circle --standing exalted
.autofarm repsearch argent
```

When `--standing` is omitted, the module targets the highest standing supported by an eligible mob for that faction.
Explicit standing names are `hated`, `hostile`, `unfriendly`, `neutral`, `friendly`, `honored`, `revered`, and
`exalted`. Existing `.autofarm status`, `.autofarm stop`, and `.autofarm stopall` commands work for both material and
reputation sessions.

The Vanilla reputation mode deliberately starts with outdoor Eastern Kingdoms and Kalimdor kill rewards. It does not
enter instances, complete quests, use repeatable turn-ins, or farm a reputation whose only source is one of those
activities. A normally friendly target must already be attackable, such as by marking its faction At War.

## Farming behavior

- Every session chooses exactly one zone and remains there. Candidate zones are ranked by usable source density,
  average distance between nearby sources, terrain grade, total elevation range, and overall zone spread, with a small
  faction-location preference. The route therefore favors a flatter, well-connected zone.
- The selected route patrols every known possible source spawn in that zone, including inactive members of rotating
  spawn pools. When a zone exceeds `Autofarm.MaxRoutePoints`, points are spread across the whole zone instead of being
  taken only from its center. The route is then optimized as a closed loop to avoid a long straight return leg.
- Ore and herbs: routes through nodes that directly contain the requested item. Herb targets use only Herbalism
  gameobjects, so herbable creatures and ordinary creature drops cannot displace skill-granting nodes from the route.
- Cloth and meat: routes through suitable normal creatures whose corpse loot contains the requested item.
- Leather and scales: routes through suitable skinnable creatures.
- Elemental mining and other gatherable raw items: uses the creature's required loot skill. Herbalism target routes
  deliberately exclude creature sources so the character gains skill from herb nodes.
- Fishing schools and other usable outdoor gameobjects: supported when the item is in the object's loot template.
- Incidental resources: while targeting Peacebloom or copper, any usable nearby mining/herbalism node such as Silverleaf
  or tin is still gathered. Herbalism routes also fill unused route slots with other usable herb nodes in the selected
  zone, prioritizing those closest to target-item nodes. The selected material controls the zone and quantity goal,
  not whether another farmable nearby node is gathered.
- Mining and herbalism node routes temporarily make the bot and its active pet immune to NPC and player-controlled
  combat. The character remains visible to players and can continue interacting with nodes.
- Creature corpses are fully looted and skinned when the character has the required profession.
- Reputation routes attack level-appropriate non-world-boss creatures whose direct kill reward can reach the requested
  standing. Elite targets are supported because some reputations require them. The session stops as soon as that
  standing is reached and does not require free inventory space.
- Combat, attackers encountered on the route, death, and recovery are handled by the bot's normal class AI.
- Food, drink, random grinding, and repetitive non-combat buffing are disabled during autofarm. Health and mana are
  restored out of combat, and all affected playerbot strategies return to their original state when farming stops.
- Active sessions temporarily force the bot out of playerbots' passive activity rotation, clear the AFK flag, and
  periodically refresh the server activity timeout at no more than half the active socket timeout. Any prior playerbots
  master is restored when farming stops.
- Inactive mining/herbalism pool members remain known to the route because they are possible future spawn locations,
  but are skipped before travel until the pool activates them.
- When normal ground movement and forced path recovery both fail, autofarm may teleport near the source, but only to a
  landing point validated against terrain, water, elevation, and the navmesh path to the source. It never uses raw spawn
  coordinates for recovery. If no safe landing can be proven, autofarm tests other active sources and quarantines the
  unreachable point for the remainder of that session instead of retrying it on every route loop.
- Long ground routes use navmesh corner waypoints as intermediate recovery steps, allowing the bot to follow roads and
  passes around hills instead of treating the direct line to a distant node as the only possible movement.
- During forced ground recovery and near a node, autofarm temporarily owns travel and mounting so the normal travel
  strategy cannot reverse a navmesh detour and loot cannot fight a simultaneous remount. Unsafe ground movement returns
  the bot to its last valid position before it can fall through terrain.
- In Outland and Northrend, a bot with usable flying automatically takes off, cruises above sampled terrain, and lands
  at the selected source. Unrelated attackers do not make it dismount; if it is forced off the mount, its original
  combat strategies are restored so it can defend itself.

The route is generated from the server's current world data. It therefore follows custom spawns and custom loot without
requiring a module update.

## Safety and limits

- Only the bot's playerbot master may start or stop it; game masters may administer any online playerbot.
- Starting on the currently played character automatically enables selfbot. Autofarm disables it on stop only when the
  module enabled it; a selfbot that was already enabled manually is left enabled.
- Battlegrounds, arenas, instances, taxis, combat starts, dead starts, and teleports already in progress are rejected.
- Material-route creature targets are limited to normal creatures at a configurable level range. Reputation routes
  may include elite creatures but never world bosses.
- Instance maps are never selected, even if added to `Autofarm.AllowedMaps`.
- The selected item is temporarily forced into the playerbot always-loot list.
- Travel, loot, gather, grind, food, and buff strategies are restored to their previous state on stop.
- NPC/player immunity and combat suppression are limited to routes made entirely from mining/herbalism gameobjects and
  are removed on stop unless the bot or pet already had the relevant immunity.
- Sessions are in memory. A worldserver restart stops all farming sessions.
- Crafted-only items, vendor items, open-water fishing drops, and items found only inside containers are not direct farm
  sources in the first version.
- AzerothCore stores grouped loot entries privately. Normal crafting materials are generally ordinary or referenced
  loot rows and are supported; an item present only in a grouped loot row may not be discovered.

## Configuration

Copy `conf/mod_autofarm.conf.dist` to the installed module configuration directory as `mod_autofarm.conf`. Important
options control teleport/return behavior, allowed maps, zone scoring, route size, creature-level tolerance, timeouts,
rest/buff suppression, passive node gathering, session keepalive, flying travel height and escape timing, and debug
logging.

No SQL updates or core patches are required.
