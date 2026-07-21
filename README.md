# mod-autofarm

`mod-autofarm` lets an owned playerbot farm a selected raw material without hardcoded zone or item lists. It uses the
live AzerothCore item, loot, creature, gameobject, and spawn stores to locate outdoor sources, chooses a productive
faction-aware cluster, teleports to the route, and lets `mod-playerbots` handle movement, combat, looting, gathering,
skinning, death recovery, food, and class rotations.

## Requirements

- AzerothCore WotLK
- `mod-playerbots`, enabled and configured to let the account add its own characters
- A character logged in through playerbots, normally with `.playerbots bot add <name>`
- The required gathering profession and skill for ore, herbs, leather, or profession-skinned elementals
- Required tools such as a mining pick or skinning knife

This module deliberately uses playerbots for combat. Azerocombat is a client-side addon and cannot control a character
that is logged in by the server as a playerbot, so it is neither required nor used.

## Optional client addon

The WotLK 3.3.5a `AzerothAutofarm` addon in `client/AzerothAutofarm/` provides a standalone graphical control panel for
the commands below. It includes material presets, custom item/link input, favorites, quantity goals, bot selection,
session controls, an activity log, help, and a minimap button. It has no external addon dependencies and does not
perform movement or combat; it sends requests to this server module. Its main, Activity, and Help windows automatically
scale to fit the current display resolution and UI scale.

## Commands

Select an owned playerbot, then use:

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

## Farming behavior

- Ore and herbs: routes through nodes that directly contain the requested item.
- Cloth and meat: routes through suitable normal creatures whose corpse loot contains the requested item.
- Leather and scales: routes through suitable skinnable creatures.
- Elemental mining/herbalism and other gatherable raw items: uses the creature's required loot skill.
- Fishing schools and other usable outdoor gameobjects: supported when the item is in the object's loot template.
- Incidental resources: while targeting copper, any usable nearby mining/herbalism node such as tin is still gathered.
- Creature corpses are fully looted and skinned when the character has the required profession.
- Combat, attackers encountered on the route, death, and recovery are handled by the bot's normal class AI.
- In Outland and Northrend, a bot with usable flying automatically takes off, cruises above sampled terrain, and lands
  at the selected source. Unrelated attackers do not make it dismount; if it is forced off the mount, its original
  combat strategies are restored so it can defend itself.

The route is generated from the server's current world data. It therefore follows custom spawns and custom loot without
requiring a module update.

## Safety and limits

- Only the bot's playerbot master may start or stop it; game masters may administer any online playerbot.
- Battlegrounds, arenas, instances, taxis, combat starts, dead starts, and teleports already in progress are rejected.
- Deliberate creature targets are limited to normal creatures at a configurable level range.
- Instance maps are never selected, even if added to `Autofarm.AllowedMaps`.
- The selected item is temporarily forced into the playerbot always-loot list.
- Travel, loot, gather, and grind strategies are restored to their previous state on stop.
- Sessions are in memory. A worldserver restart stops all farming sessions.
- Crafted-only items, vendor items, open-water fishing drops, and items found only inside containers are not direct farm
  sources in the first version.
- AzerothCore stores grouped loot entries privately. Normal crafting materials are generally ordinary or referenced
  loot rows and are supported; an item present only in a grouped loot row may not be discovered.

## Configuration

Copy `conf/mod_autofarm.conf.dist` to the installed module configuration directory as `mod_autofarm.conf`. Important
options control teleport/return behavior, allowed maps, cluster size, route size, creature-level tolerance, timeouts,
flying travel height and escape timing, and debug logging.

No SQL updates or core patches are required.
