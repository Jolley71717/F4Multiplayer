#pragma once

#include "game/FoundDead.h"

#include "Protocol.h"
#include "TokenBucket.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

// What a game believes of another player's changes to its world. The server only passes on what each
// player says (it can't see anyone's world), so each game judges what arrives by what it can see: its
// own copy of the thing, and where the sender last said they were. A friend's game that misbehaves (or
// is modified) can then change only what an honest game could have: near where its player stands, and
// not another player's story. Kept free of the game so the rules can be tested.
namespace RemoteEdits
{
	// Where someone or something is: an interior cell (worldspace 0), or a worldspace (cell 0), and where in it.
	struct Spot
	{
		std::uint32_t cell = 0;
		std::uint32_t worldspace = 0;
		float         x = 0.0f, y = 0.0f, z = 0.0f;
	};

	inline Spot SpotOf(const Protocol::PlayerState& a_state)
	{
		return { a_state.cell, a_state.worldspace, a_state.x, a_state.y, a_state.z };
	}

	inline Spot SpotOf(const Protocol::ActorState& a_state)
	{
		return { a_state.cell, a_state.worldspace, a_state.x, a_state.y, a_state.z };
	}

	constexpr bool SameSpace(const Spot& a_a, const Spot& a_b)
	{
		return a_a.cell == a_b.cell && a_a.worldspace == a_b.worldspace;
	}

	// How far apart, in game units; NaN when they are in different spaces (coordinates in one cell are
	// nowhere in another), so every range test below fails for them.
	inline float Distance(const Spot& a_a, const Spot& a_b)
	{
		if (!SameSpace(a_a, a_b)) {
			return std::numeric_limits<float>::quiet_NaN();
		}
		const float dx = a_a.x - a_b.x, dy = a_a.y - a_b.y, dz = a_a.z - a_b.z;
		return std::sqrt(dx * dx + dy * dy + dz * dz);
	}

	// Written so that NaN and infinity fail (a comparison with NaN is always false).
	constexpr bool Within(float a_distance, float a_range)
	{
		return a_distance >= 0.0f && a_distance <= a_range;
	}

	// ---- Story characters (WorldSync, NpcSync) ---------------------------------------------------
	//
	// With everyone on their own story, a character of one player's story is not another player's to
	// change: a friend's game switching one on, or killing one away from us, says nothing about ours.
	// The same rule FoundDead::Shared applies to the host's save.

	// A friend's game runs an NPC that is switched off in ours: switched on here too, unless it is a
	// character of our own story (a companion or quest character our story has not brought in yet).
	constexpr bool MaySwitchOn(bool a_sharedStory, const FoundDead::Npc& a_npc)
	{
		return a_sharedStory || !FoundDead::IsStoryCharacter(a_npc);
	}

	// A friend's game killed an NPC that is switched off in ours (it switched it on and it died before any state of it
	// reached us): ours is switched on to be killed, once its cell is drawn (until then there is nothing to see), and
	// under the same story rule. Off, it never loads, and the kill waited for good: no corpse, no loot.
	constexpr bool SwitchOnToKill(bool a_off, bool a_deleted, bool a_cellDrawn, bool a_sharedStory, const FoundDead::Npc& a_npc)
	{
		return a_off && !a_deleted && a_cellDrawn && MaySwitchOn(a_sharedStory, a_npc);
	}

	// A friend's companion is moved to where their game says it is (from a cell we haven't loaded, or after
	// its owner's copy ran off) only while that is beside the friend's own stand-in: a companion follows its
	// player. Its uniqueness is no reason to leave it behind (every vanilla companion is unique).
	constexpr float OWNER_NEAR = 1500.0f;

	constexpr bool OwnerNear(float a_distance)
	{
		return Within(a_distance, OWNER_NEAR);
	}

