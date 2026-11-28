#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The friend bot: test tooling for the dev channel (bDevChannel), off unless a dev command turns it on, never a
// player's feature. In a friend's game it drives our own player the way a companion moves, so one person (or the test
// rig) can play quests as two players: it follows the host on the navmesh, goes through a load door after the host,
// fights what is fighting the two of them, and waits when told. Its movement is the engine's own: the player is
// AI-driven (Game.SetPlayerAIDriven) and given a package made at run time, as the engine itself does for the player
// (FriendBot.cpp says which functions); a fight is the virtual pad's sticks and trigger. Never a teleport.
//
// What it decides from what the game says each frame is here, free of the game, so it is unit tested: the dev command's
// words, the state it is in and what to do about it, which load door the host took, which enemy to fight, and how the
// right stick aims.
namespace FriendBotPlan
{
	using Clock = std::chrono::steady_clock;

	inline constexpr std::string_view USAGE =
		"usage: friendbot on [playerId] | off | wait | follow | mode follow|travel | status";

	// The package the player follows the host on: the engine's Follow procedure (the default), or a Travel package to the
	// host's stand-in, made again as the host moves (for a rig run, if Follow does not run on the player).
	enum class Mode : std::uint8_t
	{
		kFollow,
		kTravel,
	};

	inline std::string_view Name(Mode a_mode)
	{
		return a_mode == Mode::kFollow ? "follow" : "travel";
	}

	struct Command
	{
		enum class Kind : std::uint8_t
		{
			kStatus,
			kOn,
			kOff,
			kWait,
			kFollow,
			kMode,
		};
		Kind          kind = Kind::kStatus;
		std::uint32_t player = 0;  // kOn: who to follow (0: the first other player, the host in a game of two)
		Mode          mode = Mode::kFollow;
	};

	inline std::vector<std::string_view> Words(std::string_view a_text)
	{
		std::vector<std::string_view> out;
		std::size_t                   at = 0;
		while (at < a_text.size()) {
			while (at < a_text.size() && (a_text[at] == ' ' || a_text[at] == '\t')) {
				++at;
			}
			const auto start = at;
			while (at < a_text.size() && a_text[at] != ' ' && a_text[at] != '\t') {
				++at;
			}
			if (at > start) {
				out.push_back(a_text.substr(start, at - start));
			}
		}
		return out;
	}

	inline std::variant<Command, std::string> Parse(std::string_view a_args)
	{
		const auto words = Words(a_args);
		const auto error = [](std::string_view a_what) { return std::format("error: {}; {}", a_what, USAGE); };
		Command    command;
		if (words.empty() || words[0] == "status") {
			if (words.size() > 1) {
				return error("too many words");
			}
			return command;
		}
		const auto verb = words[0];
		if (verb == "on") {
			command.kind = Command::Kind::kOn;
			if (words.size() > 2) {
				return error("too many words");
			}
			if (words.size() == 2) {
				const auto word = words[1];
				std::uint32_t id = 0;
				const auto [end, ec] = std::from_chars(word.data(), word.data() + word.size(), id);
				if (ec != std::errc{} || end != word.data() + word.size() || id == 0) {
					return error(std::format("'{}' is not a player id", word));
				}
				command.player = id;
			}
			return command;
		}
		if (verb == "off" || verb == "wait" || verb == "follow") {
			if (words.size() != 1) {
				return error("too many words");
			}
			command.kind = verb == "off" ? Command::Kind::kOff : verb == "wait" ? Command::Kind::kWait : Command::Kind::kFollow;
			return command;
		}
		if (verb == "mode") {
			if (words.size() != 2 || (words[1] != "follow" && words[1] != "travel")) {
				return error("mode takes follow or travel");
			}
			command.kind = Command::Kind::kMode;
			command.mode = words[1] == "follow" ? Mode::kFollow : Mode::kTravel;
			return command;
		}
		return error(std::format("no friendbot command '{}'", verb));
	}

	// ---- places and doors ----

	// Where someone is, as PlayerState says it: an interior cell, or a worldspace (0 for the other).
	struct Place
	{
		std::uint32_t cell = 0;
		std::uint32_t worldspace = 0;

		[[nodiscard]] bool Known() const { return cell != 0 || worldspace != 0; }
		friend bool operator==(const Place&, const Place&) = default;
	};

	struct Point
	{
		float x = 0.0f, y = 0.0f, z = 0.0f;
	};

