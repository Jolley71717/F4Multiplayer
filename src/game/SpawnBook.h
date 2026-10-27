#pragma once

#include "game/FrameShare.h"
#include "game/RemoteEdits.h"
#include "game/StandInHold.h"

#include "Protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// NPCs a game makes at runtime (a random encounter's raiders, an ambush, a settlement's attackers) have ids (0xFF......)
// that are that game's own: the same id names something else, or nothing, in every other game. So each one our game
// makes is shared under a spawn id (Protocol::IsSpawnRef) the server grants us: our game runs it for good, and every
// other game places a copy of it, which NpcSync mirrors like any NPC another game runs, by that id. Hits, deaths, health
// and shots name the spawn id on the wire; each game looks up its own NPC under it here.
//
// Power armor frames a game made at runtime are shared the same way (FrameShare::SharesMade): a spawn whose base is a
// frame, run by the game that made it, a copy of which every other game places where it stands (Theirs::DecideFrame).
//
// Two books: Ours (our NPCs and frames and their spawn ids) and Theirs (friends' spawns and the copies we place of
// them), with when a copy is placed and when it goes. Kept free of the game so it can be tested (tests/SpawnTests.cpp
// plays two games' books against a real server).
namespace SpawnBook
{
	using Clock = std::chrono::steady_clock;
	using RemoteEdits::Spot;

	// The spawn id asked for first is the NPC's own id (friends' logs then name it as ours do); while one is taken (a
	// friend's game shares one under it), others spread over the runtime range. Never 0xFF000000.
	constexpr std::uint32_t CandidateId(std::uint32_t a_local, std::uint32_t a_attempt)
	{
		if (a_attempt == 0 && Protocol::IsSpawnRef(a_local)) {
			return a_local;
		}
		std::uint32_t h = a_local * 0x9E3779B1u + a_attempt * 0x85EBCA77u;
		h ^= h >> 15;
		h *= 0x2C1B3C6Du;
		h ^= h >> 12;
		const std::uint32_t id = 0xFF000000u | (h & 0x00FFFFFFu);
		return Protocol::IsSpawnRef(id) ? id : 0xFF000001u;
	}

	// What a runtime NPC was placed from, as every game has it. A leveled NPC's own base is a copy the game made
	// (0xFF......); its leveled data names the NPCs it was put together from (a_templates, the traits' first) and the one it
	// was placed as (a_original: a leveled one, whose list each game rolls on its own). The first template that is a whole
	// NPC (no template of its own) comes out the same wherever it is placed, so it is what copies are placed from; then
	// the original; then the NPC's own base, when that is a plugin's. 0 when there is none: the NPC isn't shared.
	struct Base
	{
		std::uint32_t id = 0;
		bool          npc = false;           // an NPC (not a leveled list or anything else)
		bool          usesTemplate = false;  // its traits come from another (a leveled list's roll)
	};

	inline std::uint32_t PickBase(const Base& a_own, std::span<const Base> a_templates, const Base& a_original)
	{
		for (const auto& base : a_templates) {
			if (base.npc && !base.usesTemplate && Protocol::IsShareableRef(base.id)) {
				return base.id;
			}
		}
		if (a_original.npc && Protocol::IsShareableRef(a_original.id)) {
			return a_original.id;
		}
		return a_own.npc && Protocol::IsShareableRef(a_own.id) ? a_own.id : 0;
	}

	// ---- Ours: NPCs our game made, shared under spawn ids --------------------------------------------------------

	class Ours
	{
	public:
		static constexpr std::size_t MAX = Protocol::MAX_SPAWNS_PER_PLAYER;
		static constexpr auto        ANSWER_WAIT = std::chrono::seconds(5);    // a report never answered (dropped in a flood) is made again
		static constexpr auto        REFUSED_WAIT = std::chrono::seconds(10);  // a refused NPC is offered again after this