	// a_owner: where its owner last said they were (nothing if never heard from).
	inline bool OwnerNear(const std::optional<Spot>& a_owner, const Spot& a_spot)
	{
		return a_owner && OwnerNear(Distance(*a_owner, a_spot));
	}

	// Whether the owner's stand-in stands beside where the owner's game has the NPC, by what our game has heard:
	// a_owner runs it (as the server last said), a_lastState(player) is the last state that player sent us.
	template <class LastState>
	bool OwnerBeside(LastState&& a_lastState, std::uint32_t a_owner, const Protocol::ActorState& a_npc)
	{
		const std::optional<Protocol::PlayerState> owner = a_owner ? a_lastState(a_owner) : std::nullopt;
		return owner && OwnerNear(Distance(SpotOf(*owner), SpotOf(a_npc)));
	}

	// A death the reporter could have seen: they stood in the actor's cell or worldspace, this close to
	// our copy. (Measured from our copy, not from where the reporter says the corpse lies: that comes
	// from them too.)
	constexpr float DEATH_WITNESS_RANGE = 8192.0f;

	constexpr bool Witnessed(bool a_sameSpace, float a_distance)
	{
		return a_sameSpace && Within(a_distance, DEATH_WITNESS_RANGE);
	}

	// a_reporter: where the reporter was (nothing if unknown, as for a death replayed from the session).
	inline bool Witnessed(const std::optional<Spot>& a_reporter, const Spot& a_actor)
	{
		return a_reporter && Witnessed(SameSpace(*a_reporter, a_actor), Distance(*a_reporter, a_actor));
	}

	// Whether a death another player reported kills our copy. Anyone else dies; a unique or essential
	// character of our own story only when the reporter was there. Being in a quest's alias is no reason
	// to keep one alive: radiant quests put generic raiders in aliases.
	constexpr bool MayKill(bool a_sharedStory, const FoundDead::Npc& a_npc, bool a_witnessed)
	{
		return a_sharedStory || !(a_npc.unique || a_npc.essential) || a_witnessed;
	}

	// The same, for our copy standing at a_actor: a_reporterThen is where the reporter was when the report
	// came, a_reporterNow where they are now (their state can come after the report; either may be unknown).
	inline bool KillsOurCopy(bool a_sharedStory, const FoundDead::Npc& a_npc, const std::optional<Spot>& a_reporterThen, const std::optional<Spot>& a_reporterNow,
		const Spot& a_actor)
	{
		return MayKill(a_sharedStory, a_npc, Witnessed(a_reporterThen, a_actor) || Witnessed(a_reporterNow, a_actor));
	}

	// The corpse is put where it fell in the reporter's game only from this close; farther, it dies where
	// ours stands (a report can't pull an NPC across the map).
	constexpr float CORPSE_MOVE_RANGE = 4096.0f;

	constexpr bool MayMoveCorpse(float a_distance)
	{
		return Within(a_distance, CORPSE_MOVE_RANGE);
	}