	inline float Distance(const Point& a_a, const Point& a_b)
	{
		return std::sqrt((a_a.x - a_b.x) * (a_a.x - a_b.x) + (a_a.y - a_b.y) * (a_a.y - a_b.y) + (a_a.z - a_b.z) * (a_a.z - a_b.z));
	}

	// A load door in our loaded area and where it leads (its ExtraTeleport's linked door's place).
	struct Door
	{
		std::uint32_t ref = 0;
		Point         at;
		Place         to;
	};

	// How far from where the host was last seen here a door may be and still be the one they took, when none leads to
	// the place they are in now (a door whose far side our game names otherwise, or a place a door leads to by a script).
	inline constexpr float DOOR_FALLBACK_RADIUS = 1000.0f;

	// The door the host took: of those that lead where the host is now, the nearest to where they were last seen here;
	// failing that, the nearest load door within DOOR_FALLBACK_RADIUS of that spot. None when nothing fits (fast travel,
	// a script's move): the bot then waits for the host to come back or for a command.
	inline std::optional<std::size_t> PickDoor(const std::vector<Door>& a_doors, const Place& a_hostNow, const Point& a_lastSeen)
	{
		std::optional<std::size_t> best;
		float                      bestDistance = 0.0f;
		for (std::size_t i = 0; i < a_doors.size(); ++i) {
			if (!(a_doors[i].to == a_hostNow)) {
				continue;
			}
			const float d = Distance(a_doors[i].at, a_lastSeen);
			if (!best || d < bestDistance) {
				best = i;
				bestDistance = d;
			}
		}
		if (best) {
			return best;
		}
		for (std::size_t i = 0; i < a_doors.size(); ++i) {
			const float d = Distance(a_doors[i].at, a_lastSeen);
			if (d <= DOOR_FALLBACK_RADIUS && (!best || d < bestDistance)) {
				best = i;
				bestDistance = d;
			}
		}
		return best;
	}

	// ---- enemies ----

	// An actor near us the bot might fight.
	struct Candidate
	{
		std::uint32_t ref = 0;
		float         toUs = 0.0f;    // units from our player
		float         toHost = 0.0f;  // units from the host's stand-in (toUs when there is none)
		bool          alive = false;
		bool          hostile = false;   // hostile to our player (Actor::GetHostileToActor)
		bool          inCombat = false;  // in combat now: fighting the two of us, not just a raider asleep in a chair
		bool          seen = false;      // in our line of sight (PlayerCharacter::HasLOSToTarget)
	};

	// How near an enemy must be, to us or to the host, to be fought.
	inline constexpr float FIGHT_RANGE = 3000.0f;

	// What the bot fights: the nearest to us of the live, hostile actors in combat that we can see, within FIGHT_RANGE of
	// us or of the host. The friend's game knows only that much of what the host fights: an enemy in combat near the two
	// of them is fighting one of them.
	inline std::optional<std::size_t> PickTarget(const std::vector<Candidate>& a_candidates)
	{
		std::optional<std::size_t> best;
		for (std::size_t i = 0; i < a_candidates.size(); ++i) {
			const auto& c = a_candidates[i];
			if (!c.alive || !c.hostile || !c.inCombat || !c.seen || std::min(c.toUs, c.toHost) > FIGHT_RANGE) {
				continue;
			}
			if (!best || c.toUs < a_candidates[*best].toUs) {
				best = i;
			}
		}
		return best;
	}

	// ---- aiming with the right stick ----

	struct AimSettings
	{
		float full = 30.0f;      // degrees off at which the stick is pushed all the way
		float push = 0.8f;       // the stick's furthest push
		float deadband = 0.5f;   // degrees: closer than this, no push
		float fireWithin = 4.0f; // degrees: the trigger is held while both errors are within this
	};

	struct Aim
	{
		float rightX = 0.0f;  // + turns right
		float rightY = 0.0f;  // + looks up
		bool  fire = false;
	};

	// The game's pitch (data.angle.x, in degrees) to look at something a_dz above us, a_horizontal away: the game's pitch
	// grows looking down, so up is negative.
	inline float PitchTo(float a_dz, float a_horizontal)
	{
		return -std::atan2(a_dz, std::max(a_horizontal, 1.0f)) * 180.0f / std::numbers::pi_v<float>;
	}