		// One of our runtime NPCs that is ours to share (Spawns decides which): the spawn id to report it under now, if any.
		// Not while one is reported and unanswered (for ANSWER_WAIT), nor for REFUSED_WAIT after a refusal, nor past MAX.
		// a_taken(id): a friend's game shares a spawn under that id (Theirs::Knows).
		template <class Taken>
		std::optional<std::uint32_t> Offer(std::uint32_t a_local, Clock::time_point a_now, Taken&& a_taken)
		{
			if (const auto known = byLocal.find(a_local); known != byLocal.end()) {
				auto& entry = byId.at(known->second);
				if (entry.confirmed || a_now - entry.reportedAt < ANSWER_WAIT) {
					return std::nullopt;
				}
				entry.reportedAt = a_now;  // unanswered: asked again, under the same id
				return known->second;
			}
			std::uint32_t attempt = 0;
			if (const auto it = refused.find(a_local); it != refused.end()) {
				if (a_now - it->second.at < REFUSED_WAIT) {
					return std::nullopt;
				}
				attempt = it->second.attempt;
				refused.erase(it);
			}
			if (byId.size() >= MAX) {
				return std::nullopt;
			}
			std::uint32_t id = 0;
			for (std::uint32_t tries = 0; tries < 16; ++tries, ++attempt) {
				const auto candidate = CandidateId(a_local, attempt);
				if (!byId.contains(candidate) && !a_taken(candidate)) {
					id = candidate;
					break;
				}
			}
			if (id == 0) {
				return std::nullopt;
			}
			byId[id] = { a_local, false, a_now, attempt };
			byLocal[a_local] = id;
			return id;
		}

		// The server named who runs a spawn id (in answer to our report, or to everyone): ours confirms it. Anyone
		// else's, or nobody's, ends ours there: refused (another game shares under that id, which we then don't ask for
		// again; or no room), or let go of (released, dead). What was refused or let go of waits REFUSED_WAIT.
		// Whether the id was one of ours.
		bool Answered(std::uint32_t a_id, std::uint32_t a_owner, std::uint32_t a_localPlayer, Clock::time_point a_now)
		{
			const auto it = byId.find(a_id);
			if (it == byId.end()) {
				return false;
			}
			if (a_owner != 0 && a_owner == a_localPlayer) {
				it->second.confirmed = true;
				return true;
			}
			if (refused.size() >= MAX_REFUSED) {
				std::erase_if(refused, [&](const auto& a_entry) { return a_now - a_entry.second.at >= REFUSED_WAIT; });
			}
			if (refused.size() < MAX_REFUSED) {
				refused[it->second.local] = { a_now, it->second.attempt + (a_owner != 0 ? 1u : 0u) };
			}
			byLocal.erase(it->second.local);
			byId.erase(it);
			return true;
		}

		// The spawn id the server granted for our NPC, 0 if none (not shared, or not granted yet).
		[[nodiscard]] std::uint32_t IdOf(std::uint32_t a_local) const
		{
			const auto it = byLocal.find(a_local);
			return it != byLocal.end() && byId.at(it->second).confirmed ? it->second : 0;
		}

		// Our NPC under a spawn id granted to us, 0 if none.
		[[nodiscard]] std::uint32_t LocalOf(std::uint32_t a_id) const
		{
			const auto it = byId.find(a_id);
			return it != byId.end() && it->second.confirmed ? it->second.local : 0;
		}

		// Reported (answered or not) under this id.
		[[nodiscard]] bool Holds(std::uint32_t a_id) const { return byId.contains(a_id); }

		// Our NPC or frame is reported (answered or not).
		[[nodiscard]] bool Shares(std::uint32_t a_local) const { return byLocal.contains(a_local); }

		[[nodiscard]] std::size_t Size() const { return byId.size(); }

		// Every spawn id reported (answered or not).
		[[nodiscard]] std::vector<std::uint32_t> Ids() const
		{
			std::vector<std::uint32_t> ids;
			ids.reserve(byId.size());
			for (const auto& [id, entry] : byId) {
				ids.push_back(id);
			}
			return ids;
		}

		void Clear()
		{
			byId.clear();
			byLocal.clear();
			refused.clear();
		}

	private:
		static constexpr std::size_t MAX_REFUSED = 1024;

		struct Entry
		{
			std::uint32_t     local = 0;
			bool              confirmed = false;
			Clock::time_point reportedAt{};
			std::uint32_t     attempt = 0;  // which CandidateId it is
		};
		struct Refusal
		{
			Clock::time_point at{};
			std::uint32_t     attempt = 0;  // the CandidateId to ask for next
		};
		std::unordered_map<std::uint32_t, Entry>         byId;
		std::unordered_map<std::uint32_t, std::uint32_t> byLocal;
		std::unordered_map<std::uint32_t, Refusal>       refused;  // by our NPC
	};

	// ---- Theirs: friends' spawns, and our copies of them -----------------------------------------------------------