	// ---- Explosions (WeaponFire) ----------------------------------------------------------------
	//
	// Who sets off an explosion, and who deals its blows (docs/DESIGN.md, "Who sets off an explosion"): every explosion
	// goes off once in every game that has a player or an NPC it runs near it, and each game deals the blows on its own:
	// its player, and the NPCs it runs. A copy's blow (of an NPC another game runs) is given back, as that NPC's own game
	// deals it. An explosion a game cannot be sure goes off in the others' games is not shared at all: its own game deals
	// every blow, a copy's too (reported as our hit's damage, like a gunshot's).
	//
	// A friend's explosion is set off in our game where their game says it went off. It is what does the
	// damage here: the stand-in's own grenade is removed and its explosive shot held, a death their game reports for an
	// NPC ours runs is not taken, and no health comes for one already dead in theirs. So it goes off whoever stands
	// beside it, and with friendly fire off too: the enemies next to us or our companion die here as they did for the
	// thrower, and a grenade at our feet hurts us as the thrower's own would have. Our copies of NPCs another game runs
	// are the exception: made ghosts while it goes off (BlastShield), as their owner's game deals its damage to them.
	//
	// Only what a throw or a shot makes is set off. A grenade is about 500 units across, a Fat Man's shell 850, and what
	// a shot sets off no more (Fallout4.esm's explosions, read from the records: a car's first blast 128, its big one
	// 900, a vehicle's small or large alike; an explosive barrel 350, a gas canister 120); nothing bigger is thrown or
	// shot (a nuke or an artillery strike reaches thousands), and a radius of 0 or one that isn't a number is no
	// explosion of a throw.
	constexpr float MAX_EXPLOSION_RADIUS = 1024.0f;
	constexpr float EXPLOSION_RANGE = 6000.0f;  // from us: farther, nothing we'd see or feel
	// From an NPC our game runs: its blow is ours to deal, wherever our player is (the reporter left it to us). An NPC
	// within the radius and this of it is found, give or take where the reporter's copy of it stood.
	constexpr float OURS_NEAR_MARGIN = 1024.0f;

	inline Spot SpotOf(const Protocol::Explosion& a_explosion)
	{
		return { a_explosion.cell, a_explosion.worldspace, a_explosion.x, a_explosion.y, a_explosion.z };
	}

	enum class Blast
	{
		kSetOff,
		kAway,      // in another space than ours, or too far to matter, with no NPC of ours beside it
		kNotThrown, // no throw makes an explosion that size
	};

	constexpr bool Throwable(float a_radius)
	{
		return a_radius > 0.0f && a_radius <= MAX_EXPLOSION_RADIUS;
	}

	// a_at: where the friend's game says it went off; a_us: where we stand; a_radius: the explosion record's,
	// as our game has it; a_oursNear: an NPC our game runs is within its reach (a_radius + OURS_NEAR_MARGIN), in our
	// space. The reporter left that NPC's blow to us however far our player is: it goes off for it.
	inline Blast SetOff(const Spot& a_at, const Spot& a_us, float a_radius, bool a_oursNear = false)
	{
		if (!Within(Distance(a_at, a_us), EXPLOSION_RANGE) && !(a_oursNear && SameSpace(a_at, a_us))) {
			return Blast::kAway;
		}
		if (!Throwable(a_radius)) {
			return Blast::kNotThrown;
		}
		return Blast::kSetOff;
	}

	// Whether our game sends an explosion our player (or an NPC we run) set off: only when the server is sure to pass it
	// on, so that the blows it leaves to the NPCs' own games are dealt there. The server's numbers (Protocol's
	// REPORTED_EXPLOSION_*) less a margin: our position as the server has it is a state or two old, and a burst of
	// reports can reach it closer together than they left (half its budget is two seconds of that).
	class ReportGate
	{
	public:
		using Clock = std::chrono::steady_clock;
		static constexpr float  RANGE = Protocol::REPORTED_EXPLOSION_RANGE - 1000.0f;
		static constexpr double BURST = Protocol::REPORTED_EXPLOSION_BURST / 2.0;
		static constexpr double PER_SECOND = Protocol::REPORTED_EXPLOSIONS_PER_SECOND * 0.75;

		// a_distance: from our player; a_radius: the explosion record's. Spends from the budget when it passes.
		bool Pass(float a_distance, float a_radius, Clock::time_point a_now)
		{
			if (!Within(a_distance, RANGE) || !Throwable(a_radius)) {
				return false;
			}
			return budget.Take(a_now);
		}

	private:
		TokenBucket budget{ BURST, PER_SECOND };
	};

	// Each thrower's last throw (our player, an NPC we run, our companion): until when its watch is on, and whether every
	// explosion it decided so far was reported. The fire a Molotov leaves burning is judged by its own thrower's
	// (CopyHealth::OwnersToDeal): a settler of ours throwing a grenade never marks our player's kept Molotov as shared.
	class ThrowWatches
	{
	public:
		using Clock = std::chrono::steady_clock;
		static constexpr std::size_t MAX = 16;