	// One frame's right stick toward a target: a_yawOff is the target's bearing less our heading (-180..180, + to the
	// right), a_pitchOff the pitch to it less ours (+ means look further down). Pushed in proportion to how far off, as
	// walkto turns, and the trigger held once the aim is close.
	inline Aim AimAt(float a_yawOff, float a_pitchOff, const AimSettings& a_settings = {})
	{
		Aim        out;
		const auto axis = [&](float a_off) {
			if (!std::isfinite(a_off) || std::abs(a_off) <= a_settings.deadband) {
				return 0.0f;
			}
			return std::clamp(a_off / a_settings.full, -1.0f, 1.0f) * a_settings.push;
		};
		out.rightX = axis(a_yawOff);
		out.rightY = -axis(a_pitchOff);
		out.fire = std::isfinite(a_yawOff) && std::isfinite(a_pitchOff) && std::abs(a_yawOff) <= a_settings.fireWithin && std::abs(a_pitchOff) <= a_settings.fireWithin;
		return out;
	}

	// ---- the bot's states ----

	enum class State : std::uint8_t
	{
		kOff,
		kFollow,  // on a package after the host's stand-in
		kWait,    // told to wait: stands where it is
		kDoor,    // the host went through a load door: on its way to it, then through it
		kFight,   // aiming and firing with the pad
		kPaused,  // on, but something keeps it from moving now (Facts::Pause says what): resumes by itself
	};

	inline std::string_view Name(State a_state)
	{
		switch (a_state) {
		case State::kOff:
			return "off";
		case State::kFollow:
			return "follow";
		case State::kWait:
			return "wait";
		case State::kDoor:
			return "door";
		case State::kFight:
			return "fight";
		case State::kPaused:
			return "paused";
		}
		return "?";
	}

	// What the game says this frame.
	struct Facts
	{
		bool inGame = false;     // a game is loaded and our player is in a cell
		bool loading = false;    // a loading screen is up
		bool menu = false;       // a menu that takes control is open (the bot never keeps the player AI-driven under one)
		bool dialogue = false;   // our player is in a conversation
		bool down = false;       // our player is dead or downed
		bool hostKnown = false;  // the player we follow is in the session
		Place ourPlace;
		Place hostPlace;           // where the host's latest state says they are
		bool  hostHere = false;    // their stand-in is in our loaded area
		std::uint32_t hostActor = 0;  // the stand-in's form id (it changes as the mod respawns it: a save, a load)
		Point hostAt;              // the stand-in's position (hostHere)
		Point us;                  // our player's position
		std::optional<std::uint32_t> target;  // the enemy PickTarget chose, if any
		std::optional<Door>          door;    // kDoor: the door picked when the host left (the glue picks it once, on leaving)
	};

	// What the game is to do this frame.
	enum class Action : std::uint8_t
	{
		kNone,
		kRelease,       // AI-driven off, the made package dropped, the pad let go
		kFollowHost,    // AI-driven on, a package after the host's stand-in (Mode says which)
		kTravelToDoor,  // AI-driven on, a Travel package to the door
		kUseDoor,       // let go, then the player activates the door (as pressing E: the load is the game's own)
		kFight,         // let go of the AI; the pad aims at the target and fires (every frame of a fight)
		kPickDoor,      // let go, and pick the door the host took (PickDoor) to hand back in Facts::door (asked again every DOOR_RETRY)
	};

	struct Decision
	{
		Action      action = Action::kNone;
		std::string log;  // a line for the plugin log when the state changes, or "" for none
	};

	// How long a fight goes on with no target seen, before the bot follows again.
	inline constexpr auto FIGHT_GRACE = std::chrono::seconds(3);
	// How close to a door it must come before activating it, and how long a use is given to load before another try.
	inline constexpr float DOOR_REACH = 160.0f;
	inline constexpr auto  DOOR_RETRY = std::chrono::seconds(6);
	inline constexpr int   DOOR_TRIES = 3;
	// A Travel package made again whenever the host has moved this far from where the last one was aimed, or this long
	// after it; a Follow package only when the stand-in changed, or this long after it (a package can end on its own).
	inline constexpr float TRAVEL_RENEW_DISTANCE = 256.0f;
	inline constexpr auto  TRAVEL_RENEW = std::chrono::seconds(3);
	inline constexpr auto  FOLLOW_RENEW = std::chrono::seconds(15);

