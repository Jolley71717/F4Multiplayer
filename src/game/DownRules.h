#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <span>

// Downed and revive, kept free of the game so it can be tested: what our own game does as the player is
// knocked down, helped up or left to bleed out (Self), and when we stand next to a downed friend (Helper).
// Downed.cpp tells both what the game shows every frame and does what they answer.
namespace DownRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr auto  BLEEDOUT_TIME = 45s;
	inline constexpr auto  GET_UP_TIME = 20s;   // after being helped, the game stands the player up by then
	inline constexpr auto  KILL_RETRY = 2s;     // a bleed-out that left the player alive and down kills again then
	inline constexpr auto  HELP_TIME = 2s;      // standing next to a downed friend this long helps them up
	inline constexpr auto  HELP_RETRY = 3s;     // before helping the same friend again
	inline constexpr float HELP_RANGE = 300.0f; // game units (about 4 m): "right here" in the down notice is 5 m

	// The engine's ACTOR_LIFE_STATE values for an essential character that is down: kEssentialDown and kBleedout.
	inline constexpr std::int32_t LIFE_ESSENTIAL_DOWN = 7;
	inline constexpr std::int32_t LIFE_BLEEDOUT = 8;

	constexpr bool IsDownState(std::int32_t a_lifeState)
	{
		return a_lifeState == LIFE_ESSENTIAL_DOWN || a_lifeState == LIFE_BLEEDOUT;
	}

	// kDying and kDead.
	inline constexpr std::int32_t LIFE_DYING = 1;
	inline constexpr std::int32_t LIFE_DEAD = 2;

	// Dead, or on the way there, from the life state and the game's IsDead. That answers true for a character
	// that is down too (seen live: a player down at -30 health read dead), so a character down is never dead
	// here: counted dead, nobody ever went down.
	constexpr bool Dead(std::int32_t a_lifeState, bool a_isDead)
	{
		return !IsDownState(a_lifeState) && (a_isDead || a_lifeState == LIFE_DYING || a_lifeState == LIFE_DEAD);
	}

	// A friend's stand-in that is dead, switched off, or down is replaced by a fresh one. It is essential and
	// kept at full health every update, but one hit bigger than its whole health (a grenade, a shotgun at
	// close range) still put it down here; with its AI off nothing stands it up again, so it sat on the
	// ground aiming its gun while its friend stood and fought. A friend who is down themselves shows it by
	// crouching, not by this.
	constexpr bool StandInBroken(std::int32_t a_lifeState, bool a_dead, bool a_disabled)
	{
		return a_dead || a_disabled || IsDownState(a_lifeState);
	}

	// What the game shows of our player this frame.
	struct Facts
	{
		bool enabled = false;    // in a session with friends, revive on, in the world
		bool essential = false;  // our essential flag is on the player's character
		bool down = false;       // the game has them down (IsDownState)
		bool dead = false;       // dead or dying
	};

	// What to do about it this frame.
	struct Step
	{
		std::optional<bool> essential;  // set our essential flag on or off
		std::optional<bool> stayDown;   // keep the game from standing them up by itself
		bool                kill = false;
		bool                wentDown = false;    // "You're down!"
		bool                trimHealth = false;  // up after being helped: the game gave full health, cut it back
		int                 secondsLeft = -1;    // a reminder of the time left to be helped up
	};

	// The player's own down state. Up; down and waiting for help; helped and getting up; or bled out (essential
	// off and killed, waiting to be dead). A bled-out player is not made essential again while still alive: the
	// kill had not taken by the next frame, essential went back on, and the player was "down!" again instead of
	// dead, every 45 s for as long as they waited.
	class Self
	{
	public:
		enum class Phase
		{
			kUp,
			kDown,
			kGettingUp,
			kBledOut,
		};

		Step Frame(const Facts& a_facts, Clock::time_point a_now)
		{
			Step step;
			switch (phase) {
			case Phase::kUp:
				// Not while dying: making a dying character essential again could stop the death.
				if (!a_facts.dead || !a_facts.enabled) {
					step.essential = a_facts.enabled;
				}
				if (a_facts.essential && a_facts.down && !a_facts.dead) {
					phase = Phase::kDown;
					giveUp = false;
					downSince = a_now;
					reminders = 0;
					++downs;
					// The game would stand the player up again by itself after a while.
					step.stayDown = true;
					step.wentDown = true;
				}
				break;
			case Phase::kDown:
				if (!a_facts.down) {
					phase = Phase::kUp;  // up again by itself (a save and load, a console command)
					step.stayDown = false;
				} else if (giveUp || !a_facts.enabled || a_now - downSince >= BLEEDOUT_TIME) {
					BleedOut(step, a_now);
				} else {
					// A save clears both flags (Downed::OnBeforeSave); still down, so set them again.
					step.essential = true;
					step.stayDown = true;
					const auto left = std::chrono::duration_cast<std::chrono::seconds>(BLEEDOUT_TIME - (a_now - downSince)).count();
					if ((reminders == 0 && left <= 30) || (reminders == 1 && left <= 10)) {
						++reminders;
						step.secondsLeft = static_cast<int>(left);
					}
				}
				break;
			case Phase::kGettingUp:
				if (!a_facts.down) {
					phase = Phase::kUp;
					step.stayDown = false;
					step.trimHealth = true;
				} else if (a_now - revivedAt >= GET_UP_TIME) {
					// Helped up: the game stands the player up on its own. If it somehow doesn't, they die.
					BleedOut(step, a_now);
				} else {
					// Essential until they stand, even across a save.
					step.essential = true;
				}
				break;
			case Phase::kBledOut:
				step.essential = false;
				if (a_facts.dead) {
					break;  // a load or a resurrection starts over (Loaded, or up below)
				}
				if (!a_facts.down) {
					phase = Phase::kUp;  // the kill did not take and the game stood them up: as before the down
				} else if (a_now - killedAt >= KILL_RETRY) {
					step.kill = true;
					killedAt = a_now;
					++killRetries;
				}
				break;
			}
			return step;
		}

		// A friend helped us up: true when that stands us up (only once per down; a second helper, or the
		// same one again, changes nothing).
		bool Revive(Clock::time_point a_now)
		{
			if (phase != Phase::kDown) {
				return false;
			}
			phase = Phase::kGettingUp;
			revivedAt = a_now;
			++revives;
			return true;
		}

		void GiveUp() { giveUp = phase == Phase::kDown; }

		// A save was loaded: whatever state we were in belongs to the old game. True when the player is down in
		// it (a save made while down): they are stood up as if helped.
		bool Loaded(bool a_down, Clock::time_point a_now)
		{
			phase = Phase::kUp;
			giveUp = false;
			if (!a_down) {
				return false;
			}
			phase = Phase::kGettingUp;
			revivedAt = a_now;
			return true;
		}

		[[nodiscard]] Phase Now() const { return phase; }
		// On the ground: down, or helped and getting up (the stand-in crouches).
		[[nodiscard]] bool IsDown() const { return phase == Phase::kDown || phase == Phase::kGettingUp; }
		// What friends are told: down and still waiting for help. Once helped, friends stop helping; they kept
		// helping while the game stood the player up, and the helper read "You helped Host up" three times for one down.
		[[nodiscard]] bool WaitsForHelp() const { return phase == Phase::kDown; }

		std::uint32_t downs = 0;
		std::uint32_t revives = 0;
		std::uint32_t bleedOuts = 0;
		std::uint32_t killRetries = 0;

	private:
		void BleedOut(Step& a_step, Clock::time_point a_now)
		{
			phase = Phase::kBledOut;
			a_step.stayDown = false;
			a_step.essential = false;
			a_step.kill = true;
			killedAt = a_now;
			++bleedOuts;
		}

		Phase             phase = Phase::kUp;
		bool              giveUp = false;
		Clock::time_point downSince{};
		Clock::time_point revivedAt{};
		Clock::time_point killedAt{};
		int               reminders = 0;
	};

	// Helping downed friends up: stay within HELP_RANGE of one's stand-in for HELP_TIME.
	class Helper
	{
	public:
		// A friend's stand-in in our world, and how far it is from us.
		struct Near
		{
			std::uint32_t id = 0;
			float         distance = 0.0f;
		};

		struct HelpStep
		{
			std::uint32_t started = 0;  // began helping this friend ("Helping ... up")
			std::uint32_t helped = 0;   // send them a Revive
		};

		void SetFriendDown(std::uint32_t a_id, bool a_down)
		{
			if (a_down) {
				friendsDown.insert(a_id);
			} else {
				friendsDown.erase(a_id);
			}
		}

		void Left(std::uint32_t a_id) { friendsDown.erase(a_id); }

		void Clear()
		{
			friendsDown.clear();
			helping = 0;
		}

		// a_able: we are up, alive and in a session with revive on. a_near: friends' stand-ins in our world.
		HelpStep Frame(bool a_able, std::span<const Near> a_near, Clock::time_point a_now)
		{
			HelpStep      step;
			std::uint32_t near = 0;
			if (a_able && !friendsDown.empty() && a_now >= helpAgain) {
				for (const auto& friendNear : a_near) {
					if (friendsDown.contains(friendNear.id) && friendNear.distance <= HELP_RANGE) {
						near = friendNear.id;
						break;
					}
				}
			}
			if (near != helping) {
				helping = near;
				helpSince = a_now;
				step.started = near;
			} else if (near && a_now - helpSince >= HELP_TIME) {
				step.helped = near;
				++helped;
				helping = 0;
				helpAgain = a_now + HELP_RETRY;
			}
			return step;
		}

		[[nodiscard]] std::size_t FriendsDown() const { return friendsDown.size(); }

		std::uint32_t helped = 0;

	private:
		std::set<std::uint32_t> friendsDown;
		std::uint32_t           helping = 0;
		Clock::time_point       helpSince{};
		Clock::time_point       helpAgain{};
	};
}