		void Arm(std::uint32_t a_thrower, Clock::time_point a_until)
		{
			if (const auto it = Find(a_thrower); it != entries.end()) {
				it->until = a_until;
				it->armed = ++armedCount;
				it->shared = true;
				return;
			}
			if (entries.size() >= MAX) {
				entries.erase(std::ranges::min_element(entries, {}, &Entry::until));
			}
			entries.push_back({ a_thrower, a_until, ++armedCount, true });
		}

		// One of a_thrower's explosions was kept ours (0: an explosion that named none; every watch on then).
		void Unshare(std::uint32_t a_thrower, Clock::time_point a_now)
		{
			for (auto& entry : entries) {
				if ((a_thrower == 0 || entry.thrower == a_thrower) && a_now <= entry.until) {
					entry.shared = false;
				}
			}
		}

		// Whether a_thrower's watch is on (with a_after: or was until a_after ago) and every explosion of it was
		// reported. a_thrower 0 (a hit that names nobody): the last to throw's.
		[[nodiscard]] bool Shared(std::uint32_t a_thrower, Clock::time_point a_now, Clock::duration a_after = Clock::duration::zero()) const
		{
			const Entry* entry = nullptr;
			if (a_thrower != 0) {
				const auto it = std::ranges::find(entries, a_thrower, &Entry::thrower);
				entry = it != entries.end() ? &*it : nullptr;
			} else if (!entries.empty()) {
				entry = &*std::ranges::max_element(entries, {}, &Entry::armed);
			}
			return entry && a_now <= entry->until + a_after && entry->shared;
		}

	private:
		struct Entry
		{
			std::uint32_t     thrower = 0;
			Clock::time_point until{};
			std::uint64_t     armed = 0;  // the order they were armed in
			bool              shared = true;
		};

		std::vector<Entry>::iterator Find(std::uint32_t a_thrower) { return std::ranges::find(entries, a_thrower, &Entry::thrower); }

		std::vector<Entry> entries;
		std::uint64_t      armedCount = 0;
	};

	// What became of each explosion our game saw go off, as WeaponFire decided it once (BlastLog), and so whether the game
	// that runs an NPC deals that explosion's blow on our copy of it itself (CopyHealth::OwnersToDeal): exactly one game
	// deals each blow. The decision is the explosion's, made where it is reported or not, never the hit's: a hit judged by
	// a watch of its own, while the explosion went unreported (out of the scan's range, or at the watch's very end), was
	// dealt in neither game, and the owner's next health report healed the copy back.
	enum class Fate : std::uint8_t
	{
		kOurs,     // not shared: it goes off in no other game, so its blows on copies are ours to report
		kShared,   // ours, reported (ReportGate passed it): each friend's game near it sets it off once
		kEachOwn,  // a world trap's, or a destructible object's own (its stage's): every game sets its own off
	};

	struct Seen
	{
		Spot                                  at;
		float                                 radius = 0.0f;
		Fate                                  fate = Fate::kOurs;
		std::chrono::steady_clock::time_point when{};
		std::uint32_t                         source = 0;  // the explosion reference's form id (0: not known)
	};

	// Whether the game that runs an NPC deals the blow this explosion dealt our copy of it: it goes off there too (a
	// reported one beside that game's NPC wherever its player is: SetOff's a_oursNear).
	constexpr bool DealtThere(const Seen& a_seen)
	{
		return a_seen.fate != Fate::kOurs;
	}

	// The explosions our game saw lately and what became of each (main thread).
	class BlastLog
	{
	public:
		using Clock = std::chrono::steady_clock;
		static constexpr std::size_t MAX = 64;
		static constexpr auto        KEPT = std::chrono::seconds(3);  // an explosion's blows land in the frames after it
		static constexpr float       REACH = 256.0f;                 // a blow lands on an actor within its radius, give or take the actor's size

