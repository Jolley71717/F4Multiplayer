#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

// When a conversation left open without its speaker is ended. The engine ends a conversation through
// its speaker, so once the speaker is gone (a friend's stand-in deleted in the middle of it) nothing
// ends it, and the player has no weapons, Pip-Boy or workshop until they talk to someone else. Only a
// conversation the plugin itself left that way is ended: one whose speaker the plugin took out of the
// world (a friend's stand-in disabled, then deleted: see RemotePlayers), remembered here as it goes
// (TakenOut). One the game left open is the game's, whoever the speaker was and however it went; and a
// scene the player is in (a quest's) is never ended from here unless it is the one that speaker was in
// with them. It is only ended after a moment of that, since a speaker can briefly fail to resolve while
// a conversation starts or its NPC reloads.
namespace StrandedDialogue
{
	using Clock = std::chrono::steady_clock;

	constexpr auto GRACE = std::chrono::seconds(2);
	// A speaker the plugin took out is remembered this long. Its conversation, if it was in one, is ended
	// within GRACE of that (a few, when the end doesn't take); and a created reference's form ID can be
	// given out again to something else.
	constexpr auto REMEMBER = std::chrono::minutes(2);

	class Timer
	{
	public:
		// Whether to end the conversation now: it has been stranded for GRACE without a break. If it
		// is still stranded GRACE after that (the end didn't take), again.
		bool Due(bool a_stranded, Clock::time_point a_now)
		{
			if (!a_stranded) {
				Reset();
				return false;
			}
			if (!since) {
				since = a_now;
				return false;
			}
			if (a_now - *since < GRACE) {
				return false;
			}
			since = a_now;
			++ends;
			return true;
		}

		// The wait starts over: the conversation is fine, or over, or a loading screen is up (it may last
		// longer than GRACE, and is no part of the wait).
		void Reset()
		{
			since.reset();
			ends = 0;
		}

		// How often Due() said so since the conversation was last fine: 1 the first time.
		[[nodiscard]] std::uint32_t Ends() const { return ends; }

	private:
		std::optional<Clock::time_point> since;
		std::uint32_t                    ends = 0;
	};

	// The references the plugin took out of the world lately, by form ID, each with the scene it was in
	// as it went (0: none).
	class TakenOut
	{
	public:
		void Add(std::uint32_t a_id, std::uint32_t a_scene, Clock::time_point a_now)
		{
			if (a_id == 0) {
				return;
			}
			Forget(a_now);
			for (auto& entry : entries) {
				if (entry.id == a_id) {
					entry.scene = a_scene;
					entry.at = a_now;
					return;
				}
			}
			entries.push_back({ a_id, a_scene, a_now });
		}

		// The scene a_id was in as the plugin took it out, if the plugin did lately.
		[[nodiscard]] std::optional<std::uint32_t> Find(std::uint32_t a_id, Clock::time_point a_now)
		{
			Forget(a_now);
			for (const auto& entry : entries) {
				if (a_id != 0 && entry.id == a_id) {
					return entry.scene;
				}
			}
			return std::nullopt;
		}

	private:
		struct Entry
		{
			std::uint32_t     id;
			std::uint32_t     scene;
			Clock::time_point at;
		};

		void Forget(Clock::time_point a_now)
		{
			std::erase_if(entries, [&](const Entry& a_entry) { return a_now - a_entry.at >= REMEMBER; });
		}

		std::vector<Entry> entries;
	};

	// The player's conversation as the game shows it this frame.
	struct Facts
	{
		bool          open = false;         // MenuTopicManager.menuOpen: a conversation is open
		std::uint32_t speaker = 0;          // its speaker's form ID; 0 when none resolves
		bool          speakerGone = false;  // the speaker is disabled or deleted, or none resolves
		bool          talking = false;      // the dialogue menu is up: the player is talking in it
		std::uint32_t scene = 0;            // the scene the player is in (a quest's), 0 in none
		bool          loading = false;      // a loading screen is up
	};

	// The decision for the conversation the player is in: whether the plugin ends it now.
	class Watch
	{
	public:
		// The plugin took a reference out of the world, which was in scene a_scene (0: none).
		void Removed(std::uint32_t a_id, std::uint32_t a_scene, Clock::time_point a_now) { takenOut.Add(a_id, a_scene, a_now); }

		// Whether to end the conversation this frame. The speaker last seen stays the conversation's while
		// none resolves (one deleted no longer does); another speaker takes its place.
		bool Due(const Facts& a_facts, Clock::time_point a_now)
		{
			if (a_facts.speaker != 0) {
				speaker = a_facts.speaker;
			}
			if (a_facts.loading) {  // no part of the wait: it starts over once the screen is gone
				timer.Reset();
				return false;
			}
			const auto inScene = takenOut.Find(speaker, a_now);  // the scene it was in as it went, if the plugin took it out
			ours = inScene && (a_facts.scene == 0 || a_facts.scene == *inScene);
			return timer.Due(a_facts.open && a_facts.speakerGone && !a_facts.talking && ours, a_now);
		}

		// Out of dialogue: nothing known of a conversation, nothing waited for. What the plugin took out
		// stays known.
		void Reset()
		{
			timer.Reset();
			speaker = 0;
			ours = false;
		}

		// The conversation's speaker: the one last seen, 0 when none was.
		[[nodiscard]] std::uint32_t Speaker() const { return speaker; }

		// As of the last Due(): the conversation is the plugin's to end, its speaker one the plugin took out
		// and the scene, if the player is in one, the one that speaker was in.
		[[nodiscard]] bool Ours() const { return ours; }

		// How often Due() said so since the conversation was last fine: 1 the first time.
		[[nodiscard]] std::uint32_t Ends() const { return timer.Ends(); }

	private:
		Timer         timer;
		TakenOut      takenOut;
		std::uint32_t speaker = 0;
		bool          ours = false;
	};
}
