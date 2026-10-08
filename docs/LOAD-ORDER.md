# Load order: why it must match, and what could be relaxed

## Why everything is keyed by form ID

Everything the session shares is named by its form ID: NPCs, items, containers, doors, map
markers, quests, workshop pieces. A form ID's top byte is the plugin's slot in the load order, so
"same ID, same thing" only holds when everyone loads the same plugins in the same order. With a
different order, a friend's "raider 0004B2BA picked up 0001F276" lands on whatever those IDs mean
in your game, or on nothing.

## What is checked today

At startup the plugin hashes the names of the loaded plugins, in order (FNV-1a, 32 bits), skipping
the free HD texture pack. The hash travels in the hello message. The server keeps the first
player's hash; a later hello with a different one is refused, and the refusal names the plugins only
the joiner has and the ones they are missing (the plugin list travels with the hello; protocol 23),
or says the order differs. Nothing else is compared: not plugin versions, not file contents, not F4SE
plugins, not INI settings.

## Options, from cheapest to hardest

| Option | What players get | Cost | Risk |
|---|---|---|---|
| **A. Keep the strict check** (today) | A clear refusal with a reason | none | Friends with one extra texture or sound mod can't play together until they match lists |
| **B. Ignore plugins that add no records that are shared** | Texture/mesh/sound/UI mods and ESL-flagged "cosmetic" plugins no longer count | small: a list of known-safe plugin names, plus a rule for plugins whose records are only in types we never share (TXST, SNDR, MUSC, INNR...) | A plugin that looks cosmetic but edits an NPC or a leveled list would desync that NPC |
| **C. Show the difference instead of a hash** | The refusal names the plugins that differ ("you have X, they don't") | small: send the plugin name list (a few KB) with the hello | none; purely better messaging |
| **D. Host-decided allow list** (`sIgnoredPlugins` in the ini) | The host says which plugins may differ | small | The host is responsible; a wrong entry gives silent desyncs rather than a refusal |
| **E. Translate form IDs by plugin name** | Different *orders* of the same plugins work | medium: send (plugin name, local ID) pairs instead of raw IDs, or send the plugin list once and remap at both ends | ESL/light plugins and runtime (0xFF) IDs need their own handling; one missed path (there are about 25 message types with IDs) desyncs quietly |
| **F. One-sided mods: a plugin only one player has** | Anything goes | large: every shared reference would need a fallback when the base form doesn't exist locally ("a mod we don't have" already skips workshop copies; NPC gear, loot, spawned NPCs, quests would all need the same) | Visible holes: a friend fights a raider you can't see, loots an item you can't receive, builds a wall that isn't there. This is the "works on one side but not the other" case, and it can't be made consistent, only tolerated |

## Recommendation

C is done (protocol 23). Next B with a conservative built-in list (the
official HD pack is already there; add the common texture and audio replacers, and any plugin whose
record types are all outside what the mod shares) and D so hosts can extend it. Leave E and F out:
E is a large, silent-failure change for a problem that matching load orders solves, and F cannot
be made consistent, so it would become the main source of bug reports.

## Version mismatches of the same plugin

Two players with different versions of the same plugin pass the check. If that becomes a real
problem, a hash of each plugin's size and modification time (not contents, which would be slow)
could be added to the hello at the same cost as C.