	// A copy is placed while our player stands within PLACE_RANGE of where its owner's game has it, in the same space,
	// and the owner's stand-in within OWNER_RANGE of it: their game's word for an NPC near them, not across the map. It
	// goes when our player is farther than KEEP_RANGE from it (placed again on coming back), when its owner's game lets
	// it go (unloaded, or the owner left) while alive, and a dead one's corpse when we leave it behind.
	constexpr float PLACE_RANGE = 6000.0f;
	constexpr float OWNER_RANGE = 8000.0f;
	constexpr float KEEP_RANGE = 9000.0f;
	constexpr int   PLACE_TRIES = 3;  // placing it failed this often: left unplaced
	constexpr auto  PLACE_RETRY = std::chrono::seconds(5);
	// A copy just placed has no 3D for a moment (the engine loads it on another thread): it counts as unloaded only once
	// its 3D is still missing this long after it was placed, or has gone.
	constexpr auto  LOAD_WAIT = std::chrono::seconds(10);

	// What the game sees of one spawn, for Theirs::Decide.
	struct Facts
	{
		Spot                player;             // where our player is
		std::optional<Spot> owner;              // where its owner's stand-in last said it was (nothing: not heard from)
		bool                copyThere = false;  // our copy is still there (its handle resolves, not deleted)
		std::optional<Spot> copyAt;             // where it stands, while it is in a cell
		bool                copyLoaded = true;  // its 3D is in
		bool                baseOk = true;      // its base is an NPC we place copies of (not unique, not the player's)
		bool                waiting = false;    // its owner's roll of it is yet to come (NpcRoll::IDENTITY_WAIT): not placed yet
	};

	enum class Act
	{
		kNone,
		kPlace,    // place our copy now
		kDiscard,  // delete our copy (the record stays, unplaced, unless the spawn is over)
		kLost,     // our copy is gone from under us (a load, the engine): forget it, unplaced
		kForget,   // the spawn is over and no copy is left: drop the record
		kMove,     // a frame's copy: moved in place to where its owner's game has it now (DecideFrame)
		kAbandon,  // somebody here got into our copy of a frame: theirs now, neither moved nor deleted (DecideFrame)
	};

	// What the game sees of our copy of a friend's frame (a spawn whose base is a power armor frame), for DecideFrame.
	struct FrameFacts
	{
		bool                inView = false;     // where its owner's game has it is in the area our engine draws around our player
		bool                copyThere = false;  // our copy is still there (its handle resolves, not deleted)
		std::optional<Spot> copyAt;             // where it stands, while it is in a cell
		float               copyHeading = 0.0f;
		bool                copyLoaded = true;  // its 3D is in
		bool                occupied = false;   // somebody here is in our copy, or it is the frame our player left last
		bool                leaving = false;    // who is in it is being sent out of it, or just was (RemotePlayers::BeingSentOut)
		bool                baseOk = true;      // its base is a power armor frame here
		// Lent to our player (FrameLoans): the kit is in our copy, or they wear it. Neither moved, nor deleted, nor given up
		// to whoever is in it, until the loan is over (then it goes, and is placed again where the owner's game has it).
		bool                lent = false;
	};

	class Theirs
	{
	public:
		static constexpr std::size_t MAX = 128;  // friends' spawns known at once (the server holds 128)

		struct Copy
		{
			Protocol::Spawn   spawn;
			Spot              at;                // where its owner's game has it now (its spawn, then its states)
			float             heading = 0.0f;
			std::uint32_t     local = 0;         // our copy, 0 while none is placed
			bool              dead = false;      // it died (in its owner's game, or ours by a believed kill)
			bool              gone = false;      // its owner's game let go of it (an owner of 0)
			int               tries = 0;         // placements that failed in a row
			Clock::time_point triedAt{};
			Clock::time_point placedAt{};       // when our copy was placed
			bool              taken = false;     // a frame: somebody here got into our copy, and it is theirs now
		};

		// A friend's game shares one. False when it is ours (the server never grants one id twice, but a book can lag),
		// from us, or the book is full. One already known under the id is replaced: the old one is over (Forget it first,
		// after deleting its copy, if it had one: Find(id)->local).
		bool Add(const Protocol::Spawn& a_spawn, std::uint32_t a_localPlayer)
		{
			if (!Protocol::IsSpawnRef(a_spawn.refId) || a_spawn.playerId == 0 || a_spawn.playerId == a_localPlayer) {
				return false;
			}
			if (!copies.contains(a_spawn.refId) && copies.size() >= MAX) {
				return false;
			}
			auto& copy = copies[a_spawn.refId];
			if (copy.local != 0) {
				byLocal.erase(copy.local);
			}
			copy = Copy{ .spawn = a_spawn, .at = { a_spawn.cell, a_spawn.worldspace, a_spawn.x, a_spawn.y, a_spawn.z }, .heading = a_spawn.heading };
			return true;
		}

