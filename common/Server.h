#pragma once

#include "Transport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
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

		// A player asking for the world state again (a reload, a rejoin) has a bucket of three, refilled
		// one per this. Past it, theirs is sent when the bucket refills. Tests shorten it.
		std::chrono::milliseconds worldStateRefill{ 10000 };

		// A player's face goes to the others at most this often (each change makes a new base form in
		// every game); the latest one follows when the time is up. Tests shorten it.
		std::chrono::milliseconds faceInterval{ 5000 };

		// A player who hurt an NPC, or tried to take it over, within this long is believed about killing it while
		// another player's game runs it (the takeover is refused for 5 s after one, and a kill in that time used to
		// be lost). Tests shorten it.
		std::chrono::milliseconds deathGrace{ 10000 };

		// The host's game offering to run an NPC the session has dead brings it back for everyone (its cell reset:
		// the raiders of a camp cleared days ago). Believed only once this long has passed since the host's game
		// last asked for the session's world, and since the death: before that, its game may still have it alive
		// from a save older than the kill, or not have heard of the kill yet. Tests shorten it.
		std::chrono::milliseconds aliveAgainGrace{ 10000 };

		// Plugins that may differ between players (texture, sound or UI mods): a load order that
		// matches once these are left out is accepted.
		std::vector<std::string> ignoredPlugins;

		// Where the session's world is saved (and loaded from at start), so it survives the host
		// quitting. Empty = not saved.
		std::string sessionFile;

		// How often the session file is checked for changes to write. Tests shorten it.
		std::chrono::milliseconds saveInterval{ 15000 };
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

	[[nodiscard]] bool Running() const { return running; }

private:
	void Run();

	LogFn                                         log;
	Options                                       options;
	std::vector<std::unique_ptr<ServerTransport>> transports;
	std::thread                                   thread;
	std::atomic<bool>                             running{ false };
	std::atomic<bool>                             stopRequested{ false };
	std::atomic<bool>                             saveRequested{ false };
};