	class Bot
	{
	public:
		// The user's commands. Each returns the line to log.
		std::string TurnOn(std::uint32_t a_player)
		{
			wanted = State::kFollow;
			player = a_player;
			Reset();
			return std::format("on, following {}", a_player == 0 ? std::string{ "the first other player" } : std::format("player {}", a_player));
		}
		std::string TurnOff()
		{
			wanted = State::kOff;
			return "off";
		}
		std::string Wait()
		{
			if (wanted == State::kOff) {
				return "off: not waiting";
			}
			wanted = State::kWait;
			return "waiting";
		}
		std::string Follow()
		{
			if (wanted == State::kOff) {
				return "off: friendbot on first";
			}
			wanted = State::kFollow;
			Reset();
			return "following";
		}
		std::string SetMode(Mode a_mode)
		{
			mode = a_mode;
			issued.reset();  // the next frame makes the new kind of package
			return std::format("mode {}", Name(a_mode));
		}

		// The package the player had is gone (a save or a load let go of it): the next frame makes another.
		void Renew() { issued.reset(); }

		[[nodiscard]] State Now() const { return state; }
		[[nodiscard]] State Wanted() const { return wanted; }
		[[nodiscard]] Mode Package() const { return mode; }
		[[nodiscard]] std::uint32_t Player() const { return player; }
		[[nodiscard]] const std::string& Pause() const { return pauseWhy; }
		[[nodiscard]] const std::optional<Door>& DoorTaken() const { return door; }
		[[nodiscard]] bool On() const { return wanted != State::kOff; }

		// One frame.
		Decision Step(Clock::time_point a_now, const Facts& a_facts)
		{
			if (wanted == State::kOff) {
				return Enter(State::kOff, Action::kRelease, "off");
			}
			if (const auto why = PauseReason(a_facts); !why.empty()) {
				// A door the host took is kept through the load it leads to (a loading screen is a pause): the cell is
				// compared again once it is over.
				pauseWhy = why;
				return Enter(State::kPaused, Action::kRelease, std::format("paused: {}", why));
			}
			pauseWhy.clear();
			if (wanted == State::kWait) {
				door.reset();
				return Enter(State::kWait, Action::kRelease, "waiting");
			}
			if (a_facts.hostHere) {
				lastSeen = a_facts.hostAt;
				seenHere = a_facts.ourPlace;
			}

			// A fight comes first, wherever the host is.
			if (a_facts.target) {
				lastTargetAt = a_now;
				const bool changed = state != State::kFight || fighting != *a_facts.target;
				fighting = *a_facts.target;
				state = State::kFight;
				issued.reset();
				return { Action::kFight, changed ? std::format("fight {:08X}", *a_facts.target) : std::string{} };
			}
			if (state == State::kFight && a_now - lastTargetAt < FIGHT_GRACE) {
				return { Action::kFight, {} };  // no target this frame: the pad is let go, and it looks again
			}

			// The host is somewhere else than we are: the door they took.
			if (a_facts.ourPlace.Known() && a_facts.hostPlace.Known() && !(a_facts.hostPlace == a_facts.ourPlace)) {
				if (door && arrivedFrom && !(a_facts.ourPlace == *arrivedFrom)) {
					// Through a door, but not yet where the host is (a door to a hall, the host already further on): the
					// next door from here.
					door.reset();
				}
				if (!door) {
					if (!a_facts.door) {
						if (!(seenHere == a_facts.ourPlace)) {
							pauseWhy = "the host is elsewhere and was never seen here";
							return Enter(State::kPaused, Action::kRelease, std::format("paused: {}", pauseWhy));
						}
						pauseWhy = "looking for the load door the host took";
						auto decision = Enter(State::kPaused, Action::kRelease, std::format("paused: {}", pauseWhy));
						if (!askedAt || a_now - *askedAt >= DOOR_RETRY) {
							askedAt = a_now;
							decision.action = Action::kPickDoor;
						}
						return decision;
					}
					door = a_facts.door;
					arrivedFrom = a_facts.ourPlace;
					tries = 0;
					usedAt.reset();
					state = State::kDoor;
					issued = a_now;
					return { Action::kTravelToDoor, std::format("door {:08X} to {} (cell {:08X} world {:08X}), {:.0f} away", door->ref,
														  door->to == a_facts.hostPlace ? "where the host is" : "near where the host was last seen", door->to.cell,
														  door->to.worldspace, Distance(door->at, a_facts.us)) };
				}
				state = State::kDoor;
				if (Distance(door->at, a_facts.us) > DOOR_REACH) {
					if (!issued || a_now - *issued >= FOLLOW_RENEW) {
						issued = a_now;
						return { Action::kTravelToDoor, {} };
					}
					return {};
				}
				if (!usedAt || a_now - *usedAt >= DOOR_RETRY) {
					if (tries >= DOOR_TRIES) {
						const auto ref = door->ref;
						door.reset();
						pauseWhy = std::format("door {:08X} did not take us through after {} tries", ref, DOOR_TRIES);
						return Enter(State::kPaused, Action::kRelease, std::format("paused: {}", pauseWhy));
					}
					++tries;
					usedAt = a_now;
					issued.reset();
					return { Action::kUseDoor, std::format("activate door {:08X} (try {} of {})", door->ref, tries, DOOR_TRIES) };
				}
				return {};
			}

			// The same place: after the host.
			door.reset();
			arrivedFrom.reset();
			askedAt.reset();
			if (!a_facts.hostKnown || !a_facts.hostHere) {
				pauseWhy = a_facts.hostKnown ? "the host's stand-in is not here" : "no host in the session";
				return Enter(State::kPaused, Action::kRelease, std::format("paused: {}", pauseWhy));
			}
			const bool entering = state != State::kFollow;
			bool       renew = entering || !issued || a_facts.hostActor != issuedFor;
			if (!renew) {
				const auto age = a_now - *issued;
				renew = mode == Mode::kTravel ? (age >= TRAVEL_RENEW || Distance(a_facts.hostAt, issuedAt) >= TRAVEL_RENEW_DISTANCE) : age >= FOLLOW_RENEW;
			}
			state = State::kFollow;
			if (!renew) {
				return {};
			}
			issued = a_now;
			issuedFor = a_facts.hostActor;
			issuedAt = a_facts.hostAt;
			return { Action::kFollowHost,
				entering ? std::format("follow {:08X} on a {} package, {:.0f} away", a_facts.hostActor, Name(mode), Distance(a_facts.hostAt, a_facts.us)) : std::string{} };
		}

