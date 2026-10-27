#pragma once

#include "Protocol.h"
#include "Transport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The session server: relays player state and keeps the shared world state (kills, loot, doors,
// quests, NPC ownership). Runs on its own thread; used by the standalone F4MPServer.exe and by
// the game plugin when a player hosts.
class Server
{
public:
	struct Options
	{
		std::uint16_t port = 7779;
		std::size_t   maxPlayers = 4;
		std::string   password;  // empty = no password

		// Accept UDP connections only from this machine (the host's own game), e.g. when friends
		// connect through Steam instead.
		bool udpLoopbackOnly = false;

		// The Hello::identity of the game this server runs in (0: none, a dedicated server). That game,
		// connecting from this machine, is the host: its clock is the session's, and an NPC it finds
		// already dead in its save is dead for everyone. Any other connection from this machine (a bot, a
		// tunnel to another PC) is an ordinary player.
		std::uint64_t hostIdentity = 0;

		// Players can hurt each other (their hits on each other's stand-ins are passed on).
		bool friendlyFire = false;

		// Main story and faction quest progress is shared too, not just side quests (each
		// player's game decides what to apply; this only tells them the host's choice).
		bool sharedStory = false;

		// The host's rules for everyone (revive, real-time VATS and menus), sent with each welcome.
		Protocol::SessionRules rules;

		// Container changes one player may author in a session: one client cannot fill the shared table
		// (60,000) on its own. Held per player ID and, for the server's run, per budget (a reconnect with a
		// made-up identity doesn't renew it). Tests lower it.
		std::size_t maxContainerChangesPerPlayer = 30000;

		// The size of each of the session's shared tables (deaths, container changes, pickups, door
		// states, quest stages); at most, and by default, Protocol::MAX_WORLD_STATE_ACTORS. Tests lower it.
		std::size_t maxWorldEntries = 60000;

		// What one player's game may add to each of those tables (container changes: the setting above)
		// and to the settlements' built objects in one server run (until the host quits). The host's own
		// game has no limit. Generous: a player picks up and kills a few hundred things an hour.
		std::size_t maxWorldEntriesPerPlayer = 15000;
		std::size_t maxWorkshopItemsPerPlayer = 10000;
		// Objects one player's game may give a kept place to (ObjectMovers::Places) in one server run. Well under its
		// 2048 (MAX_PLACES): a full Places forgets the place kept longest, so a joiner's made-up objects can't push out
		// the host's places. Held to at most a quarter of maxObjectPlaces (512) whatever is set (--max-share sets it with
		// the other shares).
		std::size_t maxObjectPlacesPerPlayer = 512;
		// How many objects' places the session keeps; at most, and by default, ObjectMovers::MAX_PLACES. Tests lower it.
		std::size_t maxObjectPlaces = 2048;

		// A player asking for the world state again (a reload, a rejoin) has a bucket of three, refilled
		// one per this. Past it, theirs is sent when the bucket refills. Tests shorten it.
		std::chrono::milliseconds worldStateRefill{ 10000 };

		// The others are told a player loaded a save (kPlayerReloaded) at most this often: each time, every game sends its
		// radiant runs going again. One asked for sooner goes when the time is up. Tests shorten it.
		std::chrono::milliseconds reloadToldEvery{ 2000 };

		// A player's face goes to the others at most this often (each change makes a new base form in
		// every game); the latest one follows when the time is up. Tests shorten it.
		std::chrono::milliseconds faceInterval{ 5000 };

		// A player who hurt an NPC, or tried to take it over, within this long is believed about killing it while
		// another player's game runs it (the takeover is refused for 5 s after one, and a kill in that time used to
		// be lost). Tests shorten it.
		std::chrono::milliseconds deathGrace{ 10000 };

		// A player who hits or talks to an NPC another player's game runs takes it over, unless it changed hands, or
		// its runner claimed it again (their game does with each hit of theirs on it, and every 2 s while they talk to
		// it), within this long: two players shooting one raider leave it with whoever hit it first, as long as they
		// keep hitting it, instead of trading it every few seconds and restarting its AI each time. Tests shorten it.
		std::chrono::milliseconds takeoverCooldown{ 5000 };

		// The host's game offering to run an NPC the session has dead brings it back for everyone (its cell reset:
		// the raiders of a camp cleared days ago). Believed only once this long has passed since the host's game
		// last asked for the session's world, and since the death: before that, its game may still have it alive
		// from a save older than the kill, or not have heard of the kill yet. Tests shorten it.
		std::chrono::milliseconds aliveAgainGrace{ 10000 };

