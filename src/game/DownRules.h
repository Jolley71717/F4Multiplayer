#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <span>

#include "Protocol.h"

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
	inline constexpr auto  FIGHT_OVER = 30s;    // out of a fight this long, it is over (revives "per fight")
	inline constexpr auto  PUT_DOWN_RETRY = 500ms;  // stood up by the game while waiting for help: put down again this often
	inline constexpr auto  PUT_DOWN_GIVE_UP = 5s;   // still up after this long of that: up it is
	inline constexpr auto  STAND_RETRY = 1s;        // helped up and still down: the engine's get-up is asked again this often
	inline constexpr auto  KNOCK_WAIT = 3s;         // helped up while knocked over: the get-up waits this long at most for the knock-down's to end
	inline constexpr auto  GET_UP_SETTLE = 2s;      // up this long, not knocked over, out of the menus: the get-up has played out

	// The session's rules for being down (the host's, Protocol::SessionRules). The defaults are what every session
	// had before there were rules: 45 s to bleed out, 2 s to help, up with 30 %, no limits.
	struct Rules
	{
		bool            revive = true;
		Clock::duration bleedOut = BLEEDOUT_TIME;
		Clock::duration helpTime = HELP_TIME;
		float           reviveHealth = 0.3f;    // of the maximum
		std::uint32_t   maxRevives = 0;         // per window (or fight); 0 = no limit
		Clock::duration window{};               // 0 = per fight
		Clock::duration cooldown{};             // helped up, a lethal hit within this long kills
		bool            outsideCombat = false;  // a helper in a fight cannot help
		bool            crawl = false;          // down, the player crawls with their movement keys (Crawl)
		bool            saveOnDown = false;     // going down makes an autosave (AutoSaveRules::DownSave)

		static Rules From(const Protocol::SessionRules& a_session)
		{
			const auto session = a_session.Clamped();
			Rules      rules;
			rules.revive = session.revive;
			rules.bleedOut = std::chrono::seconds{ session.bleedOutSeconds };
			rules.helpTime = std::chrono::milliseconds{ session.helpTenths * 100 };
			rules.reviveHealth = session.reviveHealth / 100.0f;
			rules.maxRevives = session.maxRevives;
			rules.window = std::chrono::minutes{ session.reviveWindowMinutes };
			rules.cooldown = std::chrono::seconds{ session.downCooldownSeconds };
			rules.outsideCombat = session.reviveOutsideCombat;
			rules.crawl = session.downedCrawl;
			rules.saveOnDown = session.saveOnDown;
			return rules;
		}
	};

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
	// ground aiming its gun while its friend stood and fought. A friend who is down themselves is shown down by
	// StandInDown, which lays the stand-in down itself; that one is not broken (see StandInDown::Laid).
	constexpr bool StandInBroken(std::int32_t a_lifeState, bool a_dead, bool a_disabled)
	{
		return a_dead || a_disabled || IsDownState(a_lifeState);
	}

	// The health friends are told (Party's status), in percent of the maximum; 0 is dead. a_isDead is the game's
	// IsDead, true while the player is on the ground too; a_onGround is Self::IsDown, down or helped and getting
	// up, and then the real health is told (at least 1). Helped and still on the ground, the player was told as
	// no longer down with no health: friends read "Host died", and "is back up" never came.
	inline std::uint8_t StatusHealth(bool a_isDead, bool a_onGround, float a_current, float a_maximum)
	{
		if ((a_isDead && !a_onGround) || a_maximum <= 0.0f) {
			return 0;
		}
		return static_cast<std::uint8_t>(std::clamp(std::lround(a_current / a_maximum * 100.0f), 1L, 100L));
	}

	// What a friend's new status tells us about them, against the one before.
	enum class FriendNews
	{
		kNone,
		kDown,    // "... is down!"
		kDied,    // "... died"
		kBackUp,  // "... is back up"
	};

	constexpr FriendNews ReadFriendStatus(bool a_wasDowned, std::uint8_t a_wasHealth, bool a_downed, std::uint8_t a_health)
	{
		const bool wasDead = a_wasHealth == 0 && !a_wasDowned;
		if (a_downed && !a_wasDowned) {
			return FriendNews::kDown;
		}
		if (a_health == 0 && !a_downed && !wasDead) {
			return FriendNews::kDied;
		}
		if (a_wasDowned && !a_downed && a_health > 0) {
			return FriendNews::kBackUp;
		}
		return FriendNews::kNone;
	}

	// What the game shows of our player this frame.
	struct Facts
	{
		bool enabled = false;    // in a session with friends, revive on, in the world
		bool essential = false;  // our essential flag is on the player's character
		bool down = false;       // the game has them down (IsDownState)
		bool dead = false;       // dead or dying
		bool paused = false;     // a menu has the game stopped (the pause menu, the Pip-Boy)
		bool inCombat = false;   // the player is in a fight (revives per fight)
		bool console = false;    // the console is open, or was a moment ago (Downed.cpp): a stand-up then is the player's command
		bool knockedOver = false;  // the knock state is not normal: a ragdoll, or the get-up after it
		bool sneaking = false;     // the game has the player sneaking (Actor::IsSneaking)
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
		bool                lastRevive = false;    // (AfterRevive) helped up for the last time this window or fight
		bool                revivesBack = false;   // the window or the fight is over: a lethal hit knocks down again
		int                 cooldownSeconds = -1;  // (AfterRevive) for this long a lethal hit kills
		bool                stoodUp = false;       // the game stood the player up while they wait for help (once per time)
		bool                putDown = false;       // put them down again (the engine's own essential down)
		bool                backDown = false;      // down again after that
		bool                stayedUp = false;      // the game kept them up through PUT_DOWN_GIVE_UP of that: up it is
		bool                upByCommand = false;   // stood up while the console was open (player.resurrect and the like): up it is
		bool                standUp = false;       // helped up: stand them up now, the engine's own end of the down (Downed.cpp StandUp)
		bool                gotUp = false;         // up again after a down, however it ended: what the down left on is taken off
		bool                unsneak = false;       // up again, and still in the sneak they went down in: they stand (as they got up, and once more if that did not take)
		bool                sneakKept = false;     // up again sneaking, though they went down standing: the sneak key pressed while down, theirs
		bool                settled = false;       // GET_UP_SETTLE after getting up: the get-up has played out
	};

	// The player's own down state. Up; down and waiting for help; helped and getting up; or bled out (essential
	// off and killed, waiting to be dead). A bled-out player is not made essential again while still alive: the
	// kill had not taken by the next frame, essential went back on, and the player was "down!" again instead of
	// dead, every 45 s for as long as they waited.
	//
	// The session's rules can limit it: out of revives for the window (or the fight), or within the cooldown after
	// being helped up, the player is not made essential, and a lethal hit kills as it would alone.
	//
	// Helped up (or a save loaded with them down), the player is stood up the engine's own way at once (standUp), not left
	// to its recovery a few seconds on, when other paths of the game could stand them up first and leave part of the down
	// on: once a knock-down's ragdoll and its get-up are over (KNOCK_WAIT at most), never in a stopped game, and again every
	// STAND_RETRY while it has not taken. However a down ends with the player up (helped, stood up by the game, a bleed-out
	// the kill did not take, a load), what the down left on is taken off (gotUp). The down's start and end leave the sneak
	// stance alone, so a player downed sneaking got up crouched and stayed so until they pressed the key again: one who went
	// down sneaking (as last seen up; a save loaded down counts so) and is sneaking as they get up is stood (unsneak), and
	// once more when the get-up has played out (settled, GET_UP_SETTLE on) only if that did not take, the stance never
	// having left sneak since. A sneak the player chose is theirs: one they went down standing and are sneaking as they get
	// up pressed the key while down (sneakKept), and a change of the stance after they are up is their key too.
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

		// The session's rules, and a new count. Rules that come while the player is on the ground wait until they are
		// up: a down finishes under the rules it began with (a shorter bleed-out would kill at once, revive off too).
		void SetRules(const Rules& a_rules)
		{
			Take(a_rules);
			countedFor = 0;
			helpedUpAt.clear();
			fightRevives = 0;
			upAt.reset();
			outOfRevives = false;
		}

		// The session's rules as a welcome brings them (a_session: the server's session ID; 0, out of a session). The revive
		// count, its window and the cooldown are the session's: they start over in another session, and carry on through a
		// welcome back into the same one (a dropped link, a quit to the menu and a join again), so a rejoin gives no revive
		// the host's limit doesn't. Out of a session they are kept for a return.
		void SetRules(const Rules& a_rules, std::uint64_t a_session)
		{
			if (a_session == 0 || a_session == countedFor) {
				Take(a_rules);
				return;
			}
			SetRules(a_rules);
			countedFor = a_session;
		}

		// The rules in force now (not ones waiting for the player to be up).
		[[nodiscard]] const Rules& GetRules() const { return rules; }
		[[nodiscard]] bool RulesWaiting() const { return pending.has_value(); }

		Step Frame(const Facts& a_facts, Clock::time_point a_now)
		{
			Step        step;
			const Phase before = phase;
			if (pending && !IsDown()) {
				rules = *pending;
				pending.reset();
			}
			// A fight goes on while the player is on the ground: enemies may well leave a downed player alone.
			if (a_facts.inCombat || IsDown()) {
				lastFight = a_now;
			}
			if (rules.window.count() == 0 && fightRevives > 0 && a_now - lastFight >= FIGHT_OVER) {
				fightRevives = 0;
			}
			while (rules.window.count() > 0 && !helpedUpAt.empty() && a_now - helpedUpAt.front() >= rules.window) {
				helpedUpAt.pop_front();
			}
			const bool out = RevivesLeft(a_now) == 0;
			step.revivesBack = outOfRevives && !out && a_facts.enabled;
			outOfRevives = out;
			const bool mayGoDown = a_facts.enabled && !out && !InCooldown(a_now);
			switch (phase) {
			case Phase::kUp:
				// Not while dying: making a dying character essential again could stop the death.
				if (!a_facts.dead || !mayGoDown) {
					step.essential = mayGoDown;
				}
				if (a_facts.essential && a_facts.down && !a_facts.dead) {
					phase = Phase::kDown;
					giveUp = false;
					downSince = a_now;
					// A reminder needs time before it: no "30 seconds left" when there were only 30 to start with.
					reminders = rules.bleedOut <= 15s ? 2 : rules.bleedOut <= 35s ? 1 : 0;
					++downs;
					// The game would stand the player up again by itself after a while.
					step.stayDown = true;
					step.wentDown = true;
				}
				break;
			case Phase::kDown:
				if (!a_facts.down) {
					StoodUp(a_facts, step, a_now);
					break;
				}
				if (standingSince) {
					step.backDown = true;  // the put-down took
					standingSince.reset();
					lastPutDown.reset();
				}
				if (giveUp || !a_facts.enabled || a_now - downSince >= rules.bleedOut) {
					// Bleeding out counts real time, paused or not: friends' games go on while ours is stopped.
					BleedOut(step, a_now);
				} else {
					// A save clears both flags (Downed::OnBeforeSave); still down, so set them again.
					step.essential = true;
					step.stayDown = true;
					const auto left = std::chrono::duration_cast<std::chrono::seconds>(rules.bleedOut - (a_now - downSince)).count();
					if ((reminders == 0 && left <= 30) || (reminders == 1 && left <= 10)) {
						++reminders;
						step.secondsLeft = static_cast<int>(left);
					}
				}
				break;
			case Phase::kGettingUp:
				// A stopped game stands nobody up, so time in a menu does not count toward GET_UP_TIME: a player
				// who opened the pause menu right after being helped was killed there 20 s later.
				if (a_facts.paused) {
					pausedGettingUp += a_now - (std::max)(lastFrame, revivedAt);
				}
				if (!a_facts.down) {
					phase = Phase::kUp;
					step.stayDown = false;
					step.trimHealth = true;
					upAt = a_now;
				} else if (a_now - revivedAt - pausedGettingUp >= GET_UP_TIME) {
					// Helped up, and neither our stand-up nor the game's own took. They die.
					BleedOut(step, a_now);
				} else {
					// Essential until they stand, even across a save.
					step.essential = true;
					// The knock-down's get-up first (the bleed-out's played into a ragdoll does not take), then ours.
					const bool knockWait = a_facts.knockedOver && a_now - revivedAt - pausedGettingUp < KNOCK_WAIT;
					if (!a_facts.paused && !knockWait && (!standAskedAt || a_now - *standAskedAt >= STAND_RETRY)) {
						step.standUp = true;
						standAskedAt = a_now;
						++standUpAsks;
						++standUpAsksNow;
					}
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
			GettingUp(before, a_facts, step, a_now);
			lastFrame = a_now;
			return step;
		}

		// A friend helped us up: true when that stands us up (only once per down; a second helper, or the
		// same one again, changes nothing).
		bool Revive(Clock::time_point a_now)
		{
			if (phase != Phase::kDown) {
				return false;
			}
			standingSince.reset();
			lastPutDown.reset();
			phase = Phase::kGettingUp;
			revivedAt = a_now;
			pausedGettingUp = {};
			standAskedAt.reset();
			standUpAsksNow = 0;
			++revives;
			helpedUpAt.push_back(a_now);
			++fightRevives;
			return true;
		}

		// After a Revive that stood us up: what the player is told of the rules (the last revive, the cooldown).
		[[nodiscard]] Step AfterRevive(Clock::time_point a_now) const
		{
			Step step;
			step.lastRevive = RevivesLeft(a_now) == 0;
			if (rules.cooldown.count() > 0) {
				step.cooldownSeconds = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(rules.cooldown).count());
			}
			return step;
		}

		// How many more times the player can be helped up now; -1 = no limit.
		[[nodiscard]] int RevivesLeft(Clock::time_point a_now) const
		{
			if (rules.maxRevives == 0) {
				return -1;
			}
			auto used = fightRevives;
			if (rules.window.count() > 0) {
				used = static_cast<std::uint32_t>(std::ranges::count_if(helpedUpAt, [&](Clock::time_point a_at) { return a_now - a_at < rules.window; }));
			}
			return used >= rules.maxRevives ? 0 : static_cast<int>(rules.maxRevives - used);
		}

		// Stood up after a help-up a moment ago: a lethal hit kills.
		[[nodiscard]] bool InCooldown(Clock::time_point a_now) const
		{
			return rules.cooldown.count() > 0 && upAt && a_now - *upAt < rules.cooldown;
		}

		void GiveUp() { giveUp = phase == Phase::kDown; }

		// A save was loaded: whatever state we were in belongs to the old game. True when the player is down in
		// it (a save made while down): they are stood up as if helped.
		bool Loaded(bool a_down, Clock::time_point a_now)
		{
			phase = Phase::kUp;
			giveUp = false;
			standingSince.reset();
			lastPutDown.reset();
			standAskedAt.reset();
			upSince.reset();  // a get-up of the old game's is nothing to this one
			if (!a_down) {
				return false;
			}
			phase = Phase::kGettingUp;
			revivedAt = a_now;
			pausedGettingUp = {};
			standUpAsksNow = 0;
			downSneaking = true;  // the down began in the saved game: a sneak it has is from before it
			return true;
		}

		[[nodiscard]] Phase Now() const { return phase; }
		// On the ground: down, or helped and getting up (friends are told the real health, DownRules::StatusHealth).
		[[nodiscard]] bool IsDown() const { return phase == Phase::kDown || phase == Phase::kGettingUp; }
		// What friends are told: down and still waiting for help. Once helped, friends stop helping; they kept
		// helping while the game stood the player up, and the helper read "You helped Host up" three times for one down.
		[[nodiscard]] bool WaitsForHelp() const { return phase == Phase::kDown; }
		// The session's rule lets the player crawl, and they are down waiting for help (not helped and getting up, not
		// bled out, not giving up), and really lying there: not while the game has them up and they are being put down
		// again (StoodUp), when the engine walks them itself and a step of ours could keep the put-down from taking.
		[[nodiscard]] bool MayCrawl() const { return rules.crawl && phase == Phase::kDown && !giveUp && !standingSince; }

		std::uint32_t downs = 0;
		std::uint32_t revives = 0;
		std::uint32_t bleedOuts = 0;
		std::uint32_t killRetries = 0;
		std::uint32_t standUps = 0;  // times the game stood the player up while they waited for help
		std::uint32_t putDowns = 0;  // times they were put down again for it
		std::uint32_t standUpAsks = 0;  // times the engine's get-up was asked for (standUp)
		std::uint32_t standUpAsksNow = 0;  // of those, this get-up's (from the help-up or the load)
		std::uint32_t getUps = 0;       // times up again after a down (gotUp)
		std::uint32_t unsneaks = 0;     // stands asked for after those (unsneak)

	private:
		// Up again after a down, however it ended (a_before: the phase as the frame began): told once (gotUp), and once the
		// get-up has played out (settled). Dead, or down again, before that: nothing to finish. The sneak: see the class.
		void GettingUp(Phase a_before, const Facts& a_facts, Step& a_step, Clock::time_point a_now)
		{
			if (phase != Phase::kUp) {
				if (a_before == Phase::kUp) {
					// The stance they went down in, as last seen up: while down the game's answer may not be the stance.
					downSneaking = sneakingUp;
				}
				upSince.reset();
				return;
			}
			if (!a_facts.down && !a_facts.dead) {
				sneakingUp = a_facts.sneaking;
			}
			if (a_before != Phase::kUp) {
				if (a_facts.dead) {
					return;
				}
				upSince = a_now;
				a_step.gotUp = true;
				a_step.unsneak = a_facts.sneaking && downSneaking;
				a_step.sneakKept = a_facts.sneaking && !downSneaking;
				unsneaks += a_step.unsneak ? 1u : 0u;
				++getUps;
				sneakAsked = a_step.unsneak;
				sneakSeen = a_facts.sneaking;
				sneakPressed = false;
				return;
			}
			if (!upSince) {
				return;
			}
			if (a_facts.dead || a_facts.down) {
				upSince.reset();
				return;
			}
			// A change of the stance since they got up is the player's key, but for the stand we asked for taking.
			if (a_facts.sneaking != sneakSeen) {
				sneakPressed = sneakPressed || a_facts.sneaking || !sneakAsked;
				sneakSeen = a_facts.sneaking;
			}
			if (!a_facts.knockedOver && !a_facts.paused && a_now - *upSince >= GET_UP_SETTLE) {
				upSince.reset();
				a_step.settled = true;
				a_step.unsneak = a_facts.sneaking && sneakAsked && !sneakPressed;
				unsneaks += a_step.unsneak ? 1u : 0u;
			}
		}

		// Down and waiting for help, neither helped up nor bled out, and the game has the player up again. The game
		// stands an essential character up by itself on several paths the stay-down flag does not hold back (seen
		// 2026-10-19: downed in a pile of clutter, the host was stood up and their revive state lost). They are put
		// down again, the engine's own way, every PUT_DOWN_RETRY, the bleed-out time running on, until they are down
		// or PUT_DOWN_GIVE_UP has gone by. Not when the session is over (up is right then), and not while they are
		// dying or dead: that is waited out (a hit the game is still turning into a down, or a real death). Nor when the
		// console was open: the player stood themselves up (player.resurrect, a heal), and up it is at once.
		void StoodUp(const Facts& a_facts, Step& a_step, Clock::time_point a_now)
		{
			if (!a_facts.enabled) {
				phase = Phase::kUp;
				a_step.stayDown = false;
				standingSince.reset();
				lastPutDown.reset();
				return;
			}
			if (a_facts.console && !a_facts.dead) {
				phase = Phase::kUp;
				a_step.stayDown = false;
				a_step.upByCommand = true;
				standingSince.reset();
				lastPutDown.reset();
				return;
			}
			if (!standingSince) {
				standingSince = a_now;
				standUpTold = false;
			}
			if (!a_facts.dead && !standUpTold) {
				standUpTold = true;
				a_step.stoodUp = true;
				++standUps;
			}
			if (giveUp || a_now - downSince >= rules.bleedOut) {
				standingSince.reset();
				lastPutDown.reset();
				BleedOut(a_step, a_now);
				return;
			}
			if (a_now - *standingSince >= PUT_DOWN_GIVE_UP) {
				phase = Phase::kUp;
				a_step.stayDown = false;
				a_step.stayedUp = !a_facts.dead;
				standingSince.reset();
				lastPutDown.reset();
				return;
			}
			if (a_facts.dead) {
				return;
			}
			a_step.essential = true;
			a_step.stayDown = true;
			if (!lastPutDown || a_now - *lastPutDown >= PUT_DOWN_RETRY) {
				a_step.putDown = true;
				lastPutDown = a_now;
				++putDowns;
			}
		}

		void Take(const Rules& a_rules)
		{
			if (IsDown()) {
				pending = a_rules;
			} else {
				rules = a_rules;
				pending.reset();
			}
		}

		void BleedOut(Step& a_step, Clock::time_point a_now)
		{
			phase = Phase::kBledOut;
			a_step.stayDown = false;
			a_step.essential = false;
			a_step.kill = true;
			killedAt = a_now;
			++bleedOuts;
		}

		Rules                            rules;
		std::optional<Rules>             pending;     // came while on the ground: in force once up (Take)
		std::deque<Clock::time_point>    helpedUpAt;  // help-ups within the window (revives per window)
		std::uint32_t                    fightRevives = 0;
		Clock::time_point                lastFight{};
		std::optional<Clock::time_point> upAt;  // stood up after the last help-up (the cooldown)
		bool                             outOfRevives = false;
		std::uint64_t                    countedFor = 0;  // the session the count is for (0: none named)

		Phase             phase = Phase::kUp;
		bool              giveUp = false;
		Clock::time_point downSince{};
		Clock::time_point revivedAt{};
		Clock::duration   pausedGettingUp{};  // of the time since revivedAt, how long the game was paused
		Clock::time_point lastFrame{};
		Clock::time_point killedAt{};
		int               reminders = 0;
		std::optional<Clock::time_point> standingSince;  // down and waiting, but the game has them up (StoodUp)
		std::optional<Clock::time_point> lastPutDown;
		bool                             standUpTold = false;  // this time up has been told (stoodUp)
		std::optional<Clock::time_point> standAskedAt;  // getting up: when the engine's get-up was last asked for (standUp)
		std::optional<Clock::time_point> upSince;       // up again after a down, and the get-up not yet played out (GettingUp)
		bool                             sneakingUp = false;    // sneaking as last seen up (GettingUp): what a down that begins began in
		bool                             downSneaking = false;  // this down began sneaking (or came in a loaded save)
		bool                             sneakAsked = false;    // up again: we stood them as they got up
		bool                             sneakSeen = false;     // up again: the stance as last seen since
		bool                             sneakPressed = false;  // up again: the stance changed since by the player's key
	};

	// Helping downed friends up: stay within HELP_RANGE of one's stand-in for the rules' help time (HELP_TIME), and,
	// when the rules say so, not while in a fight.
	class Helper
	{
	public:
		void SetRules(const Rules& a_rules) { rules = a_rules; }

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
			std::uint32_t blocked = 0;  // next to this friend, but in a fight ("Can't help ... up during a fight")
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
			blockedFor = 0;
		}

		// a_able: we are up, alive and in a session with revive on. a_near: friends' stand-ins in our world.
		// a_fighting: we are in a fight (helping waits for its end when the rules say so).
		HelpStep Frame(bool a_able, std::span<const Near> a_near, Clock::time_point a_now, bool a_fighting = false)
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
			if (near && rules.outsideCombat && a_fighting) {
				if (near != blockedFor) {
					step.blocked = near;
				}
				blockedFor = near;
				near = 0;
			} else {
				blockedFor = 0;
			}
			if (near != helping) {
				helping = near;
				helpSince = a_now;
				step.started = near;
			} else if (near && a_now - helpSince >= rules.helpTime) {
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
		Rules                   rules;
		std::set<std::uint32_t> friendsDown;
		std::uint32_t           helping = 0;
		std::uint32_t           blockedFor = 0;  // told we can't help this friend during the fight (once, not every frame)
		Clock::time_point       helpSince{};
		Clock::time_point       helpAgain{};
	};

	// Crawling while down (the session's rule, bDownedCrawl): the player's own movement input, read from the game's
	// controls, moves their character slowly in their own game; friends see it through the position they already get,
	// their stand-in lying where the player is (StandInDown). Forward and back crawl along the heading and sideways turns
	// the body, since the bleed-out camera gives no direction to go by. Each step is checked against the world first
	// (Accept): Downed.cpp casts the rays through the cell's physics and does what this answers.
	inline constexpr float CRAWL_SPEED = 40.0f;           // game units a second forward (half that backward): 25 m in a 45 s bleed-out
	inline constexpr float CRAWL_TURN = 1.5707964f;       // radians a second (90 degrees)
	inline constexpr float CRAWL_LONGEST_FRAME = 0.1f;    // seconds: a hitch moves no farther than this much crawl
	inline constexpr float CRAWL_DEAD_ZONE = 0.15f;       // input under this is none (a pad's stick at rest drifts)
	inline constexpr float CRAWL_BODY = 20.0f;            // something solid closer than this past the step is a wall
	inline constexpr float CRAWL_AHEAD = 20.0f;           // the ground is judged this far ahead too, so a slope reads as one
	inline constexpr float CRAWL_CLIMB = 30.0f;           // the most the ground may rise there (a stair's step, a 55 degree slope)
	inline constexpr float CRAWL_DROP = 45.0f;            // the most it may fall there: more is a ledge
	inline constexpr float CRAWL_PROBE_LOW = 35.0f;       // the low wall ray and the ground rays start this high above the feet
	inline constexpr float CRAWL_PROBE_HIGH = 60.0f;      // the high wall ray (under a table is still a way)
	inline constexpr float CRAWL_GROUND_REACH = 120.0f;   // the ground rays reach this far below the feet

	// What stops a crawl, by the collision layer (the engine's COL_LAYER) of what a ray hit: the world's own solid
	// things. Characters (our own body first: the rays start inside it), loose clutter, water, triggers and the like are
	// passed through.
	constexpr bool CrawlStops(std::uint32_t a_layer)
	{
		switch (a_layer) {
		case 0:   // unidentified: a hit that can't be told is taken as solid
		case 1:   // static
		case 2:   // animated static
		case 3:   // transparent (glass, a fence)
		case 9:   // trees
		case 10:  // props
		case 13:  // terrain
		case 17:  // ground
		case 20:  // large debris
		case 26:  // small transparent (a railing, a grate, a small fence)
		case 27:  // invisible wall
		case 28:  // small transparent, animated
		case 29:  // large clutter
			return true;
		default:
			return false;
		}
	}

	class Crawl
	{
	public:
		// What the game shows this frame.
		struct Facts
		{
			bool  may = false;          // the session's rule, down and waiting for help (Self::MayCrawl)
			float x = 0.0f;             // the movement input, -1 (left) to 1 (right)
			float y = 0.0f;             // -1 (back) to 1 (forward)
			float seconds = 0.0f;       // since the last frame
			bool  paused = false;       // a menu has the game stopped
			bool  inAir = false;
			bool  swimming = false;
			bool  inFurniture = false;
			bool  knockedOver = false;  // the knock state is not normal: a ragdoll, or the get-up after it
			bool  inScene = false;
			bool  friendNear = false;   // a friend's stand-in within HELP_RANGE: kept still, so their help isn't broken
		};

		struct Move
		{
			float turn = 0.0f;     // radians, to the right
			float forward = 0.0f;  // game units along the heading after the turn; backward is negative

			[[nodiscard]] bool Any() const { return turn != 0.0f || forward != 0.0f; }
		};

		// The input is held at all (worth a look for friends near).
		static bool Pushed(float a_x, float a_y)
		{
			return std::fabs(a_x) >= CRAWL_DEAD_ZONE || std::fabs(a_y) >= CRAWL_DEAD_ZONE;
		}

		static Move Wanted(const Facts& a_facts)
		{
			if (!a_facts.may || a_facts.paused || a_facts.inAir || a_facts.swimming || a_facts.inFurniture || a_facts.knockedOver ||
				a_facts.inScene || a_facts.friendNear) {
				return {};
			}
			const float seconds = std::isfinite(a_facts.seconds) ? std::clamp(a_facts.seconds, 0.0f, CRAWL_LONGEST_FRAME) : 0.0f;
			const auto  axis = [](float a_value) {
				return std::isfinite(a_value) && std::fabs(a_value) >= CRAWL_DEAD_ZONE ? std::clamp(a_value, -1.0f, 1.0f) : 0.0f;
			};
			const float x = axis(a_facts.x);
			const float y = axis(a_facts.y);
			Move move;
			move.turn = x * CRAWL_TURN * seconds;
			move.forward = y * CRAWL_SPEED * seconds * (y < 0.0f ? 0.5f : 1.0f);
			return move;
		}

		// The heading turned by a_turn, within 0 to 2 pi.
		static float Turned(float a_heading, float a_turn)
		{
			constexpr float FULL = 6.2831853f;
			const float     heading = std::fmod(a_heading + a_turn, FULL);
			return heading < 0.0f ? heading + FULL : heading;
		}

		// How far east (x) and north (y) a_distance along a_heading is: the engine's heading counts from north toward east.
		static std::array<float, 2> Along(float a_heading, float a_distance)
		{
			return { std::sin(a_heading) * a_distance, std::cos(a_heading) * a_distance };
		}

		// What the rays found for a step, each nothing when they hit nothing that stops a crawl (CrawlStops).
		struct Probe
		{
			std::optional<float> wall;         // how far along the step's way something solid is, from the feet, at either height
			std::optional<float> groundThere;  // the height of the ground where the step ends
			std::optional<float> groundAhead;  // and CRAWL_AHEAD along the way from the feet
		};

		enum class Refused
		{
			kNone,
			kWall,
			kNoGround,
			kLedge,
			kClimb,
		};

		struct Verdict
		{
			Refused refused = Refused::kNone;
			float   z = 0.0f;  // taken: the height to put the feet at, on the ground
		};

		// A step of a_step units (negative: backward) from feet at a_fromZ.
		static Verdict Accept(float a_fromZ, float a_step, const Probe& a_probe)
		{
			if (a_probe.wall && *a_probe.wall < std::fabs(a_step) + CRAWL_BODY) {
				return { Refused::kWall };
			}
			if (!a_probe.groundThere || !a_probe.groundAhead || !std::isfinite(*a_probe.groundThere) || !std::isfinite(*a_probe.groundAhead)) {
				return { Refused::kNoGround };
			}
			for (const float ground : { *a_probe.groundThere, *a_probe.groundAhead }) {
				if (ground - a_fromZ > CRAWL_CLIMB) {
					return { Refused::kClimb };
				}
				if (a_fromZ - ground > CRAWL_DROP) {
					return { Refused::kLedge };
				}
			}
			return { Refused::kNone, *a_probe.groundThere };
		}

		// Counts a step's verdict. True for a refusal that is news (not the same as the step before it was), for the log.
		bool Count(const Verdict& a_verdict, float a_step)
		{
			const bool news = a_verdict.refused != Refused::kNone && a_verdict.refused != last;
			last = a_verdict.refused;
			if (a_verdict.refused == Refused::kNone) {
				++steps;
				distance += std::fabs(a_step);
			} else {
				++refused;
			}
			return news;
		}

		[[nodiscard]] Refused Last() const { return last; }

		std::uint32_t steps = 0;
		std::uint32_t refused = 0;
		float         distance = 0.0f;  // game units crawled in all

	private:
		Refused last = Refused::kNone;
	};

	constexpr const char* RefusedName(Crawl::Refused a_refused)
	{
		switch (a_refused) {
		case Crawl::Refused::kWall:
			return "wall";
		case Crawl::Refused::kNoGround:
			return "no ground";
		case Crawl::Refused::kLedge:
			return "ledge";
		case Crawl::Refused::kClimb:
			return "too steep";
		default:
			return "none";
		}
	}

	// A friend who is down is shown down: their stand-in lies where they are, in the engine's own downed pose (its life
	// state put to essential-down, which plays its graph's bleedout, in a power armor frame too), held there until they
	// are up again, and then it gets up (alive again, which plays the get-up). It stood or walked about before, crouched,
	// while the friend lay elsewhere in their own game, and in a frame not even that. Once a frame per stand-in:
	// a_friendDown, their status says they are down; a_ready, it can lie down now (its 3D in, no furniture or frame switch
	// under way); a_lying, its life state is a down one.
	class StandInDown
	{
	public:
		static constexpr auto LIE_RETRY = 2s;       // laid down and found up again while its friend is down: laid down again then
		static constexpr auto GET_UP_LIMIT = 4s;    // told to get up and still down this long after: placed again

		enum class Do
		{
			kNone,
			kLieDown,  // put its life state down and hold it there
			kGetUp,    // put its life state back to alive and drive it again
			kReplace,  // it would not get up: a fresh stand-in, standing
		};

		Do Frame(bool a_friendDown, bool a_ready, bool a_lying, Clock::time_point a_now)
		{
			switch (phase) {
			case Phase::kUp:
			case Phase::kGettingUp:
				if (a_friendDown) {
					if (!a_ready || (triedAt && a_now - *triedAt < LIE_RETRY)) {
						return Do::kNone;
					}
					phase = Phase::kLying;
					triedAt = a_now;
					++lieDowns;
					return Do::kLieDown;
				}
				if (phase == Phase::kGettingUp) {
					if (!a_lying) {
						phase = Phase::kUp;
					} else if (a_now - gotUpAt >= GET_UP_LIMIT) {
						phase = Phase::kUp;
						return Do::kReplace;
					}
				}
				return Do::kNone;
			case Phase::kLying:
				if (!a_friendDown) {
					phase = Phase::kGettingUp;
					gotUpAt = a_now;
					triedAt.reset();
					return Do::kGetUp;
				}
				if (!a_lying) {
					// Something here stood it up (the engine's own recovery): laid down again after LIE_RETRY.
					phase = Phase::kUp;
					++stoodUp;
				}
				return Do::kNone;
			}
			return Do::kNone;
		}

		// Its down life state is ours (laid down, or told to get up and not up yet): it is not broken for it (StandInBroken).
		[[nodiscard]] bool Laid() const { return phase != Phase::kUp; }
		[[nodiscard]] bool Lying() const { return phase == Phase::kLying; }

		std::uint32_t lieDowns = 0;  // times laid down
		std::uint32_t stoodUp = 0;   // of those, found up again while its friend was still down

	private:
		enum class Phase
		{
			kUp,
			kLying,
			kGettingUp,
		};
		Phase                            phase = Phase::kUp;
		std::optional<Clock::time_point> triedAt;
		Clock::time_point                gotUpAt{};
	};
}