		// Its owner's game says where it is now (a state).
		void Moved(std::uint32_t a_id, const Spot& a_at, float a_heading)
		{
			if (const auto it = copies.find(a_id); it != copies.end()) {
				it->second.at = a_at;
				it->second.heading = a_heading;
			}
		}

		// It died. Our copy to kill (0: none placed, or it was known dead already).
		std::uint32_t Died(std::uint32_t a_id)
		{
			const auto it = copies.find(a_id);
			if (it == copies.end() || std::exchange(it->second.dead, true)) {
				return 0;
			}
			return it->second.local;
		}

		// Its owner's game let go of it, or nobody runs it any more (an owner of 0).
		void Gone(std::uint32_t a_id)
		{
			if (const auto it = copies.find(a_id); it != copies.end()) {
				it->second.gone = true;
			}
		}

		[[nodiscard]] Act Decide(std::uint32_t a_id, const Facts& a_facts, Clock::time_point a_now) const
		{
			const auto it = copies.find(a_id);
			if (it == copies.end()) {
				return Act::kNone;
			}
			const auto& copy = it->second;
			if (copy.local != 0) {
				if (!a_facts.copyThere) {
					return Act::kLost;
				}
				if (a_facts.lent) {
					return Act::kNone;
				}
				if (copy.gone && !copy.dead) {
					return Act::kDiscard;
				}
				// Not loaded (its cell went: no 3D, past the wait for its first), or we left it behind: it goes, a corpse too.
				const bool unloaded = !a_facts.copyLoaded && a_now - copy.placedAt >= LOAD_WAIT;
				if (!a_facts.copyAt || unloaded || !RemoteEdits::Within(RemoteEdits::Distance(*a_facts.copyAt, a_facts.player), KEEP_RANGE)) {
					return Act::kDiscard;
				}
				return Act::kNone;
			}
			if (copy.gone || copy.dead) {
				return Act::kForget;
			}
			if (!a_facts.baseOk || copy.tries >= PLACE_TRIES || (copy.tries > 0 && a_now - copy.triedAt < PLACE_RETRY)) {
				return Act::kNone;
			}
			if (!RemoteEdits::Within(RemoteEdits::Distance(copy.at, a_facts.player), PLACE_RANGE)) {
				return Act::kNone;
			}
			if (!a_facts.owner || !RemoteEdits::Within(RemoteEdits::Distance(*a_facts.owner, copy.at), OWNER_RANGE)) {
				return Act::kNone;
			}
			return a_facts.waiting ? Act::kNone : Act::kPlace;
		}

		// A friend's frame, placed as a friend's parked one is (ArmorFrames::ChoosePark) rather than as an NPC: wherever its
		// owner is (their suit stands at home while they roam), while where it stands is in our sight. Moved in place when
		// its owner's game has it a little way off (the physics nudged it), placed again when it is far off or in another
		// place, gone once out of our sight or over. A frame never dies: one its owner's game let go of (worn, gone, left in
		// a cell since unloaded) is over, and shared afresh when it stands again. A copy somebody here got into (but one being
		// sent out of it again, RemotePlayers::BeingSentOut) is theirs from then on: neither moved nor deleted, and none placed
		// again while this spawn lasts.
		[[nodiscard]] Act DecideFrame(std::uint32_t a_id, const FrameFacts& a_facts, Clock::time_point a_now) const
		{
			const auto it = copies.find(a_id);
			if (it == copies.end()) {
				return Act::kNone;
			}
			const auto& copy = it->second;
			if (copy.local != 0) {
				if (!a_facts.copyThere) {
					return Act::kLost;
				}
				if (a_facts.leaving) {
					return Act::kNone;  // still ours while they are on their way out, and until its 3D is back
				}
				if (a_facts.occupied) {
					return Act::kAbandon;
				}
				if (copy.gone || !a_facts.inView || !a_facts.copyAt) {
					return Act::kDiscard;
				}
				const float distance = RemoteEdits::Distance(*a_facts.copyAt, copy.at);  // NaN in another place
				if (!RemoteEdits::Within(distance, FrameShare::NUDGE_LIMIT)) {
					return Act::kDiscard;  // placed again where it stands now
				}
				if (!a_facts.copyLoaded) {
					return a_now - copy.placedAt >= LOAD_WAIT ? Act::kDiscard : Act::kNone;
				}
				if (!RemoteEdits::Within(distance, FrameShare::MOVED) || StandInHold::AngleBetween(a_facts.copyHeading, copy.heading) > FrameShare::TURNED) {
					return Act::kMove;
				}
				return Act::kNone;
			}
			if (copy.gone || copy.dead) {
				return Act::kForget;
			}
			if (copy.taken || !a_facts.baseOk || copy.tries >= PLACE_TRIES || (copy.tries > 0 && a_now - copy.triedAt < PLACE_RETRY)) {
				return Act::kNone;
			}
			return a_facts.inView ? Act::kPlace : Act::kNone;
		}

