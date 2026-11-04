#pragma once

#include "game/PauseHandOff.h"
#include "game/RemoteEdits.h"

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

// A shared NPC is run by one game; every other game's copy runs no AI of its own (Puppets) and only follows where the
// runner's copy walks. The runner's AI reacts to its own player alone: a friend standing next to an NPC our game runs
// sees it walk, but never its greeting, its idle chatter or its turn toward them. The distance hand-off (NpcSync's
// HANDOFF_FROM) only gives it to them once we are 6000 units off, so a friend 1-6000 units from us in a town found its
// people standing about, deaf to them.
//
// So a friendly NPC near a friend, with us well off, is handed to their game earlier: not hostile to either of us, not
// in combat, not ours (our companion, our conversation or scene, a settler we command, one we fought lately), not
// handed off lately, us more than RUNNER_FROM from it and a friend whose game runs within FRIEND_WITHIN and at most a
// third as far as us. The gap between those ranges is the hysteresis: an NPC handed over goes back only once that
// friend is beyond RUNNER_FROM and we are within FRIEND_WITHIN of it. Each change of hands restarts its AI
// (NpcHands), so two more holds keep it from trading back and forth: the facts must hold for DWELL (a friend walking
// past is no reason), and our game must have run it for SETTLED (one just handed to us stays). Released through the
// hand-off's own path (NpcSync's handedOffAt). Kept free of the game so it can be tested.
namespace FriendlyHandOff
{
	using RemoteEdits::Spot;
	using Clock = std::chrono::steady_clock;

	constexpr float RUNNER_FROM = 2000.0f;    // we are farther than this from it
	constexpr float FRIEND_WITHIN = 800.0f;   // the friend is this near it
	constexpr float NEARER_BY = 3.0f;         // and at most a third as far as we are
	constexpr auto  DWELL = std::chrono::seconds(3);
	constexpr auto  SETTLED = std::chrono::seconds(20);
	constexpr auto  FORGET_AFTER = std::chrono::seconds(5);  // a watch not looked at this long (the NPC unloaded) is dropped

	// A friend, as their latest state has them.
	struct Friend
	{
		Spot         at;
		std::uint8_t menu = Protocol::MenuKind::kNone;  // the menu they are in (their status)
		bool         hostile = false;                   // the NPC is hostile to their stand-in here
	};

	// An NPC our game runs, as Scan finds it (loaded and alive; never one of our spawns, which no other game has).
	struct Npc
	{
		Spot            at;
		bool            hostileToUs = false;   // hostile to our player
		bool            inCombat = false;
		bool            ours = false;            // our companion, in our conversation or scene, or a settler we command
		bool            fought = false;          // our player hit or talked to it lately (NpcHold::FoughtLately)
		bool            handedOffLately = false; // let go within NpcSync's HANDOFF_RETRY
		Clock::duration ranFor{};                // how long our game has run it
	};

	enum class Why
	{
		kOurs,      // it stays with us (see Npc::ours), or we fought it lately
		kHostile,   // hostile to us, or in combat
		kLately,    // handed off lately, or we took it on lately (SETTLED)
		kNear,      // we are within RUNNER_FROM of it
		kNoFriend,  // no friend near enough, nearer by enough, friendly to it and whose game runs
		kWaiting,   // all holds, not yet for DWELL
		kRelease,   // let go: the friend's game takes it on
	};

	// Whether the facts call for a hand-off now, DWELL aside: kRelease when they do.
	inline Why Judge(const Npc& a_npc, const Spot& a_us, const std::vector<Friend>& a_friends)
	{
		if (a_npc.ours || a_npc.fought) {
			return Why::kOurs;
		}
		if (a_npc.hostileToUs || a_npc.inCombat) {
			return Why::kHostile;
		}
		if (a_npc.handedOffLately || a_npc.ranFor < SETTLED) {
			return Why::kLately;
		}
		// In another space than ours (NaN), or beyond RUNNER_FROM: either way not near us.
		const float ours = RemoteEdits::Distance(a_npc.at, a_us);
		if (RemoteEdits::Within(ours, RUNNER_FROM)) {
			return Why::kNear;
		}
		const bool far = !RemoteEdits::SameSpace(a_npc.at, a_us);
		for (const auto& f : a_friends) {
			const float theirs = RemoteEdits::Distance(a_npc.at, f.at);
			if (f.hostile || PauseHandOff::Stopped(f.menu) || !RemoteEdits::Within(theirs, FRIEND_WITHIN)) {
				continue;
			}
			if (far || theirs * NEARER_BY <= ours) {
				return Why::kRelease;
			}
		}
		return Why::kNoFriend;
	}

	// Each NPC whose facts call for a hand-off, and since when they have without a break.
	class Book
	{
	public:
		// What Scan does with an NPC our game runs: kRelease once Judge has said so for DWELL.
		Why Decide(std::uint32_t a_id, const Npc& a_npc, const Spot& a_us, const std::vector<Friend>& a_friends, Clock::time_point a_now)
		{
			const auto why = Judge(a_npc, a_us, a_friends);
			if (why != Why::kRelease) {
				watches.erase(a_id);
				return why;
			}
			auto it = watches.try_emplace(a_id, Watch{ a_now, a_now }).first;
			it->second.seen = a_now;
			if (a_now - it->second.since < DWELL) {
				return Why::kWaiting;
			}
			watches.erase(it);
			return Why::kRelease;
		}

		// It is not ours any more, or gone.
		void Forget(std::uint32_t a_id) { watches.erase(a_id); }

		void Expire(Clock::time_point a_now)
		{
			std::erase_if(watches, [&](const auto& a_entry) { return a_now - a_entry.second.seen >= FORGET_AFTER; });
		}

		void Clear() { watches.clear(); }

		[[nodiscard]] std::size_t Watched() const { return watches.size(); }

	private:
		struct Watch
		{
			Clock::time_point since;
			Clock::time_point seen;
		};

		std::unordered_map<std::uint32_t, Watch> watches;
	};
}