		// A joiner's game that has an NPC alive which the session has dead (it offered to run it, hit it, or the NPC hit a
		// stand-in there: its cell reset in that game alone) is told the death again, so it kills its copy: first once
		// it has reported about the NPC for aliveAgainGrace (the host's game, walking in beside it, is given that long to
		// bring the NPC back for everyone instead), then at most this often for each NPC. Tests shorten it.
		std::chrono::milliseconds remindDeadEvery{ 5000 };

		// Plugins that may differ between players (texture, sound or UI mods): left out of what a joiner is
		// told differs. (Since protocol 36 any load order is let in and translated; a hello without a plugin
		// list still needs the session's, once these are left out.)
		std::vector<std::string> ignoredPlugins;

		// Where the session's world is saved (and loaded from at start), so it survives the host
		// quitting. Empty = not saved.
		std::string sessionFile;

		// How often the session file is checked for changes to write. Tests shorten it.
		std::chrono::milliseconds saveInterval{ 15000 };

		// Where the host's bans are kept (common/BanList.h), read at start and written at each change, so a ban outlives
		// the server. Empty = bans last until the server stops.
		std::string banFile;
	};

	using LogFn = std::function<void(std::string_view)>;

	explicit Server(LogFn a_log);
	~Server();

	Server(const Server&) = delete;
	Server& operator=(const Server&) = delete;

	// Binds the UDP port and starts the server thread. Players can also arrive through any extra
	// transports (e.g. Steam). Returns false if the port can't be bound.
	bool Start(const Options& a_options, std::vector<std::unique_ptr<ServerTransport>> a_extraTransports = {});
	// Stops the server thread; the session file is written first.
	void Stop();

	// Writes the session file on the server thread's next tick, changed or not (the host's game is saving:
	// the file is then never older than the host's save). Any thread.
	void RequestSave() { saveRequested = true; }

	// A line typed at the dedicated server's console (help, players, kick <n>, ban <n>, bans, unban <n|alias|all>,
	// password [<text> | -], lock, unlock): carried out on the server thread's next tick, the answer in the log. The
	// console is the server's owner, as the host's own game is in a hosted one. Any thread.
	void Console(std::string a_line)
	{
		std::lock_guard lock{ consoleMutex };
		if (consoleLines.size() < 64) {
			consoleLines.push_back(std::move(a_line));
		}
	}

	// The server's clock, which a test can move on (AdvanceClock) or stop (HoldClock). Nanoseconds.
	struct ClockControl
	{
		std::atomic<std::int64_t> skew{ 0 };  // added to the machine's steady clock
		std::atomic<std::int64_t> held{ 0 };  // the steady clock's reading the clock was stopped at; 0 while it runs
	};

	// For tests: moves the server's clock on by a_by, as if that long had passed (a cooldown, a refill, a grace period),
	// without waiting. Unless it is held, time still passes on its own as well. Any thread.
	void AdvanceClock(std::chrono::milliseconds a_by)
	{
		testClock.skew += std::chrono::duration_cast<std::chrono::nanoseconds>(a_by).count();
	}

	// For tests: stops the server's clock where it is, so it moves only by AdvanceClock. A test of what happens inside a
	// window (a rate limit's second) then can't be made late by a busy PC, and lets the window out by advancing, not by
	// sleeping. What the server does by time alone (dropping a connection that never said hello, rolling a second's
	// budgets) waits for the advance, so a test that holds the clock sends few heartbeats (ten a second are answered).
	void HoldClock()
	{
		const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		testClock.held = now != 0 ? now : 1;
	}

	[[nodiscard]] bool Running() const { return running; }

private:
	void Run();

	std::vector<std::string> TakeConsole()
	{
		std::lock_guard lock{ consoleMutex };
		return std::exchange(consoleLines, {});
	}

	LogFn                                         log;
	Options                                       options;
	std::vector<std::unique_ptr<ServerTransport>> transports;
	std::thread                                   thread;
	std::atomic<bool>                             running{ false };
	std::atomic<bool>                             stopRequested{ false };
	std::atomic<bool>                             saveRequested{ false };
	ClockControl                                  testClock;
	std::mutex                                    consoleMutex;
	std::vector<std::string>                      consoleLines;
};
