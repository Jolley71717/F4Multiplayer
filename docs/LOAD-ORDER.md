# Load order: what has to match, and what no longer does

## Why everything is keyed by form ID

Everything the session shares is named by its form ID: NPCs, items, containers, doors, map
markers, quests, workshop pieces. A form ID's top byte is the plugin's slot in the load order, so
"same ID, same thing" only holds when everyone loads the same plugins in the same order. With a
different order, a friend's "raider 0004B2BA picked up 0001F276" lands on whatever those IDs mean
in your game, or on nothing. Since protocol 36 the wire does not carry a game's own IDs: it carries
the session's, and each game translates (DESIGN.md, *Form IDs across load orders*).

## What is checked today

At startup the plugin hashes the names of the loaded plugins, in order (FNV-1a, 32 bits), skipping
the free HD texture pack when it is the last full plugin, and sends the hash and the list itself
(full plugins, a mark, light plugins; up to 512 names) in its hello.

- **Since protocol 36 any load order gets in.** The server keeps the session's plugin table (the
  host's load order, with each later joiner's other plugins added after the rest of their kind),
  sends it before each welcome and to everyone when it grows, and each game translates every form ID
  it sends and gets by plugin file name (common/FormIds.h, common/FormTranslate.h). A reorder, a
  light plugin in another place, or a plugin light in one game and full in the other changes nothing.
- **A plugin only one game has** cannot be shared: what comes from it is skipped in the other game,
  never guessed (a message about its form is dropped, a list entry left out, a detail set to none),
  and each game's log names the plugin once.
- **The joiner is told.** The server sends a joiner whose list differs the host's list with the
  host's `sIgnoredPlugins` (`kLoadOrder`, protocol 35) before the welcome; their game names every
  plugin only one of them has on the HUD, on the F1 menu's *Compare mods* page and in the log
  (src/net/ModDiff.h), and lists the moved ones as nothing to change.
- **A hello without a list** (a bot, a test tool), or a session whose first player sent none, still
  needs the session's hash: with no lists there is nothing to translate by. Such a joiner is refused
  as before, and their game names what differs.
- **The saved session** keeps its plugin table (session file layout 33), so the host carries on with any mods. A
  file saved by 0.8.11 or older has none, and carries on only with the load order its hash names.

Nothing else is compared: not plugin versions, not file contents, not F4SE plugins, not INI settings.

## Options, from cheapest to hardest

| Option | What players get | Status |
|---|---|---|
| **A. Keep the strict check** | A clear refusal with a reason | replaced by E (still the rule for hellos without a list) |
| **B. Ignore plugins that add no records that are shared** | Texture/mesh/sound/UI mods no longer count | the HD texture pack when last; others through D |
| **C. Show the difference instead of a hash** | The joining game names the plugins that differ | done (protocols 23 and 35) |
| **D. Host-decided allow list** (`sIgnoredPlugins` in the ini) | The host says which plugins nobody needs to hear about | done (protocol 23) |
| **E. Translate form IDs by plugin name** | Different *orders* of the same plugins work | done (protocol 36): a per-session plugin table, translated at each game's edge; every message type has its case (the build fails without one) |
| **F. One-sided mods: a plugin only one player has** | Anything goes, with holes | the skip half of it (protocol 36): a form the other game lacks is skipped there, not guessed. The holes are real: a friend fights a raider you can't see, or loots an item you can't receive |

## What is left

- A host is not told which plugins a joiner lacks: only the joiner's game compares (the host's log has
  "a joiner's load order differs").
- The table holds at most 254 full and 4096 light plugins, and 511 names in all: a joiner's plugin past
  that has no ID in the session.
- Versions of the same plugin are not compared (below).

## Version mismatches of the same plugin

Two players with different versions of the same plugin pass the check, and its forms translate as one
plugin's. If that becomes a real problem, a hash of each plugin's size and modification time (not
contents, which would be slow) could be added to the hello's list.