	private:
		static std::string PauseReason(const Facts& a_facts)
		{
			if (!a_facts.inGame) {
				return "not in a game";
			}
			if (a_facts.loading) {
				return "a loading screen";
			}
			if (a_facts.down) {
				return "our player is down";
			}
			if (a_facts.dialogue) {
				return "a conversation";
			}
			if (a_facts.menu) {
				return "a menu is open";
			}
			return {};
		}

		// Into a state, once: the same state again (a pause for the same reason) does and logs nothing.
		Decision Enter(State a_state, Action a_action, std::string a_log)
		{
			if (state == a_state && (a_state != State::kPaused || pauseLogged == pauseWhy)) {
				return {};
			}
			state = a_state;
			issued.reset();
			pauseLogged = a_state == State::kPaused ? pauseWhy : std::string{};
			return { a_action, std::move(a_log) };
		}

		void Reset()
		{
			state = State::kOff;  // the next Step enters its state afresh (and logs it)
			door.reset();
			arrivedFrom.reset();
			issued.reset();
			askedAt.reset();
			pauseWhy.clear();
			pauseLogged.clear();
		}

		State                            wanted = State::kOff;
		State                            state = State::kOff;
		Mode                             mode = Mode::kFollow;
		std::uint32_t                    player = 0;
		std::string                      pauseWhy;
		std::string                      pauseLogged;  // the pause last entered (and logged)
		std::optional<Clock::time_point> issued;      // when the current package was made
		std::uint32_t                    issuedFor = 0;
		Point                            issuedAt;
		Point                            lastSeen;    // where the host's stand-in was last seen in our place
		Place                            seenHere;    // the place it was seen in
		Clock::time_point                lastTargetAt{};
		std::uint32_t                    fighting = 0;
		std::optional<Door>              door;
		std::optional<Place>             arrivedFrom;  // the place the door was picked in
		std::optional<Clock::time_point> usedAt;
		int                              tries = 0;
		std::optional<Clock::time_point> askedAt;  // when the glue was last asked to pick the door (Action::kPickDoor)

	public:
		// Where the host's stand-in was last seen in our place: the spot PickDoor measures from.
		[[nodiscard]] const Point& LastSeen() const { return lastSeen; }
	};
}