		void Note(const Seen& a_seen)
		{
			if (seen.size() >= MAX) {
				seen.erase(seen.begin());
			}
			seen.push_back(a_seen);
		}

		// The explosion a blow on an actor at a_at came from, seen within KEPT before a_now: the one the hit names
		// (a_source, the explosion reference in its HitData), else the nearest that reaches it. When two reach it that
		// fared differently and the hit names neither, which dealt it is not known: the blow is taken as ours (its fate
		// read kOurs), dealt here and reported, and a_ambiguous is set.
		[[nodiscard]] std::optional<Seen> At(const Spot& a_at, Clock::time_point a_now, std::uint32_t a_source = 0, bool* a_ambiguous = nullptr) const
		{
			if (a_ambiguous) {
				*a_ambiguous = false;
			}
			std::optional<Seen> best;
			float               bestDistance = std::numeric_limits<float>::max();
			bool                mixed = false;
			for (const auto& entry : seen) {
				if (a_now - entry.when > KEPT) {
					continue;
				}
				if (a_source != 0 && entry.source == a_source) {
					return entry;
				}
				const float distance = Distance(entry.at, a_at);
				if (!Within(distance, entry.radius + REACH)) {
					continue;
				}
				if (best && DealtThere(*best) != DealtThere(entry)) {
					mixed = true;
				}
				if (distance < bestDistance) {
					best = entry;
					bestDistance = distance;
				}
			}
			if (best && mixed) {
				best->fate = Fate::kOurs;
				if (a_ambiguous) {
					*a_ambiguous = true;
				}
			}
			return best;
		}

		void Prune(Clock::time_point a_now)
		{
			std::erase_if(seen, [&](const Seen& a_entry) { return a_now - a_entry.when > KEPT; });
		}

		[[nodiscard]] std::size_t Size() const { return seen.size(); }
		void Clear() { seen.clear(); }

	private:
		std::vector<Seen> seen;
	};

	// Where a hit of ours on a destructible object may set off an explosion that isn't the object's own stage's (a
	// mine's) for a while (Destructibles): only an explosion within that object's blast radius is reported for it, not
	// every one around our player (a raider's grenade there went off twice in each friend's game).
	class AreaWatches
	{
	public:
		using Clock = std::chrono::steady_clock;
		static constexpr std::size_t MAX = 16;

		void Arm(const Spot& a_at, float a_radius, Clock::time_point a_until)
		{
			if (!(a_radius > 0.0f)) {
				return;
			}
			for (auto& watch : watches) {
				if (SameSpace(watch.at, a_at) && Within(Distance(watch.at, a_at), 1.0f)) {
					watch.radius = (std::max)(watch.radius, a_radius);
					watch.until = (std::max)(watch.until, a_until);
					return;
				}
			}
			if (watches.size() >= MAX) {
				watches.erase(std::ranges::min_element(watches, {}, &Watch::until));
			}
			watches.push_back({ a_at, a_radius, a_until });
		}

		[[nodiscard]] bool Covers(const Spot& a_at, Clock::time_point a_now) const
		{
			return std::ranges::any_of(watches, [&](const Watch& a_watch) { return a_now <= a_watch.until && Within(Distance(a_watch.at, a_at), a_watch.radius); });
		}

		[[nodiscard]] bool Any(Clock::time_point a_now) const
		{
			return std::ranges::any_of(watches, [&](const Watch& a_watch) { return a_now <= a_watch.until; });
		}

		void Prune(Clock::time_point a_now)
		{
			std::erase_if(watches, [&](const Watch& a_watch) { return a_now > a_watch.until; });
		}

		[[nodiscard]] std::size_t Size() const { return watches.size(); }
		void Clear() { watches.clear(); }