		// Somebody here got into our copy of a frame (DecideFrame's kAbandon): it is theirs, no copy of ours any more, and
		// none is placed again while this spawn lasts.
		void Taken(std::uint32_t a_id)
		{
			const auto it = copies.find(a_id);
			if (it == copies.end()) {
				return;
			}
			byLocal.erase(it->second.local);
			it->second.local = 0;
			it->second.taken = true;
		}

		// The spawn whose copy of ours stands for a frame a friend's status says they left parked (Protocol::ParkedFrame):
		// one of a_owner's, of that base, placed, which their game has within a_radius of that spot. 0 when none.
		[[nodiscard]] std::uint32_t FrameAt(std::uint32_t a_owner, std::uint32_t a_base, const Spot& a_spot, float a_radius) const
		{
			for (const auto& [id, copy] : copies) {
				if (copy.local != 0 && copy.spawn.playerId == a_owner && copy.spawn.base == a_base && !copy.gone &&
					RemoteEdits::Within(RemoteEdits::Distance(copy.at, a_spot), a_radius)) {
					return id;
				}
			}
			return 0;
		}

		// Our copy was placed (a_local), or placing it failed (0).
		void Placed(std::uint32_t a_id, std::uint32_t a_local, Clock::time_point a_now)
		{
			const auto it = copies.find(a_id);
			if (it == copies.end()) {
				return;
			}
			if (a_local == 0) {
				++it->second.tries;
				it->second.triedAt = a_now;
				return;
			}
			it->second.local = a_local;
			it->second.placedAt = a_now;
			it->second.tries = 0;
			byLocal[a_local] = a_id;
		}

		// Our copy is gone (deleted by us, or lost): the record stays, unplaced, unless the spawn is over.
		void Unplaced(std::uint32_t a_id)
		{
			const auto it = copies.find(a_id);
			if (it == copies.end()) {
				return;
			}
			if (it->second.local != 0) {
				byLocal.erase(it->second.local);
				it->second.local = 0;
			}
			if (it->second.gone || it->second.dead) {
				copies.erase(it);
			}
		}

		void Forget(std::uint32_t a_id)
		{
			if (const auto it = copies.find(a_id); it != copies.end()) {
				byLocal.erase(it->second.local);
				copies.erase(it);
			}
		}

		[[nodiscard]] const Copy* Find(std::uint32_t a_id) const
		{
			const auto it = copies.find(a_id);
			return it != copies.end() ? &it->second : nullptr;
		}

		// Whether a friend's game shares a spawn under this id.
		[[nodiscard]] bool Knows(std::uint32_t a_id) const { return copies.contains(a_id); }

		// Our copy of the spawn, 0 if none is placed.
		[[nodiscard]] std::uint32_t LocalOf(std::uint32_t a_id) const
		{
			const auto it = copies.find(a_id);
			return it != copies.end() ? it->second.local : 0;
		}

		// The spawn our copy is of, 0 if it is no copy of ours.
		[[nodiscard]] std::uint32_t IdOf(std::uint32_t a_local) const
		{
			const auto it = byLocal.find(a_local);
			return it != byLocal.end() ? it->second : 0;
		}

		[[nodiscard]] std::vector<std::uint32_t> Ids() const
		{
			std::vector<std::uint32_t> ids;
			ids.reserve(copies.size());
			for (const auto& [id, copy] : copies) {
				ids.push_back(id);
			}
			return ids;
		}

		[[nodiscard]] std::size_t Size() const { return copies.size(); }
		[[nodiscard]] std::size_t PlacedCount() const { return byLocal.size(); }

		void Clear()
		{
			copies.clear();
			byLocal.clear();
		}

	private:
		std::unordered_map<std::uint32_t, Copy>          copies;
		std::unordered_map<std::uint32_t, std::uint32_t> byLocal;  // our copy -> its spawn id
	};
}
