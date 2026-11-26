#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

// Who owns a conversation with an NPC: one player at a time. Whoever starts first owns it; a friend who presses E on an
// NPC another player already talks to gets no conversation of their own (the engine opens one on their copy: NpcSync ends
// it, with a notice, and their game goes on mirroring the NPC, so they hear the owner's lines, Conversations::Apply).
//
// Each game says whom it talks to in its status (PlayerStatus::talkingTo, the NPC's id as claims name it, protocol 86),
// only while the conversation is its own. A friend's status that names the NPC is their claim on it. Pure, so it can be
// tested with the server.
namespace TalkOwner
{
	using Clock = std::chrono::steady_clock;

	// A status comes at least every 2 s (Party's STATUS_INTERVAL) while the game runs. One not heard for this long is no
	// claim any more: the friend's game stopped or hung, or they left without the server's word reaching us. Their claim
	// ends sooner when their status stops naming the NPC (their menu closed) or they leave (Gone).
	constexpr auto STALE = std::chrono::seconds(8);
	constexpr std::size_t MAX_PLAYERS = 64;

	class Book
	{
	public:
		// A friend's status: the NPC they talk to (0: none).
		void Heard(std::uint32_t a_player, std::uint32_t a_talkingTo, Clock::time_point a_now)
		{
			if (a_talkingTo == 0) {
				talks.erase(a_player);
				return;
			}
			if (talks.size() >= MAX_PLAYERS && !talks.contains(a_player)) {
				return;
			}
			talks[a_player] = { a_talkingTo, a_now };
		}

		void Gone(std::uint32_t a_player)
		{
			talks.erase(a_player);
		}

		void Clear()
		{
			talks.clear();
		}

		// The player with the lowest id who talks to a_npc (their status heard within STALE), a_self left out; none when
		// nobody does.
		[[nodiscard]] std::optional<std::uint32_t> Holder(std::uint32_t a_npc, std::uint32_t a_self, Clock::time_point a_now) const
		{
			for (const auto& [player, talk] : talks) {  // ordered by id
				if (player != a_self && talk.npc == a_npc && a_now - talk.at <= STALE) {
					return player;
				}
			}
			return std::nullopt;
		}

		// Whom a player talks to, as last heard (0: none or stale): for the log.
		[[nodiscard]] std::uint32_t NpcOf(std::uint32_t a_player, Clock::time_point a_now) const
		{
			const auto it = talks.find(a_player);
			return it != talks.end() && a_now - it->second.at <= STALE ? it->second.npc : 0;
		}

	private:
		struct Talk
		{
			std::uint32_t     npc = 0;
			Clock::time_point at{};
		};
		std::map<std::uint32_t, Talk> talks;
	};

	struct Verdict
	{
		bool          ours = true;  // the conversation is ours: go on, and say we talk to it
		std::uint32_t owner = 0;    // else: the friend who owns it
	};

	// Whether our conversation with a_npc is ours. a_claimed: our status already names it (we have been the one it was
	// reported for), so a friend's claim on it too is a race: both pressed E before either status arrived. The lower
	// player id keeps it, the same answer in both games, so exactly one conversation stays. Not claimed yet (the engine
	// just opened it): a friend who has claimed it started first, whatever their id.
	[[nodiscard]] inline Verdict Decide(const Book& a_book, std::uint32_t a_self, std::uint32_t a_npc, bool a_claimed, Clock::time_point a_now)
	{
		const auto holder = a_book.Holder(a_npc, a_self, a_now);
		if (!holder) {
			return {};
		}
		if (a_claimed && a_self < *holder) {
			return {};
		}
		return { .ours = false, .owner = *holder };
	}
}