	private:
		struct Watch
		{
			Spot              at;
			float             radius = 0.0f;
			Clock::time_point until{};
		};
		std::vector<Watch> watches;
	};

	// ---- Workshop (WorkshopSync) ----------------------------------------------------------------

	// A friend's build is shown at most this much smaller or bigger than the game's own size: what the
	// workshop makes is never far off 1, and a wall a hundred times its size covers a town.
	constexpr float MIN_SCALE = 0.1f;
	constexpr float MAX_SCALE = 10.0f;

	constexpr float ClampScale(float a_scale)
	{
		if (!(a_scale == a_scale)) {
			return 1.0f;
		}
		return a_scale < MIN_SCALE ? MIN_SCALE : (a_scale > MAX_SCALE ? MAX_SCALE : a_scale);
	}

	// The size our copy of a friend's build is set to, when it is to change: a_current is our copy's now (1 for
	// one just placed). Nothing when it is that size already.
	inline std::optional<float> ScaleToApply(const Protocol::WorkshopItem& a_item, float a_current)
	{
		const float scale = ClampScale(a_item.scale);
		return std::abs(a_current - scale) > 0.01f ? std::optional{ scale } : std::nullopt;
	}

	// A workbench's build area, as the game knows it here: volumes linked to the bench, loaded or not, or
	// none at all.
	enum class BuildArea
	{
		kNoVolumes,
		kNotLoaded,     // none of them
		kPartlyLoaded,  // some of them (a settlement's area can be several volumes, in different cells)
		kLoaded,        // all of them
	};

	enum class Verdict
	{
		kApply,
		kWait,    // tried again when a cell loads
		kRefuse,  // for good
	};

	// How far a build area reaches from a workbench that has no volumes (the game's own fallback).
	constexpr float BUILD_AREA_RADIUS = 5000.0f;

	// Whether a friend's scrap of a piece of our world falls in its workbench's build area. a_inArea: the
	// game's own test with the volumes that are loaded. While some of its volumes aren't loaded, a scrap not in
	// the loaded ones waits (it may lie in another) rather than being refused or let through by a guess.
	constexpr Verdict ScrapReach(BuildArea a_area, bool a_inArea, float a_distanceToBench)
	{
		switch (a_area) {
		case BuildArea::kLoaded:
			return a_inArea ? Verdict::kApply : Verdict::kRefuse;
		case BuildArea::kPartlyLoaded:
			return a_inArea ? Verdict::kApply : Verdict::kWait;
		case BuildArea::kNotLoaded:
			return Verdict::kWait;
		default:
			return Within(a_distanceToBench, BUILD_AREA_RADIUS) ? Verdict::kApply : Verdict::kRefuse;
		}
	}

	// ---- A friend's look (RemotePlayers) --------------------------------------------------------

	// The default stand-in bases: settlers with a fixed look and no factions, one per sex.
	constexpr std::uint32_t FEMALE_SETTLER = 0x0020A578;
	constexpr std::uint32_t MALE_SETTLER = 0x0020A57B;

	// What a friend's stand-in may be placed from: the two settlers, or what our own ini picked for
	// stand-ins. Any other NPC brings its own scripts and factions into our world on a friend's word
	// (a unique one its quests, but plain ones too: a settler that runs workshop scripts, one that
	// teleports), so a friend asking for one is shown as our default.
	constexpr bool AllowedAppearance(std::uint32_t a_base, std::uint32_t a_localDefault)
	{
		return a_base == FEMALE_SETTLER || a_base == MALE_SETTLER || a_base == a_localDefault;
	}

	// The look a friend's stand-in is placed with: the one they asked for (Hello.appearance, passed on as they
	// sent it) when it is allowed, else 0: our default.
	constexpr std::uint32_t LookToPlace(std::uint32_t a_asked, std::uint32_t a_localDefault)
	{
		return a_asked && AllowedAppearance(a_asked, a_localDefault) ? a_asked : 0;
	}
}
