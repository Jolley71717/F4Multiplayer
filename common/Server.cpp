#include "Server.h"

#include "BanList.h"
#include "DropStore.h"
#include "EnetTransport.h"
#include "FormIds.h"
#include "Name.h"
#include "ObjectMovers.h"
#include "Protocol.h"
#include "Redact.h"
#include "SaveRetry.h"
#include "SessionFile.h"
#include "StorageBook.h"
#include "TokenBucket.h"
#include "WorkshopStore.h"

#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <random>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
	// The server's clock is the machine's steady clock plus however far a test has moved it (Server::AdvanceClock). A test
	// waiting out a cooldown moves the clock past it, so it needs no sleep that a busy PC could make too short for the time
	// it means; one that must act inside a window can hold the clock (Server::HoldClock), so a busy PC can't make it late.
	// Only the server thread reads it.
	thread_local const Server::ClockControl* t_clock = nullptr;

	struct Clock
	{
		using rep = std::chrono::steady_clock::rep;
		using period = std::chrono::steady_clock::period;
		using duration = std::chrono::steady_clock::duration;
		using time_point = std::chrono::steady_clock::time_point;
		static constexpr bool is_steady = true;

		static time_point now()
		{
			if (!t_clock) {
				return std::chrono::steady_clock::now();
			}
			const auto held = t_clock->held.load(std::memory_order_relaxed);
			const auto base = held != 0 ? time_point{ std::chrono::duration_cast<duration>(std::chrono::nanoseconds{ held }) } : std::chrono::steady_clock::now();
			return base + std::chrono::duration_cast<duration>(std::chrono::nanoseconds{ t_clock->skew.load(std::memory_order_relaxed) });
		}
	};

	constexpr auto TICK_INTERVAL = std::chrono::milliseconds(16);  // ~60 Hz relay of new player states
	constexpr int  MAX_STATES_PER_SECOND = 60;                     // player states (sent at 30 Hz)
	constexpr int  MAX_ACTOR_BATCHES_PER_SECOND = 120;             // NPC state batches (20 Hz, one per 64 NPCs)
	// "Take all" from a big container reports one event per item type.
	constexpr int  MAX_EVENTS_PER_SECOND = 500;
	// Shots only animate other players' stand-ins, so excess ones are simply dropped.
	constexpr int  MAX_SHOTS_PER_SECOND = 60;
	// A player's name changes at most this often (a game looks at its name once a second; see flushRename).
	constexpr auto RENAME_INTERVAL = std::chrono::milliseconds(500);
	// A game says what its world holds of the items given (kDropRecord) after its welcome and each load; one sooner than
	// this after the last waits (the latest kept) for the tick.
	constexpr auto DROP_RECORD_INTERVAL = std::chrono::milliseconds(250);
	// What lies in the session (kDropList, up to ~385 KB) answers the first record after a welcome, and a later one only
	// this long after the last list: a game that loads a save already knows what lies there (each change is told to every
	// game), so a game sending records over and over isn't sent the whole list each time.
	constexpr auto DROP_LIST_INTERVAL = std::chrono::seconds(10);
	// The session's load order goes to a player who asks (kRequestLoadOrder) at most this often: it is up to 150 KB.
	constexpr auto LOAD_ORDER_INTERVAL = std::chrono::seconds(5);
	constexpr int  MAX_VOICE_BYTES_PER_SECOND = 16000;  // Steam voice needs about 2 KB/s
	// A world state is big (up to a megabyte), so asking for it again (hello again, a loaded save)
	// has its own small budget: the outgoing queue would otherwise grow without limit.
	// World states a player may ask for: a bucket of three (a join, a reload and one more right after),
	// refilled one every Options::worldStateRefill (ten seconds). It used to be four a second, every second:
	// a client asking without pause could have the host send its whole session, up to a couple of
	// megabytes, four times a second. Past the bucket the world state is owed, not refused: it goes when
	// the bucket refills.
	constexpr double WORLD_STATE_BUCKET = 3.0;
	// What a client is believed about. These bound what one player (or one player's broken game) can do
	// to everyone else's world: see docs/AUDIT-LIFETIME.md, network trust.
	constexpr float MAX_HIT_DAMAGE = 500.0f;                 // a mini nuke is about 400
	constexpr float MAX_HIT_DAMAGE_PER_SECOND = 1500.0f;     // per reporter, across all victims
	// An explosion is passed on from near where its reporter (or another player) is, within their budget, and nothing
	// else: no window after a shot or a throw. The game that runs an NPC it reaches deals its blow there, and its reporter gave that blow back on
	// its own copy (docs/DESIGN.md, "Who sets off an explosion"): one dropped here was a blow dealt nowhere. Each game judges
	// its reports by stricter numbers before it sends them (RemoteEdits::ReportGate), so none it sends is dropped.
	constexpr float EXPLOSION_RANGE = Protocol::REPORTED_EXPLOSION_RANGE;  // from the reporter's last position, or another player's
	// A trap is reported by a game that has it loaded: it lies within the loaded cells around the reporter (a mine can be
	// shot from the far side of them), never across the map.
	constexpr float TRAP_RANGE = 16000.0f;
	// Taking or disarming a trap is done standing at it: the reporter is within this of it (their last state comes 20 times
	// a second), or a report is a way to take away every mine around them.
	constexpr float TRAP_REACH = 1024.0f;
	// A frame lent out is given back where its borrower got out of it: within this of their last state (they step out
	// beside it), or it is moved there instead, never across the map.
	constexpr float RETURN_REACH = 1024.0f;
	// A destructible object is reported by a game that has it loaded, as a trap is: within the loaded cells around the
	// reporter (a car can be shot from the far side of them), never across the map.
	constexpr float DESTRUCTION_RANGE = 16000.0f;
	// A destructible stage's own explosion goes with it, a burst and then this many a second per player: far past what any
	// fight sets off (a street of cars going up), a bound on a broken or modified game setting every car near the others
	// off at once. Past it the stage is still relayed, with no explosion (docs/DESIGN.md, known limits).
	constexpr double STAGE_EXPLOSIONS_PER_SECOND = 64.0;
	// Explosions one player may report: a few grenades at once, then four a second. All joiners' together have room for
	// every seat's at once (joinersExplosions): what the others spend can't be told from the reporter's game (ReportGate).
	constexpr double EXPLOSION_BURST = Protocol::REPORTED_EXPLOSION_BURST;
	constexpr double EXPLOSIONS_PER_SECOND = Protocol::REPORTED_EXPLOSIONS_PER_SECOND;
	// Deaths one player may report: a settlement defence or a ghoul pack at once, then two a second. A death
	// is shared and saved with the session; a client making them up could otherwise empty the map.
	constexpr double DEATH_BURST = 60.0;
	constexpr double DEATHS_PER_SECOND = 2.0;
	// All joiners together, on top of each one's own: a client that leaves and comes back as someone new
	// starts a new budget of its own, but not of this one.
	constexpr double JOINERS_SHARE = 2.0;
	constexpr std::size_t MAX_COMPANIONS_PER_PLAYER = 4;
	// Hello identities remembered per server run; a client can make up new ones, so there's a cap.
	constexpr std::size_t MAX_IDENTITIES = 1024;
	// Budgets remembered per server run (see Budget). Past this, the one unused longest goes; never one
	// a connection still uses.
	constexpr std::size_t MAX_BUDGET_KEYS = 1024;
	constexpr std::size_t MAX_WORKSHOP_ITEMS = 20000;  // built objects remembered for joiners (a big settlement is a few thousand)
	constexpr std::size_t MAX_WORLD_SCRAPS = 20000;    // pieces of the world scrapped, remembered apart from what's built
	// Shared XP: at most MAX_XP_SHARE per XP_REFILL_TIME, in at most XP_REPORT_BURST reports
	// (one more each XP_REPORT_INTERVAL).
	constexpr auto XP_REFILL_TIME = std::chrono::seconds(10);
	constexpr int  XP_REPORT_BURST = 10;
	constexpr auto XP_REPORT_INTERVAL = std::chrono::milliseconds(500);
	// Connections that haven't said Hello by then are dropped (they hold a slot).
	constexpr auto HELLO_TIMEOUT = std::chrono::seconds(5);
	// A player who hits or talks to an NPC another player runs takes it over, at most every Options::takeoverCooldown.
	constexpr std::size_t MAX_FIGHTING = 256;  // NPCs one player is remembered fighting (a big fight is a dozen)
	// A player's game that has an NPC alive which the session has dead (it hit it, offered to run it, or it hit their
	// stand-in) is told its death again (Options::remindDeadEvery): at most this many NPCs are remembered for one player.
	constexpr std::size_t MAX_REMINDED = 256;
	constexpr auto DROPPED_DEATH_LOG_INTERVAL = std::chrono::seconds(10);
	// A flooder's budget refills and runs out again twice a second: the log says so at most this often.
	constexpr auto BUDGET_LOG_INTERVAL = std::chrono::seconds(10);
	// NPCs games made at runtime and share (Protocol::IsSpawnRef): each game's share at once is
	// Protocol::MAX_SPAWNS_PER_PLAYER, the session's this. A game may share a fight's worth at once, then two a second
	// (each one is a copy placed in every other game).
	constexpr std::size_t MAX_SPAWNS = 128;
	constexpr double      SPAWN_BURST = 32.0;
	constexpr double      SPAWNS_PER_SECOND = 2.0;
	// Shared storage (StorageBook.h). A friend's moves at storage another game keeps have their own budget, ahead of
	// anything the book is asked or answers: a burst of a Take All and a Store All of the fullest container (each move
	// dropped would be refused, and undone in their game), then the event budget's rate. Its refusals have one of the same
	// size (past it a move gets no answer: their game keeps it on its way, and sends it again as a late move when it next
	// settles, after a load or a rejoin). The contents a view is
	// sent (up to ~8 KB) go a few a second at most; one past that is answered closed.
	constexpr double      STORAGE_MOVE_BURST = static_cast<double>(Protocol::MAX_STORAGE_MOVES_UNANSWERED);
	constexpr double      STORAGE_MOVES_PER_SECOND = 500.0;
	constexpr double      STORAGE_VIEW_BURST = 16.0;
	constexpr double      STORAGE_VIEWS_PER_SECOND = 8.0;

	// A connection: which transport, and that transport's peer ID.
	using ConnectionKey = std::uint64_t;

	ConnectionKey MakeKey(std::size_t a_transport, PeerId a_peer)
	{
		return (static_cast<std::uint64_t>(a_transport) << 32) | a_peer;
	}

	struct RateWindow
	{
		Clock::time_point start{};
		int               states = 0;
		int               actorStates = 0;
		int               events = 0;
		int               shots = 0;
		int               heartbeats = 0;
		int               voiceBytes = 0;
		int               objectHolds = 0;  // kReportObjectHold (Protocol::HOLDS_PER_SECOND)
		float             hitDamage = 0.0f;

		void Roll(Clock::time_point a_now)
		{
			if (a_now - start >= std::chrono::seconds(1)) {
				start = a_now;
				states = actorStates = events = shots = heartbeats = voiceBytes = objectHolds = 0;
				hitDamage = 0.0f;
			}
		}
	};

	// The session's shared tables a player's game adds to; each player has a share of each.
	enum Table : std::size_t
	{
		kDeaths,
		kPickups,
		kRefStates,
		kQuestStages,
		kContainers,
		kWorkshop,
		kTraps,
		kDestructions,
		kObjectPlaces,
		kTables
	};
	// For log lines: "the session's {} table", "{} sharing".
	constexpr std::array<std::string_view, kTables> TABLE_NAMES{ "dead NPC", "picked-up item", "door and lock", "quest stage", "container change", "built object", "world trap", "destroyed object",
		"moved object place" };
	constexpr std::array<std::string_view, kTables> SHARING_NAMES{ "kill", "pickup", "door", "quest", "loot", "building", "trap", "destruction", "moved object" };

	// Whose budget: the transport's own name for the peer (a Steam ID, which a client can't choose), else
	// the identity their game says hello with, else the connection itself.
	enum class BudgetKind : std::uint8_t
	{
		kTransport,
		kHello,
		kConnection
	};
	using BudgetKey = std::pair<BudgetKind, std::uint64_t>;

	// What a joiner may still report (the host's own game has no budget). A burst, then a few a second.
	struct RateBudget
	{
		TokenBucket explosions;
		TokenBucket deaths;
		TokenBucket stageExplosions;  // destructible stages' own, relayed with the stage (handleDestruction)

		RateBudget() :
			RateBudget(1.0)
		{}
		explicit RateBudget(double a_share) :
			explosions(a_share * EXPLOSION_BURST, a_share * EXPLOSIONS_PER_SECOND), deaths(a_share * DEATH_BURST, a_share * DEATHS_PER_SECOND),
			stageExplosions(a_share * STAGE_EXPLOSIONS_PER_SECOND, a_share * STAGE_EXPLOSIONS_PER_SECOND)
		{}
	};

	// What a player may still do, kept apart from the connection so that leaving and coming back
	// (or saying hello again under a made-up identity) doesn't renew it. Kept for the server's run.
	struct Budget
	{
		double            worldStateTokens = WORLD_STATE_BUCKET;
		Clock::time_point worldStateRefill{};
		float             xpBudget = Protocol::MAX_XP_SHARE;
		float             xpReports = XP_REPORT_BURST;
		Clock::time_point xpRefilled{};
		Clock::time_point lastFace{};
		Clock::time_point lastLine{};
		bool              floodLogged = false;
		RateBudget        rates;  // explosions and deaths
		Clock::time_point explosionsLogged{};  // when the log last said the explosion budget ran out
		std::size_t       explosionsDropped = 0;  // dropped since then
		Clock::time_point deathsLogged{};
		std::size_t       deathsDropped = 0;
		TokenBucket       spawns{ SPAWN_BURST, SPAWNS_PER_SECOND };  // runtime NPCs shared (the host's own game too)
		TokenBucket       objectRests{ ObjectMovers::REST_BURST, ObjectMovers::RESTS_PER_SECOND };  // objects let go of or come to rest
		TokenBucket       storageMoves{ STORAGE_MOVE_BURST, STORAGE_MOVES_PER_SECOND };      // moves at storage another game keeps
		TokenBucket       storageRefusals{ STORAGE_MOVE_BURST, STORAGE_MOVES_PER_SECOND };   // those answered refused
		TokenBucket       storageViews{ STORAGE_VIEW_BURST, STORAGE_VIEWS_PER_SECOND };     // contents sent to a view
		Clock::time_point spawnsLogged{};
		std::size_t       spawnsRefused = 0;
		std::array<std::size_t, kTables> added{};  // entries their games added to each table this run (built objects: those still kept)
		std::array<bool, kTables>        shareLogged{};
		int               connections = 0;  // connections using it (it isn't forgotten while any do)
		Clock::time_point lastSeen{};
	};

	struct Player
	{
		ConnectionKey              key = 0;
		std::uint32_t              id = 0;
		BudgetKey                  budgetKey{};
		Clock::time_point          connectedAt{};
		std::string                name;
		std::uint32_t              appearance = 0;
		std::vector<std::uint32_t> equipment;
		std::vector<Protocol::ItemMods> equipmentMods;  // the mods on those items, as reported with them
		bool                       equipmentReported = false;  // equipment is what they last reported (not just none yet)
		std::optional<Protocol::Face> face;
		bool                       faceDirty = false;  // the latest face hasn't gone to the others yet (Options::faceInterval)
		bool                       welcomed = false;
		bool                       kicked = false;
		bool                       hasState = false;
		bool                       stateDirty = false;
		// Asked for the world state once too often: it goes when their budget refills, from owedFrom on.
		// Until then their container changes aren't echoed to them (the world state carries them, in order).
		bool                       worldStateOwed = false;
		std::uint32_t              owedFrom = 0;
		bool                       workshopOwed = false;  // a load's settlements, sent with the owed world state
		// Shared storage: how many world states they asked for on this connection (the hello's first), and whether their
		// game has settled its storage moves since the last (Protocol::StorageSettle). Until it has, their storage views are
		// answered closed and their moves at storage another game keeps refused.
		std::uint32_t              worldAsked = 0;
		bool                       storageSettled = true;
		// A hello that came when their budget had no world state for it (a game rejoining over and over):
		// handled when it refills, so each hello's welcome (who runs which NPC, what's built) is paid for too.
		std::vector<std::uint8_t>  waitingHello;
		Protocol::PlayerState      state;
		RateWindow                 rate;
		std::optional<Protocol::PlayerStatus> status;
		Clock::time_point          lastPing{};
		Clock::time_point          lastRename{};  // a name changes twice a second at most (a game sends one on a change only, once a second)
		Clock::time_point          loadOrderSent{};  // when the session's load order last went to them (LOAD_ORDER_INTERVAL)
		std::optional<std::string> pendingName;   // cleaned; waits for RENAME_INTERVAL to pass (flushRename)
		bool                       local = false;  // the host's own game (Options::hostIdentity)
		std::vector<std::string>   plugins;        // their game's plugins, from their hello: whether it can hold an item put in storage it keeps
		// Their plugins against the session's table (sessionTableVersion), made once for storage they keep (handleContainer).
		std::optional<FormIds::Mapper> storageMapper;
		std::uint64_t              storageMapperFor = 0;
		// Who they are, for a ban (BanList): the transport's name for them (a Steam ID; 0 over UDP), and the identity
		// their game said hello with.
		std::uint64_t              transportId = 0;
		std::uint64_t              helloId = 0;
		// Host commands they sent without being the host: refused, and logged once per BUDGET_LOG_INTERVAL.
		Clock::time_point          hostRefusedLogged{};
		std::size_t                hostRefused = 0;
		// NPCs this player hurt, tried to take over, or ran until another took it over or they let it go, and when
		// last: a kill of one another player's game runs is believed for a while after (Options::deathGrace). At most
		// MAX_FIGHTING.
		std::unordered_map<std::uint32_t, Clock::time_point> fighting;
		// Dead NPCs this player's game has alive (remindDead): since when it has reported about one, when last, and when
		// its death was last told again.
		struct DeadReminder
		{
			Clock::time_point since{};  // its first report of this run (the game has had the NPC alive since)
			Clock::time_point last{};   // its latest report
			Clock::time_point told{};   // when the death was last told again (never: zero)
		};
		std::unordered_map<std::uint32_t, DeadReminder> deadReminded;
		Clock::time_point          worldAskedAt{};  // its hello, or its last request for the world (after loading a save)
		Clock::time_point          reloadToldAt{};  // when the others were last told it loaded a save (kPlayerReloaded)
		bool                       reloadOwed = false;  // it loaded again sooner: the others are told once Options::reloadToldEvery is up
		// Its last request for the world isn't answered yet (owed while its budget refills, or dropped for flooding):
		// its game may have anything alive from the save it loaded, so it brings nobody back (see handleClaims) until
		// a world state is sent to it.
		bool                       worldStale = false;
		Clock::time_point          dropRecordAt{};  // when their last kDropRecord was answered
		std::optional<Protocol::DropRecord> heldDropRecord;  // one that came sooner than DROP_RECORD_INTERVAL after
		std::optional<Clock::time_point>    dropListAt;      // when they were last sent what lies there (kDropList), if since their welcome
	};

	struct Ownership
	{
		std::uint32_t         playerId = 0;
		Protocol::ClaimReason reason = Protocol::ClaimReason::kUnowned;
		Clock::time_point     since{};
	};

	std::uint64_t NewSessionId()
	{
		std::random_device rd;
		std::uint64_t      id = 0;
		while (id == 0) {
			id = (static_cast<std::uint64_t>(rd()) << 32) | rd();
		}
		return id;
	}
}

// Every line goes through Redact::Scrub: the lines below name players by ID and mask addresses
// already, so this only catches what a transport's description or an error text brings in.
Server::Server(LogFn a_log) :
	log([a_log = std::move(a_log)](std::string_view a_line) { a_log(Redact::Scrub(a_line)); })
{}

Server::~Server()
{
	Stop();
}

bool Server::Start(const Options& a_options, std::vector<std::unique_ptr<ServerTransport>> a_extraTransports)
{
	if (running) {
		return true;
	}

	options = a_options;

	// Allow a couple of extra connections so full servers can still send a rejection.
	std::string error;
	auto        enet = EnetServerTransport::Create(options.port, options.maxPlayers + 2, options.udpLoopbackOnly, error);
	if (!enet) {
		log("server: " + error);
		return false;
	}
	transports.clear();
	transports.push_back(std::move(enet));
	for (auto& transport : a_extraTransports) {
		transports.push_back(std::move(transport));
	}

	stopRequested = false;
	running = true;
	thread = std::thread([this] { Run(); });
	// The story's choice too: a joiner whose story progress never reaches the host's game is first asked for this line.
	log(std::format("server: listening on UDP port {}{} (max {} players, {} transports, {} story)", options.port,
		options.udpLoopbackOnly ? " (this machine only)" : "", options.maxPlayers, transports.size(), options.sharedStory ? "shared" : "own"));
	const auto kept = (std::min)(options.maxObjectPlaces, ObjectMovers::MAX_PLACES);
	log(std::format("server: a joiner's share this run: {} world entries, {} container changes, {} built objects, {} moved object places of {}",
		options.maxWorldEntriesPerPlayer, options.maxContainerChangesPerPlayer, options.maxWorkshopItemsPerPlayer,
		ObjectMovers::PlacesShare(options.maxObjectPlacesPerPlayer, kept), kept));
	return true;
}

void Server::Stop()
{
	if (!running) {
		return;
	}
	stopRequested = true;
	if (thread.joinable()) {
		thread.join();
	}
	transports.clear();
	running = false;
	log("server: stopped");
}

void Server::Run()
{
	t_clock = &testClock;
	std::unordered_map<ConnectionKey, Player> players;
	std::uint32_t                             nextId = 1;
	std::unordered_map<std::uint64_t, std::uint32_t> identities;  // Hello::identity -> player ID, for this server run
	std::map<BudgetKey, Budget>               budgets;                // for this server run (see Budget)
	std::uint32_t                             tick = 0;
	std::uint32_t                             sessionContentHash = 0;  // set by the first player
	std::vector<std::string>                  sessionPlugins;          // their load order, to name the difference
	// The plugins that number every form ID the session holds and relays (Protocol::PluginTable, common/FormIds.h): the
	// saved session's, or the first player's load order, with every later joiner's other plugins added after it.
	std::vector<std::string>                  sessionTable;
	std::uint64_t                             sessionTableVersion = 0;  // moves on with every change to it
	std::uint64_t                             sessionId = NewSessionId();
	// The password as clients can send it (the wire format caps its length). The host can change it (HostCommand).
	auto password = options.password.substr(0, Protocol::MAX_PASSWORD_LENGTH);
	// Host controls: nobody new joins while locked (not saved: a restart opens it again) but those who joined during
	// this server run and dropped out, and the host's bans.
	bool                        locked = false;
	// Who a locked session takes back: the hello identities that joined during this server run, each with the Steam
	// account (Player::transportId, 0 on any direct connection) it joined over and when it was last here. Only in memory:
	// not saved, so a restart forgets them (the saved identities above only give a returning player their old number,
	// and every player who ever joined is in them). At the cap the one gone longest makes room, never a new member.
	struct Member
	{
		std::uint64_t     account = 0;
		Clock::time_point lastSeen{};
	};
	std::unordered_map<std::uint64_t, Member> members;
	// Hello identities and accounts of players the host kicked or banned this run: a locked session takes back those
	// that dropped out, not these (or another identity of the same account). Forgotten when they are let in again.
	std::unordered_set<std::uint64_t> removed;
	std::unordered_set<std::uint64_t> removedAccounts;
	BanList                     bans;
	const std::filesystem::path banPath{ options.banFile };
	if (!options.banFile.empty()) {
		const auto error = bans.Load(banPath);
		if (!error.empty()) {
			log("server: bans: " + error);
		}
		if (bans.Size() > 0) {
			log(std::format("server: {} banned player{} in {}", bans.Size(), bans.Size() == 1 ? "" : "s", banPath.filename().string()));
		}
	}
	// Writes the bans (when there's a file for them); false when it couldn't, which the log says.
	const auto saveBans = [&] {
		if (options.banFile.empty()) {
			return true;
		}
		if (!bans.Save(banPath)) {
			log(std::format("server: could not write the bans to {}; they hold until the server stops", banPath.filename().string()));
			return false;
		}
		return true;
	};
	// A world state can't carry more of a table than this.
	const auto maxWorldEntries = (std::min)(options.maxWorldEntries, Protocol::MAX_WORLD_STATE_ACTORS);

	// Shared world state for this session.
	std::unordered_set<std::uint32_t>                     deadActors;
	std::vector<Protocol::IndexedContainerChange>         containerChanges;  // index = session index
	std::unordered_set<std::uint32_t>                     pickedUp;
	std::unordered_map<std::uint32_t, Protocol::RefState> refStates;
	std::unordered_map<std::uint32_t, Protocol::TrapState> traps;  // world traps spent: gone off, disarmed or taken (TrapState)
	// Destructible objects damaged in the session (a car, a barrel): each one's lowest health, destroyed for good. Saved.
	std::unordered_map<std::uint32_t, Protocol::Destruction> destructions;
	std::unordered_map<std::uint32_t, Ownership>          actorOwners;  // which player's game runs each NPC's AI
	ObjectMovers::Book                                    objectMovers;  // who moves each loose object right now (not saved: a move is over in seconds)
	const auto                                            maxObjectPlaces = (std::min)(options.maxObjectPlaces, ObjectMovers::MAX_PLACES);
	ObjectMovers::Places                                  objectPlaces{ maxObjectPlaces };  // where each came to rest last, for a game that loads it later. Saved.
	// NPCs games made at runtime and share, by spawn id (Protocol::IsSpawnRef): each is run by its owner (in actorOwners
	// too) until it dies, its owner's game lets it go, or its owner leaves. Not saved: it is in no save but its owner's.
	std::unordered_map<std::uint32_t, Protocol::Spawn>    spawns;
	// Power armor frames lent between players (src/game/FrameLoan.h), by spawn id: a frame its owner's game shares as a
	// spawn, asked for by a friend (granted false) or lent to them, with the kit lent. One loan per frame and one per
	// borrower. Each ends with a return, the borrower leaving or loading a save, or the spawn ending. Not saved, as spawns are not.
	struct Loan
	{
		Protocol::FrameLending lending;  // ownerId, borrowerId, spawnId; the kit once granted
		bool                   granted = false;
	};
	std::unordered_map<std::uint32_t, Loan> loans;
	std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> npcEquipment;  // each run NPC's gear, from its owner
	// What each run NPC was rolled as (a leveled one's templates, a spawn's factions), from the first game to run it in its
	// life here: the session's roll, which the others make theirs. playerId is that game's player. A later runner whose
	// game still has its own roll (it could not make the session's) does not replace it, so nobody's NPC changes again
	// when a fight hands it over. Gone with the NPC (dead, or nobody runs it) and with that player.
	std::unordered_map<std::uint32_t, Protocol::NpcIdentity> npcIdentities;
	WorkshopStore                                         workshop{ MAX_WORKSHOP_ITEMS, MAX_WORLD_SCRAPS };  // built and scrapped at settlements, kept while the players are away too
	bool                                                  builtFullLogged = false;
	bool                                                  scrapsFullLogged = false;
	// Items players dropped for each other, which the server hands to one taker each (common/DropStore.h). Saved with the
	// session: a drop lies in nobody's save but the session file.
	DropStore                                             drops;
	bool                                                  dropsFullLogged = false;
	// Shared storage (Options::rules.sharedStorage): who keeps each home's or settlement's container, and what it holds
	// (common/StorageBook.h). The owners are saved with the session; what they hold is listed again by the host's game.
	StorageBook::Book                                     storage;
	std::size_t                                           storageRefused = 0;  // friends' moves at storage refused this run (for the log)
	std::size_t                                           storageUnanswered = 0;  // of those, past the refusals' budget: not answered (their game undoes them in time)
	std::size_t                                           storageUndone = 0;  // friends' moves the true copy undid or did again for their loaded worlds (for the log)
	bool                                                  aheadLogged = false;  // a player counting more container changes than we have
	std::vector<Protocol::QuestStage>                     questStages;  // in order, no repeats
	std::unordered_set<std::uint64_t>                     questStageKeys;
	std::unordered_map<std::uint32_t, std::uint32_t>      containersAuthored;  // player id -> changes they made this session
	// What all joiners together may report, on top of each one's own (Budget::rates).
	RateBudget                                            joinersBudget{ JOINERS_SHARE };
	// The same for explosions, with room for every seat's own budget at once: a game sends no more than its ReportGate allows,
	// well within its own, so what the others spend never drops one it sent; a client that leaves and comes back as someone
	// new gets no more than a full session's worth.
	TokenBucket joinersExplosions{ static_cast<double>((std::max)(options.maxPlayers, std::size_t{ 1 })) * EXPLOSION_BURST,
		static_cast<double>((std::max)(options.maxPlayers, std::size_t{ 1 })) * EXPLOSIONS_PER_SECOND };
	// Where an NPC fell, by the report that killed it; a joiner gets these before the dead list so its copy
	// lies where the killer saw it. Not saved: after a restart the list alone is what there is.
	std::unordered_map<std::uint32_t, std::array<float, 3>> deathPositions;
	// Who reported each death this run, and when: that player alone may say once where its corpse came to rest
	// (Protocol::CORPSE_REST_WINDOW). Not saved.
	struct RestFrom
	{
		std::uint32_t     playerId = 0;
		Clock::time_point at{};
	};
	std::unordered_map<std::uint32_t, RestFrom> restFrom;
	constexpr std::size_t MAX_DEATH_POSITIONS_SENT = 500;  // the same, for the repeat check (quest << 16 | stage)
	const auto questKey = [](const Protocol::QuestStage& a_stage) { return (static_cast<std::uint64_t>(a_stage.quest) << 16) | a_stage.stage; };
	std::optional<Protocol::WorldTime>                    worldTime;    // from the first player
	std::map<std::uint32_t, std::uint8_t>                 markers;      // discovered map markers (flags)
	// NPCs the host's game has had alive in this session (it claimed them or reported their health) or seen
	// die while another game ran them: its world can't be dead "from its save" about those (see handleDeath).
	std::unordered_set<std::uint32_t>                     hostHadAlive;
	// When each NPC died in this run (one from the saved session died long ago), and how many came back in the host's
	// world (see handleClaims). Not saved.
	std::unordered_map<std::uint32_t, Clock::time_point>  diedAt;
	std::size_t                                           aliveAgain = 0;
	// Deaths a build before session layout 29 saved (SessionFile::Snapshot::healable): one may be an essential NPC
	// that only went down, which the host's game can undo (see handleClaims). Saved, so a death seen in any run since
	// is never one, however many times the server restarts.
	std::unordered_set<std::uint32_t>                     healable;

	// A full table stops that kind of sharing for everyone (until the session file goes), so the log says
	// when one gets near, once at 80% and once full.
	std::array<bool, kTables> nearFullLogged{};
	std::array<bool, kTables> fullLogged{};
	const auto noteFill = [&](Table a_table, std::size_t a_size) {
		if (a_size >= maxWorldEntries) {
			nearFullLogged[a_table] = true;
			if (!std::exchange(fullLogged[a_table], true)) {
				log(std::format("server: the session's {} table is 100% full ({}/{}); {} sharing has stopped for this session", TABLE_NAMES[a_table], a_size, maxWorldEntries,
					SHARING_NAMES[a_table]));
			}
		} else if (a_size * 5 >= maxWorldEntries * 4 && !std::exchange(nearFullLogged[a_table], true)) {
			log(std::format("server: the session's {} table is {}% full ({}/{}); {} sharing stops at 100%", TABLE_NAMES[a_table], a_size * 100 / maxWorldEntries, a_size,
				maxWorldEntries, SHARING_NAMES[a_table]));
		}
	};

	// The saved session, if any: adopted as this run's session (the first player's load order must
	// match, or the hello path starts a fresh one). Saved again every few seconds while it changes.
	const std::filesystem::path sessionPath{ options.sessionFile };
	if (!options.sessionFile.empty()) {
		if (const auto saved = SessionFile::Recover(sessionPath, [&](const std::string& a_line) { log("server: " + a_line); })) {
			sessionContentHash = saved->contentHash;
			sessionTable = saved->plugins;
			++sessionTableVersion;
			sessionId = saved->sessionId;
			nextId = (std::max)(nextId, saved->nextId);
			for (const auto& [identity, id] : saved->identities) {
				identities[identity] = id;
			}
			deadActors.insert(saved->deadActors.begin(), saved->deadActors.end());
			for (const auto id : saved->healable) {
				if (deadActors.contains(id)) {
					healable.insert(id);
				}
			}
			containerChanges = saved->containerChanges;
			containersAuthored.clear();
			for (const auto& change : containerChanges) {
				++containersAuthored[change.playerId];
			}
			pickedUp.insert(saved->pickedUp.begin(), saved->pickedUp.end());
			for (const auto& state : saved->refStates) {
				refStates[state.refId] = state;
			}
			for (const auto& trap : saved->traps) {
				auto& kept = traps[trap.refId];
				if (Protocol::TrapSupersedes(trap.state, kept.refId != 0 ? std::optional{ kept.state } : std::nullopt)) {
					kept = trap;
				}
			}
			questStages = saved->questStages;
			questStageKeys.clear();
			for (const auto& stage : questStages) {
				questStageKeys.insert(questKey(stage));
			}
			worldTime = saved->worldTime;
			for (const auto& [ref, flags] : saved->markers) {
				markers[ref] = flags;
			}
			for (const auto& item : saved->workshopItems) {
				workshop.Restore(item);
			}
			for (const auto& drop : saved->drops) {
				drops.Restore(drop);
			}
			// Kept whatever the rule: a run with it off leaves the book alone (every game keeps its own copy, and nothing asks
			// the book) and saves it again, so turning it on later finds each container's keeper and the moves since its claim
			// as they were. Forgotten, the next listing would be a first claim, and its take-back (StorageBook::BeforeClaim)
			// would take out of the host's chest everything friends put in it while the host kept it.
			for (const auto& owner : saved->storageOwners) {
				storage.Restore(owner);  // closed to friends until the host's game lists it again
			}
			for (const auto& reversal : saved->storageReversals) {
				storage.Restore(reversal, containerChanges.size());  // none undone twice after a restart
			}
			for (const auto& destruction : saved->destructions) {
				destructions[destruction.refId] = destruction;
			}
			for (const auto& place : saved->objectPlaces) {
				objectPlaces.Rested(place);
			}
			log(std::format("server: loaded the saved session ({} dead, {} loot changes, {} built, {} scrapped, {} items dropped for others)", deadActors.size(),
				containerChanges.size(), workshop.Built().size(), workshop.Scraps().size(), drops.LyingCount()));
			noteFill(kDeaths, deadActors.size());
			noteFill(kPickups, pickedUp.size());
			noteFill(kRefStates, refStates.size());
			noteFill(kTraps, traps.size());
			noteFill(kQuestStages, questStages.size());
			noteFill(kContainers, containerChanges.size());
			noteFill(kDestructions, destructions.size());
		}
	}
	// Counts every change to what is saved, so one that leaves the counts as they were (a door closed again, a
	// settlement object moved) is saved too. The time of day isn't counted: it changes every second, and is
	// written with any other change, on the host's game saves and when the server stops.
	std::uint64_t revision = 0;
	std::uint64_t lastSavedRevision = 0;
	// A save that failed is tried again on its own, with a growing wait, whether or not anything else changed since:
	// the time of day alone is never a change, so a save the host's game asked for would otherwise be lost.
	SaveRetry  saveRetry{ options.saveInterval };
	const auto touched = [&] { ++revision; };
	const auto saveSession = [&](bool a_stopping = false) {
		if (options.sessionFile.empty() || sessionContentHash == 0) {
			return true;
		}
		SessionFile::Snapshot snapshot;
		snapshot.contentHash = sessionContentHash;
		snapshot.sessionId = sessionId;
		snapshot.nextId = nextId;
		for (const auto& [identity, id] : identities) {
			snapshot.identities.emplace_back(identity, id);
		}
		snapshot.deadActors.assign(deadActors.begin(), deadActors.end());
		snapshot.healable.assign(healable.begin(), healable.end());
		snapshot.containerChanges = containerChanges;
		snapshot.pickedUp.assign(pickedUp.begin(), pickedUp.end());
		for (const auto& [id, state] : refStates) {
			snapshot.refStates.push_back(state);
		}
		for (const auto& [id, trap] : traps) {
			snapshot.traps.push_back(trap);
		}
		snapshot.questStages = questStages;
		snapshot.worldTime = worldTime;
		for (const auto& [ref, flags] : markers) {
			snapshot.markers.emplace_back(ref, flags);
		}
		workshop.ForEach([&](const WorkshopStore::Key&, const Protocol::WorkshopItem& a_item) { snapshot.workshopItems.push_back(a_item); });
		snapshot.drops = drops.All();
		snapshot.storageOwners = storage.Owners();
		snapshot.storageReversals = storage.Reversals();
		for (const auto& [id, destruction] : destructions) {
			snapshot.destructions.push_back(destruction);
		}
		snapshot.objectPlaces = objectPlaces.All();
		snapshot.plugins = sessionTable;
		const auto saved = revision;
		if (!SessionFile::Save(sessionPath, snapshot)) {
			if (saveRetry.Failed(Clock::now()) && !a_stopping) {  // (stopping says its own line)
				log(std::format("server: could not save the session to {}; trying again until it works", sessionPath.filename().string()));
			}
			return false;  // tried again once the wait is over
		}
		if (saveRetry.Succeeded()) {
			log("server: the session is saved again");
		}
		lastSavedRevision = saved;
		return true;
	};
	Clock::time_point nextSave = Clock::now() + options.saveInterval;

	const auto resetWorld = [&] {
		deadActors.clear();
		healable.clear();
		containerChanges.clear();
		containersAuthored.clear();
		deathPositions.clear();
		restFrom.clear();
		diedAt.clear();
		pickedUp.clear();
		refStates.clear();
		traps.clear();
		actorOwners.clear();
		destructions.clear();
		objectPlaces.Clear();
		spawns.clear();
		loans.clear();
		npcEquipment.clear();
		npcIdentities.clear();
		workshop.Clear();
		drops.Clear();
		storage.Clear();
		questStages.clear();
		questStageKeys.clear();
		worldTime.reset();
		markers.clear();
		hostHadAlive.clear();
		nearFullLogged = {};
		fullLogged = {};
		// What each player added to the old session's tables is gone with it.
		for (auto& [key, budget] : budgets) {
			budget.added = {};
			budget.shareLogged = {};
		}
		sessionId = NewSessionId();
		sessionTable.clear();  // the next arrival's load order numbers the new session
		++sessionTableVersion;
		touched();
	};

	const auto send = [&](ConnectionKey a_key, const std::vector<std::uint8_t>& a_packet, bool a_reliable) {
		const auto transport = static_cast<std::size_t>(a_key >> 32);
		if (transport < transports.size()) {
			transports[transport]->Send(static_cast<PeerId>(a_key), a_packet, a_reliable);
		}
	};

	const auto disconnect = [&](Player& a_player) {
		a_player.kicked = true;
		const auto transport = static_cast<std::size_t>(a_player.key >> 32);
		transports[transport]->Disconnect(static_cast<PeerId>(a_player.key));
	};

	const auto describe = [&](ConnectionKey a_key) {
		return transports[static_cast<std::size_t>(a_key >> 32)]->Describe(static_cast<PeerId>(a_key));
	};

	// A player's connection stops using their budget (it is kept for when they come back).
	const auto unbindBudget = [&](Player& a_player) {
		if (const auto it = budgets.find(a_player.budgetKey); a_player.budgetKey != BudgetKey{} && it != budgets.end()) {
			--it->second.connections;
			it->second.lastSeen = Clock::now();
		}
		a_player.budgetKey = {};
	};

	// The player's connection uses the budget kept under a_key from now on: a fresh one the first time.
	const auto bindBudget = [&](Player& a_player, BudgetKey a_key) {
		if (a_player.budgetKey == a_key) {
			return;
		}
		unbindBudget(a_player);
		auto it = budgets.find(a_key);
		if (it == budgets.end()) {
			if (budgets.size() >= MAX_BUDGET_KEYS) {
				// The one unused longest goes. There is always one: far fewer connections are let in.
				auto oldest = budgets.end();
				for (auto entry = budgets.begin(); entry != budgets.end(); ++entry) {
					if (entry->second.connections == 0 && (oldest == budgets.end() || entry->second.lastSeen < oldest->second.lastSeen)) {
						oldest = entry;
					}
				}
				if (oldest != budgets.end()) {
					budgets.erase(oldest);
				}
			}
			it = budgets.emplace(a_key, Budget{}).first;
		}
		++it->second.connections;
		it->second.lastSeen = Clock::now();
		a_player.budgetKey = a_key;
	};

	// Looked up on every use. A connection's budget is never forgotten while it is connected, so this
	// only makes one if something went wrong.
	const auto budgetOf = [&](Player& a_player) -> Budget& {
		const auto [it, added] = budgets.try_emplace(a_player.budgetKey);
		if (added) {
			it->second.connections = 1;
		}
		return it->second;
	};

	// Sends to every welcomed player except a_except (0 = nobody excepted).
	const auto broadcast = [&](const std::vector<std::uint8_t>& a_packet, bool a_reliable, ConnectionKey a_except) {
		for (const auto& [key, other] : players) {
			if (key != a_except && other.welcomed) {
				send(key, a_packet, a_reliable);
			}
		}
	};

	const auto welcomedCount = [&] {
		std::size_t count = 0;
		for (const auto& [key, player] : players) {
			count += player.welcomed;
		}
		return count;
	};

	const auto reject = [&](Player& a_player, std::string_view a_reason) {
		send(a_player.key, Protocol::Encode(Protocol::Reject{ std::string{ a_reason } }), true);
		disconnect(a_player);
	};

	// The host controls' state for the host's game (kHostInfo): the newest bans first.
	const auto hostInfo = [&](Protocol::HostResult a_result, Protocol::HostAction a_action) {
		Protocol::HostInfo info;
		info.locked = locked;
		info.password = !password.empty();
		info.result = a_result;
		info.action = a_action;
		info.bans = static_cast<std::uint32_t>(bans.Size());
		for (auto it = bans.Entries().rbegin(); it != bans.Entries().rend() && info.list.size() < Protocol::MAX_BANS_SENT; ++it) {
			info.list.push_back({ it->Id(), it->Alias(), it->name, it->bannedAt });
		}
		return info;
	};
	// To the host's game, if it is in the session: after any change (from its menu or the console).
	const auto tellHost = [&](Protocol::HostResult a_result, Protocol::HostAction a_action) {
		const auto packet = Protocol::Encode(hostInfo(a_result, a_action));
		for (const auto& [key, player] : players) {
			if (player.local && player.welcomed && !player.kicked) {
				send(key, packet, true);
			}
		}
	};

	// The session's load order, for the player's game to compare with its own (Protocol::LoadOrder). None before
	// anyone set it.
	const auto sendLoadOrder = [&](Player& a_player, Clock::time_point a_now) {
		if (sessionPlugins.empty()) {
			return;
		}
		a_player.loadOrderSent = a_now;
		send(a_player.key, Protocol::Encode(Protocol::LoadOrder{ sessionPlugins, options.ignoredPlugins }), true);
	};

	// Counts an event against the player's budget. A flooding player loses the excess (logged once).
	const auto allowEvent = [&](Player& a_player) {
		if (!a_player.welcomed) {
			return false;
		}
		a_player.rate.Roll(Clock::now());
		if (++a_player.rate.events <= MAX_EVENTS_PER_SECOND) {
			return true;
		}
		if (!std::exchange(budgetOf(a_player).floodLogged, true)) {
			log(std::format("server: player {} sends too many events; dropping some", a_player.id));
		}
		return false;
	};

	// Spends one from a joiner's own rate budget (kept across rejoins: see Budget) and all joiners': false
	// when either is empty.
	const auto spend = [&](Player& a_player, TokenBucket RateBudget::*a_kind, Clock::time_point a_now) {
		return (budgetOf(a_player).rates.*a_kind).Take(a_now) && (joinersBudget.*a_kind).Take(a_now);
	};

	// Whether the player's budget has a world state for them now.
	const auto worldStateReady = [&](Player& a_player) {
		auto&      budget = budgetOf(a_player);
		const auto now = Clock::now();
		if (budget.worldStateRefill == Clock::time_point{}) {
			budget.worldStateRefill = now;
		}
		const auto elapsed = std::chrono::duration<double>(now - budget.worldStateRefill).count();
		const auto refill = std::chrono::duration<double>(options.worldStateRefill).count();
		budget.worldStateTokens = (std::min)(WORLD_STATE_BUCKET, budget.worldStateTokens + (refill > 0.0 ? elapsed / refill : WORLD_STATE_BUCKET));
		budget.worldStateRefill = now;
		return budget.worldStateTokens >= 1.0;
	};

	// Counts a world state the player gets (a hello, or kRequestWorldState after a load) against their budget.
	const auto allowWorldState = [&](Player& a_player) {
		if (!worldStateReady(a_player)) {
			return false;
		}
		budgetOf(a_player).worldStateTokens -= 1.0;
		return true;
	};

	// Whether a_player's game may add one more entry to a_table this run. Everyone but the host's own
	// game has a share of each table, kept in their budget, so one player (or a reconnect under a new
	// identity) can't fill a table for everyone. Logged once per player and table.
	const auto withinShare = [&](Player& a_player, Table a_table) {
		auto& budget = budgetOf(a_player);
		if (a_player.local) {
			return true;
		}
		const auto share = a_table == kContainers   ? options.maxContainerChangesPerPlayer :
		                   a_table == kWorkshop     ? options.maxWorkshopItemsPerPlayer :
		                   a_table == kObjectPlaces ? ObjectMovers::PlacesShare(options.maxObjectPlacesPerPlayer, maxObjectPlaces) :
		                                              options.maxWorldEntriesPerPlayer;
		if (budget.added[a_table] < share) {
			return true;
		}
		if (!std::exchange(budget.shareLogged[a_table], true)) {
			log(std::format("server: player {} has added {} {} entries this run, their share; no more of theirs are kept", a_player.id, budget.added[a_table], TABLE_NAMES[a_table]));
		}
		return false;
	};

	const auto sendMarkers = [&](ConnectionKey a_to) {
		Protocol::MarkersFound batch;
		for (const auto& [ref, flags] : markers) {
			batch.markers.push_back({ ref, flags });
			if (batch.markers.size() == Protocol::MAX_MARKERS_PER_PACKET) {
				send(a_to, Protocol::Encode(batch, Protocol::MessageType::kMarkersFound), true);
				batch.markers.clear();
			}
		}
		if (!batch.markers.empty()) {
			send(a_to, Protocol::Encode(batch, Protocol::MessageType::kMarkersFound), true);
		}
	};

	// Sends the session's world state in chunks. Container changes before a_containerFrom are
	// already in the receiver's world.
	const auto sendWorldState = [&](ConnectionKey a_to, std::uint32_t a_containerFrom) {
		sendMarkers(a_to);
		// The spent traps before the world state that ends the catching up, so a game holds the whole list by then.
		Protocol::TrapStates spent;
		for (const auto& [id, trap] : traps) {
			spent.traps.push_back(trap);
			if (spent.traps.size() == Protocol::MAX_TRAPS_PER_PACKET) {
				send(a_to, Protocol::Encode(spent), true);
				spent.traps.clear();
			}
		}
		if (!spent.traps.empty()) {
			send(a_to, Protocol::Encode(spent), true);
		}
		// Deaths with a known place go first, one by one with the place; the list that follows then finds
		// them already dead, so each copy lies where the killer saw it fall.
		std::size_t placed = 0;
		for (const auto& [refId, at] : deathPositions) {
			if (placed++ >= MAX_DEATH_POSITIONS_SENT) {
				break;
			}
			send(a_to, Protocol::Encode(Protocol::ActorDeath{ refId, 0, false, at[0], at[1], at[2] }, Protocol::MessageType::kActorDied), true);
		}
		const std::vector<std::uint32_t> dead(deadActors.begin(), deadActors.end());
		const std::vector<std::uint32_t> picked(pickedUp.begin(), pickedUp.end());
		std::vector<Protocol::RefState>  states;
		for (const auto& [id, state] : refStates) {
			states.push_back(state);
		}
		// A player whose save has more of the session's container changes than we do: the last ones were never
		// written (the host's game quit between saves). Our next ones take those numbers; the player's game
		// sees in the world state where our table ends.
		if (a_containerFrom > containerChanges.size() && !std::exchange(aheadLogged, true)) {
			log(std::format("server: {} has container changes up to #{}, the session only {}; the last ones weren't saved", describe(a_to), a_containerFrom,
				containerChanges.size()));
		}
		const std::size_t containerFrom = (std::min<std::size_t>)(a_containerFrom, containerChanges.size());

		const auto slice = [](const auto& a_list, std::size_t a_offset, std::size_t a_from = 0) {
			using T = typename std::decay_t<decltype(a_list)>::value_type;
			const auto begin = (std::min)(a_list.size(), a_from + a_offset);
			const auto end = (std::min)(a_list.size(), begin + Protocol::WORLD_STATE_CHUNK);
			return std::vector<T>(a_list.begin() + begin, a_list.begin() + end);
		};

		const auto longest = (std::max)({ dead.size(), containerChanges.size() - containerFrom, picked.size(), states.size(), questStages.size() });
		std::size_t offset = 0;
		do {
			Protocol::WorldState world;
			world.deadActors = slice(dead, offset);
			world.containerFirstIndex = static_cast<std::uint32_t>((std::min)(containerChanges.size(), containerFrom + offset));
			world.containerChanges = slice(containerChanges, offset, containerFrom);
			world.pickedUp = slice(picked, offset);
			world.refStates = slice(states, offset);
			world.questStages = slice(questStages, offset);
			send(a_to, Protocol::Encode(world), true);
			offset += Protocol::WORLD_STATE_CHUNK;
		} while (offset < longest);
		// Wrecks as they are, with nobody named: the receiver sets none of them off.
		Protocol::Destructions wrecks;
		for (const auto& [id, destruction] : destructions) {
			wrecks.entries.push_back(destruction);
			if (wrecks.entries.size() == Protocol::MAX_DESTRUCTION_ENTRIES) {
				send(a_to, Protocol::Encode(wrecks, Protocol::MessageType::kDestructions), true);
				wrecks.entries.clear();
			}
		}
		if (!wrecks.entries.empty()) {
			send(a_to, Protocol::Encode(wrecks, Protocol::MessageType::kDestructions), true);
		}
		// Where moved objects came to rest, with nobody named: the receiver puts each there once its game has it loaded.
		for (const auto& place : objectPlaces.All()) {
			send(a_to, Protocol::Encode(place, Protocol::MessageType::kObjectRested), true);
		}
	};

	// A player gets the world state they need (on joining, after loading a save) now if their budget
	// allows, else when it refills (the tick sends it): owed, never dropped, or their world would miss
	// what happened. Asking again while it is owed only moves where it starts.
	// After each world state, with shared storage on: which of their requests it answered, so their game settles its
	// storage moves against the last (Protocol::StorageCaughtUp).
	const auto sendCaughtUp = [&](const Player& a_player) {
		if (options.rules.sharedStorage) {
			send(a_player.key, Protocol::Encode(Protocol::StorageCaughtUp{ a_player.worldAsked }), true);
		}
	};

	const auto giveWorldState = [&](Player& a_player, std::uint32_t a_containerFrom) {
		if (a_player.worldStateOwed) {
			a_player.owedFrom = (std::min)(a_player.owedFrom, a_containerFrom);
			return;
		}
		if (allowWorldState(a_player)) {
			sendWorldState(a_player.key, a_containerFrom);
			sendCaughtUp(a_player);
			a_player.worldStale = false;
			return;
		}
		a_player.worldStateOwed = true;
		a_player.owedFrom = a_containerFrom;
		log(std::format("server: player {} has asked for the world too often; sending it when their budget refills", a_player.id));
	};

	// Everything built and scrapped at settlements so far, by anyone (their builder may be gone), in
	// batches; not what the player built themselves. Sent on joining, and again when they load a save:
	// the copies their game had made went with the world it had before the load.
	const auto sendWorkshop = [&](const Player& a_player) {
		std::vector<Protocol::WorkshopItem> batch;
		workshop.ForEach([&](const WorkshopStore::Key& a_key, const Protocol::WorkshopItem& a_item) {
			if (a_key.first == a_player.id) {
				return;
			}
			batch.push_back(a_item);
			if (batch.size() == Protocol::MAX_ACTORS_PER_PACKET) {
				send(a_player.key, Protocol::Encode(batch), true);
				batch.clear();
			}
		});
		if (!batch.empty()) {
			send(a_player.key, Protocol::Encode(batch), true);
		}
	};

	// Sends ownership changes in packets of at most MAX_ACTORS_PER_PACKET. a_to = 0: everyone.
	const auto sendOwners = [&](const std::vector<Protocol::ActorOwner>& a_owners, ConnectionKey a_to) {
		for (std::size_t i = 0; i < a_owners.size(); i += Protocol::MAX_ACTORS_PER_PACKET) {
			const auto end = (std::min)(a_owners.size(), i + Protocol::MAX_ACTORS_PER_PACKET);
			const auto packet = Protocol::Encode(std::vector<Protocol::ActorOwner>(a_owners.begin() + i, a_owners.begin() + end));
			if (a_to) {
				send(a_to, packet, true);
			} else {
				broadcast(packet, true, 0);
			}
		}
	};

	// (An NPC nobody runs has no gear kept: its next runner reports its own.)
	const auto releaseAll = [&](std::uint32_t a_playerId) {
		std::vector<Protocol::ActorOwner> released;
		std::erase_if(actorOwners, [&](const auto& a_entry) {
			if (a_entry.second.playerId != a_playerId) {
				return false;
			}
			released.push_back({ a_entry.first, 0 });
			npcEquipment.erase(a_entry.first);
			npcIdentities.erase(a_entry.first);
			spawns.erase(a_entry.first);  // nobody else's game has it to run: the others' copies go
			return true;
		});
		// The rolls they gave NPCs someone else runs now stay the session's, under that runner: their game was made that
		// roll (or tried), its report was refused for it, and it never reports the same again (NpcRoll::Sent). A newcomer
		// is told it as theirs; the runner's next change is the session's.
		for (auto& [ref, identity] : npcIdentities) {
			if (identity.playerId != a_playerId) {
				continue;
			}
			if (const auto runner = actorOwners.find(ref); runner != actorOwners.end()) {
				identity.playerId = runner->second.playerId;
			}
		}
		std::erase_if(npcIdentities, [&](const auto& a_entry) { return a_entry.second.playerId == a_playerId; });
		sendOwners(released, 0);
	};

	// Shared storage (common/StorageBook.h): friends whose view of a container closed (its keeper's world reloaded, or they
	// left) are told so, and their game closes its menu.
	const auto tellViewsClosed = [&](const std::vector<StorageBook::Closed>& a_closed) {
		for (const auto& closed : a_closed) {
			for (const auto& [key, other] : players) {
				if (other.welcomed && other.id == closed.viewer) {
					send(key, Protocol::Encode(Protocol::StorageRefusal{ closed.container, 0, 0, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed }), true);
				}
			}
		}
	};

	// A player's own views of storage others keep close (their world loaded: it settles before they view again).
	const auto closeOwnViews = [&](const Player& a_player) {
		for (const auto container : storage.CloseViews(a_player.id)) {
			send(a_player.key, Protocol::Encode(Protocol::StorageRefusal{ container, 0, 0, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed }), true);
		}
	};

	// Whose game keeps storage containers, in packets. a_to = 0: everyone.
	const auto sendStorageOwners = [&](const std::vector<Protocol::StorageOwner>& a_owners, ConnectionKey a_to) {
		for (std::size_t i = 0; i < a_owners.size(); i += Protocol::MAX_STORAGE_OWNERS_PER_PACKET) {
			const auto end = (std::min)(a_owners.size(), i + Protocol::MAX_STORAGE_OWNERS_PER_PACKET);
			const auto packet = Protocol::Encode(Protocol::StorageOwners{ { a_owners.begin() + i, a_owners.begin() + end } });
			if (a_to) {
				send(a_to, packet, true);
			} else {
				broadcast(packet, true, 0);
			}
		}
	};

	// The connection of the welcomed player a_id, or 0.
	const auto keyOfPlayer = [&](std::uint32_t a_id) -> ConnectionKey {
		for (const auto& [key, other] : players) {
			if (other.welcomed && !other.kicked && other.id == a_id) {
				return key;
			}
		}
		return 0;
	};

	// An ask the server dropped before it was granted: the owner's game hears it ended (kFrameReturned, ended, to it alone),
	// so it forgets the frame it was about to lend (or had answered for) and its own player may use it again.
	const auto tellAskOver = [&](const Protocol::FrameLending& a_loan) {
		if (const auto owner = keyOfPlayer(a_loan.ownerId)) {
			send(owner, Protocol::Encode(Protocol::FrameReturn{ a_loan.borrowerId, a_loan.ownerId, a_loan.spawnId, true }, Protocol::MessageType::kFrameReturned), true);
		}
	};

	// Loans a_ends picks end: a lent one with kFrameReturned (ended) to everyone, so the owner's game shows its frame where
	// it stood; one only asked for with a refusal to whoever asked, when they are still here, and its end to the owner.
	const auto endLoans = [&](auto a_ends) {
		std::erase_if(loans, [&](const auto& a_entry) {
			const auto& loan = a_entry.second;
			if (!a_ends(loan.lending)) {
				return false;
			}
			if (loan.granted) {
				log(std::format("server: loan of frame {:08X} from player {} to player {} ended", loan.lending.spawnId, loan.lending.ownerId, loan.lending.borrowerId));
				broadcast(Protocol::Encode(Protocol::FrameReturn{ loan.lending.borrowerId, loan.lending.ownerId, loan.lending.spawnId, true }, Protocol::MessageType::kFrameReturned), true, 0);
			} else {
				if (const auto asker = keyOfPlayer(loan.lending.borrowerId)) {
					send(asker, Protocol::Encode(Protocol::FrameLending{ loan.lending.ownerId, loan.lending.borrowerId, loan.lending.spawnId, Protocol::LoanRefusal::kUnavailable, {} }, Protocol::MessageType::kFrameLent), true);
				}
				tellAskOver(loan.lending);
			}
			return true;
		});
	};

	// Loans of frames whose spawn is over (it died, its owner's game let it go, or its owner left).
	const auto endLoansOfGoneSpawns = [&] {
		endLoans([&](const Protocol::FrameLending& a_loan) { return !spawns.contains(a_loan.spawnId); });
	};

	// The player is gone: the others are told, and the NPCs they ran are free for someone else.
	const auto leave = [&](const Player& a_player) {
		if (a_player.welcomed) {
			if (const auto member = members.find(a_player.helloId); member != members.end()) {
				member->second.lastSeen = Clock::now();
			}
			log(std::format("server: player {} left", a_player.id));
			broadcast(Protocol::Encode(Protocol::PlayerLeft{ a_player.id }), true, a_player.key);
		}
		releaseAll(a_player.id);
		// Their loans end: a frame lent to them, and one of theirs lent out (its spawn went with releaseAll).
		endLoans([&](const Protocol::FrameLending& a_loan) { return a_loan.borrowerId == a_player.id || a_loan.ownerId == a_player.id; });
		// Offers of loose drops they never confirmed go: their game gives those items back as it leaves. Their gifts' offers
		// wait for their next record (common/DropStore.h).
		drops.DropOffers(a_player.id);
		objectMovers.Leave(a_player.id);
		// They view no storage now, and what their game keeps closes to friends until it lists it again.
		tellViewsClosed(storage.Left(a_player.id));
	};

	// a_again: a hello on a connection already in the session (their game restarted; see welcome).
	const auto handleHello = [&](Player& a_player, std::span<const std::uint8_t> a_data, bool a_again) {
		const auto hello = Protocol::DecodeHello(a_data);
		if (!hello || hello->magic != Protocol::MAGIC) {
			reject(a_player, "Not an F4Multiplayer client");
			return;
		}
		if (hello->version != Protocol::VERSION) {
			reject(a_player, std::format("Version mismatch: server uses protocol {}, you have {}. Install the same mod version.", Protocol::VERSION, hello->version));
			return;
		}
		// The host's own game: the game this server runs in, by its identity, connecting from this machine.
		// The identity alone isn't enough (it isn't a secret), and nor is the address: a bot or a tunnel to
		// another PC connects from this machine too.
		const bool local = options.hostIdentity != 0 && hello->identity == options.hostIdentity &&
		                   transports[static_cast<std::size_t>(a_player.key >> 32)]->IsLocal(static_cast<PeerId>(a_player.key));
		// The host's bans, and the lock, before the password: a banned player isn't told whether theirs is right. The
		// host's own game is never refused for either.
		if (!local) {
			if (const auto ban = bans.Find(a_player.transportId, hello->identity)) {
				log(std::format("server: refused {}: banned ({})", describe(a_player.key), ban->Alias()));
				reject(a_player, Protocol::REJECT_BANNED);
				return;
			}
			// Locked: nobody new. Someone who joined during this server run is not new: their game restarted (a hello
			// again, or a new connection while the old one is still here), or it crashed or their connection dropped and
			// they come back. They must come by the Steam account they joined with (a game identity isn't a secret), and
			// not be someone the host kicked or banned, by identity or by account: that is being removed, not dropping out.
			// (Not the saved identities: those hold everyone who ever joined, before a restart too.)
			const bool inSession = a_again || [&] {
				const auto member = hello->identity != 0 ? members.find(hello->identity) : members.end();
				return member != members.end() && member->second.account == a_player.transportId && !removed.contains(hello->identity) &&
				       (a_player.transportId == 0 || !removedAccounts.contains(a_player.transportId));
			}();
			if (locked && !inSession) {
				log(std::format("server: refused {}: the session is locked", describe(a_player.key)));
				reject(a_player, Protocol::REJECT_LOCKED);
				return;
			}
		}
		if (!password.empty() && hello->password != password) {
			log(std::format("server: wrong password from {}", describe(a_player.key)));
			reject(a_player, "Wrong password");
			return;
		}
		if (welcomedCount() == 0) {
			// A server in a host's game waits for that game: the first arrival decides the session's load
			// order (and a different one wipes the saved session), so a friend whose Join lands while the
			// host is still at the main menu or loading would decide it for the host.
			if (options.hostIdentity != 0 && !local) {
				reject(a_player, Protocol::REJECT_HOST_NOT_READY);
				return;
			}
			// A saved session keeps the plugin table its form IDs are numbered by (session layouts 32 and 33): it carries on
			// whatever the first arrival's mods and their order, translated to it as anyone's are. One saved before
			// (or a hello without a list, which nothing translates) is the load order its hash names: a different
			// one makes the stored form IDs meaningless, so a fresh session starts. (On a dedicated server, which has
			// no local player, the first arrival decides.) A session saved by an older version, before the texture
			// pack counted, is the same load order's when its hash matches the old way.
			if (sessionContentHash != 0 && (sessionTable.empty() || hello->plugins.empty()) && hello->contentHash != sessionContentHash) {
				if (Protocol::LoadOrderHashBeforeTexturePack(hello->plugins) == sessionContentHash) {
					log("server: the saved session has this load order (hashed as older versions did, without the HD texture pack); it carries on");
				} else {
					log("server: load order changed; starting a new session");
					resetWorld();
				}
			}
			sessionContentHash = hello->contentHash;
			sessionPlugins = hello->plugins;
		} else if (hello->contentHash != sessionContentHash) {
			// The session's list first, whether they are let in or not: their game names every plugin that differs, the
			// host's ignored ones left out where they move no other plugin (the refusal names none). Before the refusal, on
			// the same channel, so it is there by then.
			sendLoadOrder(a_player, Clock::now());
			// Not their name, and they have no player number yet: where they come from (masked) says which
			// joiner it was. A difference only in plugins the host allows to differ (texture packs and the like) is
			// fine as before.
			if (Protocol::SameLoadOrderIgnoring(hello->plugins, sessionPlugins, options.ignoredPlugins)) {
				log(std::format("server: a joiner's load order differs only in ignored plugins ({})", describe(a_player.key)));
			} else if (hello->plugins.empty() || sessionTable.empty()) {
				// Without both lists nothing translates their form IDs (a tool's hello, or a session started by one).
				reject(a_player, Protocol::LoadOrderDifference(hello->plugins, sessionPlugins, options.ignoredPlugins));
				return;
			} else {
				// Since protocol 36 any other mods, in any order: their game translates by plugin name (FormIds.h), and
				// what comes from a plugin a game lacks is skipped there. Their game says which plugins differ.
				log(std::format("server: a joiner's load order differs ({}); their form IDs are translated by plugin name", describe(a_player.key)));
			}
		}

		// Someone coming back gets their old ID. If their old connection is still here, their game
		// restarted before it timed out: that one is stale, so it goes.
		bool returning = false;
		bool newIdentity = false;
		if (hello->identity != 0) {
			if (const auto known = identities.find(hello->identity); known != identities.end()) {
				const auto stale = std::ranges::find_if(players, [&](const auto& a_entry) {
					return a_entry.first != a_player.key && a_entry.second.welcomed && a_entry.second.id == known->second;
				});
				if (stale != players.end()) {
					log(std::format("server: dropping the old connection of player {} ({})", stale->second.id, describe(stale->first)));
					leave(stale->second);
					disconnect(stale->second);
					unbindBudget(stale->second);
					players.erase(stale);
				}
				a_player.id = known->second;
				returning = true;
			} else {
				newIdentity = identities.size() < MAX_IDENTITIES;
			}
		}
		// (After dropping a stale connection: it doesn't take up a place.)
		if (welcomedCount() >= options.maxPlayers) {
			reject(a_player, "Server is full");
			return;
		}
		// Only now: someone turned away for a full server was never in the session, which a locked one goes by.
		if (newIdentity) {
			identities[hello->identity] = a_player.id;
			touched();
		}
		removed.erase(hello->identity);
		removedAccounts.erase(a_player.transportId);
		if (hello->identity != 0) {
			if (!members.contains(hello->identity) && members.size() >= MAX_IDENTITIES) {
				// Full: the member gone longest makes room (one in the session now is never the one).
				auto oldest = members.end();
				for (auto it = members.begin(); it != members.end(); ++it) {
					const bool here = std::ranges::any_of(players, [&](const auto& a_entry) { return a_entry.second.welcomed && a_entry.second.helloId == it->first; });
					if (!here && (oldest == members.end() || it->second.lastSeen < oldest->second.lastSeen)) {
						oldest = it;
					}
				}
				if (oldest != members.end()) {
					members.erase(oldest);
				}
			}
			members[hello->identity] = { a_player.transportId, Clock::now() };
		}

		// Every letter of every script stays (a Cyrillic or CJK character name); what can't be shown goes (common/Name.h).
		a_player.name = Name::Shown(hello->name, a_player.id);
		a_player.appearance = hello->appearance;
		// The session's plugin table: their plugins it lacks get places after the rest of their kind (an ID it gave
		// stays what it was), and the players already in hear the new table on the reliable channel, ahead of anything
		// numbered by it. Then the newcomer's, before the welcome. Their hello's one form ID, sent before they knew
		// the table, is translated here.
		if (const auto added = FormIds::Extend(sessionTable, hello->plugins); added != 0) {
			++sessionTableVersion;
			touched();
			log(std::format("server: the session's plugin table has {} plugins ({} new)", FormIds::Table::Of(sessionTable).Size(), added));
			if (welcomedCount() != 0) {
				broadcast(Protocol::Encode(Protocol::PluginTable{ sessionTable }), true, a_player.key);
			}
		}
		if (!sessionTable.empty()) {
			send(a_player.key, Protocol::Encode(Protocol::PluginTable{ sessionTable }), true);
			if (!hello->plugins.empty()) {
				a_player.appearance = FormIds::Mapper(hello->plugins, sessionTable).ToSession(hello->appearance).value_or(0);
			}
		}
		a_player.welcomed = true;
		a_player.local = local;
		a_player.plugins = hello->plugins;
		a_player.storageMapper.reset();  // made again for these plugins
		a_player.dropListAt.reset();
		a_player.helloId = hello->identity;
		a_player.worldAskedAt = Clock::now();
		// Their game's world is its save's now: what it keeps is closed to friends until it lists it again, and what it does
		// at storage others keep waits for its settle (handleStorageSettle).
		tellViewsClosed(storage.Stale(a_player.id));
		closeOwnViews(a_player);
		a_player.worldAsked = 1;
		a_player.storageSettled = !options.rules.sharedStorage;
		send(a_player.key, Protocol::Encode(Protocol::Welcome{ a_player.id, sessionId, options.friendlyFire, options.sharedStory, options.rules.Clamped() }), true);
		// Whose game keeps each storage container, ahead of the world state: their game judges its changes there by it.
		if (options.rules.sharedStorage) {
			sendStorageOwners(storage.Owners(), a_player.key);
		}
		giveWorldState(a_player, hello->world.sessionId == sessionId ? hello->world.containerFrom : 0);

		for (auto& [key, other] : players) {
			if (!other.welcomed || key == a_player.key) {
				continue;
			}
			send(a_player.key, Protocol::Encode(Protocol::PlayerJoined{ other.id, other.name, other.appearance }), true);
			send(key, Protocol::Encode(Protocol::PlayerJoined{ a_player.id, a_player.name, a_player.appearance }), true);
			// Their last report, even one with nothing on: without one, the newcomer's stand-in of them stays in the
			// settler's own outfit (StandInGear dresses only from a report) while everyone else's is undressed.
			if (other.equipmentReported) {
				send(a_player.key, Protocol::Encode(Protocol::Equipment{ other.id, other.equipment, other.equipmentMods }, Protocol::MessageType::kPlayerEquipment), true);
			}
			if (other.face) {
				send(a_player.key, Protocol::Encode(*other.face, Protocol::MessageType::kPlayerFace), true);
			}
			if (other.status) {
				send(a_player.key, Protocol::Encode(*other.status, Protocol::MessageType::kPlayerStatus), true);
			}
			// Let the newcomer see players who are standing still right away.
			other.stateDirty = other.hasState;
		}

		// The NPCs other games made at runtime and share now: the newcomer places its copies, before it hears who runs them.
		for (const auto& [ref, spawn] : spawns) {
			if (spawn.playerId != a_player.id) {
				send(a_player.key, Protocol::Encode(spawn, Protocol::MessageType::kSpawned), true);
			}
		}
		// ... and the frames of those lent out now: the newcomer hides its copy of each while its borrower wears it.
		for (const auto& [ref, loan] : loans) {
			if (loan.granted) {
				send(a_player.key, Protocol::Encode(loan.lending, Protocol::MessageType::kFrameLent), true);
			}
		}
		std::vector<Protocol::ActorOwner> owners;
		for (const auto& [ref, owner] : actorOwners) {
			owners.push_back({ ref, owner.playerId });
		}
		for (std::size_t i = 0; i < owners.size(); i += Protocol::MAX_ACTORS_PER_PACKET) {
			const auto end = (std::min)(owners.size(), i + Protocol::MAX_ACTORS_PER_PACKET);
			send(a_player.key, Protocol::Encode(std::vector<Protocol::ActorOwner>(owners.begin() + i, owners.begin() + end)), true);
		}
		// ... and what those NPCs are wearing, as their owners reported it.
		for (const auto& [ref, items] : npcEquipment) {
			if (const auto owner = actorOwners.find(ref); owner != actorOwners.end()) {
				send(a_player.key, Protocol::Encode(Protocol::NpcEquipment{ owner->second.playerId, ref, items }, Protocol::MessageType::kNpcEquipment), true);
			}
		}
		// ... and what they were rolled as.
		for (const auto& [ref, identity] : npcIdentities) {
			if (actorOwners.contains(ref)) {
				send(a_player.key, Protocol::Encode(identity, Protocol::MessageType::kNpcIdentity), true);
			}
		}
		sendWorkshop(a_player);

		// Not to the host's own game: its clock is the session's, and a saved time would turn its clock back to
		// when the session was last saved. (A dedicated server has no such game; it restores the saved time.)
		if (worldTime && !a_player.local) {
			send(a_player.key, Protocol::Encode(*worldTime, Protocol::MessageType::kWorldTime), true);
		}

		log(std::format("server: player {} {} from {} ({} online)", a_player.id, returning ? "came back" : "joined", describe(a_player.key),
			welcomedCount()));
		// The host's game: the host controls' state, for its menu.
		if (local) {
			send(a_player.key, Protocol::Encode(hostInfo(Protocol::HostResult::kNone, Protocol::HostAction::kKick)), true);
		}
	};

	const auto handleState = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}
		a_player.rate.Roll(Clock::now());
		if (++a_player.rate.states > MAX_STATES_PER_SECOND) {
			return;
		}
		const auto state = Protocol::DecodePlayerState(a_data);
		if (!state) {
			return;
		}
		if (a_player.hasState && state->sequence <= a_player.state.sequence) {
			return;  // out of order
		}
		a_player.state = *state;
		a_player.hasState = true;
		a_player.stateDirty = true;
	};

	const auto noteHostHadAlive = [&](const Player& a_player, std::uint32_t a_refId) {
		if (a_player.local && hostHadAlive.size() < Protocol::MAX_WORLD_STATE_ACTORS) {
			hostHadAlive.insert(a_refId);
		}
	};

	// The player hurt the NPC or tried to take it over (see handleDeath).
	const auto noteFighting = [&](Player& a_player, std::uint32_t a_refId, Clock::time_point a_now) {
		auto& fighting = a_player.fighting;
		if (fighting.size() >= MAX_FIGHTING && !fighting.contains(a_refId)) {
			std::erase_if(fighting, [&](const auto& a_entry) { return a_now - a_entry.second > options.deathGrace; });
			if (fighting.size() >= MAX_FIGHTING) {
				return;
			}
		}
		fighting[a_refId] = a_now;
	};
	const auto wasFighting = [&](const Player& a_player, std::uint32_t a_refId, Clock::time_point a_now) {
		const auto it = a_player.fighting.find(a_refId);
		return it != a_player.fighting.end() && a_now - it->second <= options.deathGrace;
	};
	// The NPC no longer runs in this player's game: another player took it over, or theirs let it go. It ran there
	// until now, so a death their game saw happen is believed for a while yet (see handleDeath).
	const auto noteRan = [&](std::uint32_t a_playerId, std::uint32_t a_refId, Clock::time_point a_now) {
		for (auto& [key, other] : players) {
			if (other.id == a_playerId && other.welcomed) {
				noteFighting(other, a_refId, a_now);
			}
		}
	};
	Clock::time_point droppedDeathLogged{};
	std::size_t       droppedDeaths = 0;  // since the last line about them
	Clock::time_point staleHealthLogged{};
	std::size_t       staleHealthDropped = 0;  // health reports from a game that no longer runs the NPC, since the last line
	Clock::time_point deadHealthLogged{};
	std::size_t       deadHealthDropped = 0;  // health and damage reports for an NPC the session has dead, since the last line
	Clock::time_point deadHitLogged{};
	std::size_t       deadHitsDropped = 0;  // hits on stand-ins by an NPC the session has dead, since the last line
	Clock::time_point hitBudgetLogged{};
	std::size_t       hitBudgetDropped = 0;  // hits on stand-ins past the reporter's damage a second, since the last line
	std::size_t       deadReminders = 0;     // deaths told again to a game that had the NPC alive (remindDead)

	// The session has the NPC dead, and this player's game just had it alive: it hit it, offered to run it, or the NPC
	// hit a stand-in there (its cell reset in that game, or its save is from before the kill). Told its death again, as
	// the world state would, at most once per Options::remindDeadEvery for each NPC: its game kills its copy (WorldSync's
	// TakeDeath kills one alive here though known dead), so every game has it dead until the host's world brings it
	// back. Before, every report about it was dropped without a word and the game fought a copy alive only there. Not
	// the host's game: its offer to run one that came back is the revival (handleClaims), and a reminder would kill
	// what came back.
	// Not at the first report either: the players mostly walk into a reset camp together, and the host's game offers
	// each raider in the same moment the joiner's does. A reminder sent on the joiner's claim, with the host's offer
	// next in the queue, killed the joiner's copy just before the host's game brought the raider back: a corpse for the
	// joiner, hitting them unseen, for the rest of the session. So the death is told again only once the game has
	// reported about the NPC for Options::aliveAgainGrace, the time the host's game is given to bring one back, with the
	// NPC still dead; then every Options::remindDeadEvery while it keeps reporting. A silence of aliveAgainGrace ends
	// the run: its next report starts the wait again (its cell reset once more, days later).
	const auto remindDead = [&](Player& a_player, std::uint32_t a_refId, Clock::time_point a_now) {
		if (a_player.local || !a_player.welcomed) {
			return;
		}
		auto& reminded = a_player.deadReminded;
		auto  it = reminded.find(a_refId);
		if (it == reminded.end()) {
			if (reminded.size() >= MAX_REMINDED) {
				std::erase_if(reminded, [&](const auto& a_entry) { return a_now - a_entry.second.last >= options.aliveAgainGrace; });
				if (reminded.size() >= MAX_REMINDED) {
					return;
				}
			}
			it = reminded.try_emplace(a_refId, Player::DeadReminder{ a_now, a_now, {} }).first;
		} else if (a_now - it->second.last >= options.aliveAgainGrace) {
			it->second = { a_now, a_now, {} };
		}
		auto& reminder = it->second;
		reminder.last = a_now;
		if (a_now - reminder.since < options.aliveAgainGrace || (reminder.told != Clock::time_point{} && a_now - reminder.told < options.remindDeadEvery)) {
			return;
		}
		reminder.told = a_now;
		++deadReminders;
		const auto at = deathPositions.find(a_refId);
		Protocol::ActorDeath death{ a_refId, 0, false };
		if (at != deathPositions.end()) {
			death.x = at->second[0];
			death.y = at->second[1];
			death.z = at->second[2];
		}
		send(a_player.key, Protocol::Encode(death, Protocol::MessageType::kActorDied), true);
	};

	// A spawn died: in its owner's game, or in the game of a player fighting their copy of it (they hurt it or tried to
	// take it over within Options::deathGrace, as for any NPC another game runs). Not kept with the session's dead: it is
	// in no save but its owner's, and its id may name another spawn later. Its owner's game kills it (a friend's kill),
	// the others their copies, and then it is nobody's: an owner of 0 ends it everywhere.
	const auto handleSpawnDeath = [&](Player& a_player, Protocol::ActorDeath a_death) {
		const auto it = spawns.find(a_death.refId);
		if (it == spawns.end() || a_death.foundDead) {
			return;
		}
		const auto now = Clock::now();
		if (it->second.playerId != a_player.id && !wasFighting(a_player, a_death.refId, now)) {
			++droppedDeaths;
			if (now - droppedDeathLogged >= DROPPED_DEATH_LOG_INTERVAL) {
				droppedDeathLogged = now;
				log(std::format("server: death of spawn {:08X} from player {} dropped: player {} runs it and the reporter wasn't fighting it ({} dropped since the last such line)",
					a_death.refId, a_player.id, it->second.playerId, std::exchange(droppedDeaths, 0)));
			}
			return;
		}
		spawns.erase(it);
		actorOwners.erase(a_death.refId);
		npcEquipment.erase(a_death.refId);
		npcIdentities.erase(a_death.refId);
		a_death.playerId = a_player.id;
		broadcast(Protocol::Encode(a_death, Protocol::MessageType::kActorDied), true, a_player.key);
		sendOwners({ { a_death.refId, 0 } }, 0);
		endLoansOfGoneSpawns();
	};

	const auto handleDeath = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto death = Protocol::DecodeActorDeath(a_data);
		if (death && Protocol::IsSpawnRef(death->refId)) {
			handleSpawnDeath(a_player, *death);
			return;
		}
		if (!death || !Protocol::IsShareableRef(death->refId)) {
			return;
		}
		// Dead already. A game reporting it again saw it die (or found it dead): an older build's death is then no
		// mistake, and the host's game can no longer undo it (see handleClaims).
		if (deadActors.contains(death->refId)) {
			if (healable.erase(death->refId)) {
				touched();
			}
			// From the game that reported it as it began to fall: where the corpse came to rest. Kept for joiners and passed
			// on, not as a kill (the kill feed had it with the death). Once, and only soon after the death.
			const bool placed = death->x != 0.0f || death->y != 0.0f || death->z != 0.0f;
			const auto rest = restFrom.find(death->refId);
			if (rest != restFrom.end() && rest->second.playerId == a_player.id && placed && !death->foundDead &&
				Clock::now() - rest->second.at < Protocol::CORPSE_REST_WINDOW) {
				restFrom.erase(rest);
				if (deathPositions.contains(death->refId) || deathPositions.size() < Protocol::MAX_WORLD_STATE_ACTORS) {
					deathPositions[death->refId] = { death->x, death->y, death->z };
				}
				death->killed = false;
				death->playerId = a_player.id;
				broadcast(Protocol::Encode(*death, Protocol::MessageType::kActorDied), true, a_player.key);
			}
			return;
		}
		if (deadActors.size() >= maxWorldEntries) {
			return;
		}
		// An NPC another player's game runs dies in that game; a report from anyone else is a mistake
		// or mischief (it would kill the NPC for everyone and be saved). Only the host's game is believed
		// about one, and only that it was already dead in the host's save (a found-dead report: WorldSync's
		// CheckFoundDead): the host's world is the session's, so it is dead for everyone, even though a
		// joiner's game took it over because it was alive there (a joiner's game ran bloatflies the host's
		// save had dead, and they fought the host's stand-in). The host's game sends those only for NPCs its
		// story shares (with everyone on their own story, not a story character: FoundDead::Shared). A death
		// it saw happen is no such report, even one without a corpse position (an NPC with no 3D there, killed
		// by a quest's script): of that, the runner's game is the judge. And a found-dead report counts only
		// while the host's game hasn't had the NPC alive this session; if it has, the NPC died in the host's
		// game during the session, after the runner had it alive, which is again the runner's to judge (the
		// host's copy of an NPC another game runs can fall over by mistake; this report is then the same
		// mistake found again after a reconnect or a load).
		// A kill by a player who was fighting it is believed too: they hurt it or tried to take it over within
		// Options::deathGrace. The takeover is refused for 5 s after the NPC changed hands, so two players
		// fighting one raider used to lose the second one's kill (it lived on in the runner's game). Their
		// game sends the takeover claim the frame it hits and the death a frame later (Session::Frame), on the
		// one reliable channel, so even a one-shot kill finds the claim here first. So is a kill by the player
		// whose game ran it until moments ago (noteRan): the death event comes seconds after the killing blow,
		// and the NPC can change hands in between, while the friend's game, where it still lived, took it over.
		if (const auto owner = actorOwners.find(death->refId); owner != actorOwners.end() && owner->second.playerId != a_player.id) {
			const auto now = Clock::now();
			const bool foundDead = a_player.local && death->foundDead && !hostHadAlive.contains(death->refId);
			const bool fought = !death->foundDead && wasFighting(a_player, death->refId, now);
			if (!foundDead && !fought) {
				noteHostHadAlive(a_player, death->refId);  // the host's game had it alive, or saw it die
				++droppedDeaths;
				if (now - droppedDeathLogged >= DROPPED_DEATH_LOG_INTERVAL) {
					droppedDeathLogged = now;
					log(std::format("server: death of {:08X} from player {} dropped: player {} runs it and the reporter wasn't fighting it ({} dropped since the last such line)",
						death->refId, a_player.id, owner->second.playerId, std::exchange(droppedDeaths, 0)));
				}
				return;
			}
		}
		if (deadActors.contains(death->refId) || !withinShare(a_player, kDeaths)) {
			return;
		}
		// Within the reporter's budget (what was refused or known already above costs nothing). One over it
		// is dropped for good: the reporter's game counts it as sent.
		if (!a_player.local) {
			auto& budget = budgetOf(a_player);
			const auto now = Clock::now();
			if (!spend(a_player, &RateBudget::deaths, now)) {
				++budget.deathsDropped;
				if (now - budget.deathsLogged >= BUDGET_LOG_INTERVAL) {
					budget.deathsLogged = now;
					log(std::format("server: player {} reports deaths faster than any fight; dropping them until their budget refills ({} dropped since the last such line)", a_player.id, std::exchange(budget.deathsDropped, 0)));
				}
				return;
			}
		}
		deadActors.insert(death->refId);
		++budgetOf(a_player).added[kDeaths];
		noteFill(kDeaths, deadActors.size());
		touched();
		// A death older than aliveAgainGrace is the same as one with no entry (see handleClaims): those make room.
		const auto diedNow = Clock::now();
		if (diedAt.size() >= Protocol::MAX_WORLD_STATE_ACTORS) {
			std::erase_if(diedAt, [&](const auto& a_entry) { return diedNow - a_entry.second >= options.aliveAgainGrace; });
		}
		if (diedAt.size() < Protocol::MAX_WORLD_STATE_ACTORS) {
			diedAt[death->refId] = diedNow;
		}
		if ((death->x != 0.0f || death->y != 0.0f || death->z != 0.0f) && deathPositions.size() < Protocol::MAX_WORLD_STATE_ACTORS) {
			deathPositions[death->refId] = { death->x, death->y, death->z };
		}
		if (!death->foundDead) {
			if (restFrom.size() >= Protocol::MAX_WORLD_STATE_ACTORS) {
				std::erase_if(restFrom, [&](const auto& a_entry) { return diedNow - a_entry.second.at >= Protocol::CORPSE_REST_WINDOW; });
			}
			if (restFrom.size() < Protocol::MAX_WORLD_STATE_ACTORS) {
				restFrom[death->refId] = { a_player.id, diedNow };
			}
		}
		if (actorOwners.erase(death->refId)) {
			sendOwners({ { death->refId, 0 } }, 0);
		}
		npcEquipment.erase(death->refId);
		npcIdentities.erase(death->refId);
		death->playerId = a_player.id;
		broadcast(Protocol::Encode(*death, Protocol::MessageType::kActorDied), true, a_player.key);
	};

	// A health report (kReportHealth: its health now, passed on as kActorHealth), or what the sender took off an NPC
	// another game runs (kReportDamage, as kActorDamaged: the game that runs it takes it off). Both tell the server the
	// sender fights it.
	const auto handleHealth = [&](Player& a_player, std::span<const std::uint8_t> a_data, Protocol::MessageType a_out) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto health = Protocol::DecodeActorHealth(a_data);
		// A spawn's health goes to its owner too, whoever hit their copy of it.
		const bool spawn = health && spawns.contains(health->refId);
		if (!health || (!spawn && !Protocol::IsShareableRef(health->refId))) {
			return;
		}
		// Dead for the session: nothing to pass on, and the sender's game, which has it alive, is told so (remindDead).
		// A spawn's death ends it (handleSpawnDeath), so this is a plugin's NPC.
		if (deadActors.contains(health->refId)) {
			++deadHealthDropped;
			const auto now = Clock::now();
			if (now - deadHealthLogged >= DROPPED_DEATH_LOG_INTERVAL) {
				deadHealthLogged = now;
				log(std::format("server: {} of {:08X} from player {} dropped: the session has it dead ({} since the last such line)",
					a_out == Protocol::MessageType::kActorDamaged ? "damage" : "health", health->refId, a_player.id, std::exchange(deadHealthDropped, 0)));
			}
			remindDead(a_player, health->refId, now);
			return;
		}
		if (a_out == Protocol::MessageType::kActorDamaged && !(health->health > 0.0f && health->health <= 1.0f)) {
			return;  // no damage, or more than all of it
		}
		if (!spawn) {
			noteHostHadAlive(a_player, health->refId);  // hit and still alive
		}
		noteFighting(a_player, health->refId, Clock::now());
		// Where an NPC's health stands is said by the game that runs it, or by anyone while nobody does. A report from
		// any other game is from before it learnt who runs the NPC now: a takeover by a friend's hit, granted while the
		// game that had run it sent its last report. Passed on, it set the new runner's NPC back to that older health,
		// undoing the very hit that took it over (and the runner then shared that as where it stood): the hit healed.
		if (const auto owner = actorOwners.find(health->refId);
			a_out == Protocol::MessageType::kActorHealth && !spawn && owner != actorOwners.end() && owner->second.playerId != a_player.id) {
			++staleHealthDropped;
			if (const auto now = Clock::now(); now - staleHealthLogged >= DROPPED_DEATH_LOG_INTERVAL) {
				staleHealthLogged = now;
				log(std::format("server: health of {:08X} from player {} not passed on: player {} runs it ({} since the last such line)", health->refId, a_player.id,
					owner->second.playerId, std::exchange(staleHealthDropped, 0)));
			}
			return;
		}
		broadcast(Protocol::Encode(*health, a_out), true, a_player.key);
	};

	// Container changes are numbered and echoed to their sender too, so every player knows exactly
	// which of them its world contains (see Protocol::WorldRequest).
	// A player's container change, numbered (and sent to every game) if it may be. a_late: a move at storage another game
	// keeps that their game made before its connection went and the session never numbered, sent again with a settle
	// (StorageBook::Book::Check's a_late).
	const auto takeContainerChange = [&](Player& a_player, const std::optional<Protocol::ContainerChange>& change, bool a_late) {
		// With the rule off the book is only kept for a later run: no container has a keeper.
		const auto keeper = options.rules.sharedStorage && change && Protocol::IsValid(*change) ? storage.KeeperOf(change->container) : std::nullopt;
		const bool atFriends = keeper && *keeper != a_player.id;
		// The budget first, before the book is asked or anything answered: a friend's moves at storage another game keeps
		// have their own (STORAGE_MOVE_BURST), everything else the event budget.
		const auto now = Clock::now();
		const bool allowed = atFriends || a_late ? budgetOf(a_player).storageMoves.Take(now) : allowEvent(a_player);
		if (!change || !Protocol::IsValid(*change)) {
			return;
		}
		// A friend's move at storage another game keeps (common/StorageBook.h) goes through only if the book can check it.
		// Whatever doesn't go through is refused to their game, which undoes it: their item left their inventory (or came
		// into it) already, and a move dropped without a word would lose it, or dupe it. Past the refusals' own budget a
		// move gets no answer at all: their game keeps it on its way and sends it again late at its next settle.
		const auto refuse = [&](Protocol::StorageReason a_reason) {
			if (!budgetOf(a_player).storageRefusals.Take(now)) {
				++storageUnanswered;
				return;
			}
			++storageRefused;
			send(a_player.key, Protocol::Encode(Protocol::StorageRefusal{ change->container, change->item, change->count, Protocol::STORAGE_NOT_NUMBERED, a_reason }), true);
		};
		if (!allowed) {
			if (atFriends || a_late) {
				refuse(Protocol::StorageReason::kCantHold);
			}
			return;
		}
		if (a_late && !atFriends) {
			refuse(Protocol::StorageReason::kClosed);  // a late move is only ever one at storage another game keeps
			return;
		}
		// Their game hasn't settled its moves since their world last loaded (Protocol::StorageSettle): nothing it does at
		// storage another game keeps goes through until it has, so their moves the true copy undoes are still there to undo.
		if (atFriends && !a_late && !a_player.storageSettled) {
			refuse(Protocol::StorageReason::kClosed);
			return;
		}
		const auto verdict = !atFriends ? (keeper ? StorageBook::Verdict::kKeeper : StorageBook::Verdict::kNotStorage) :
		                                  storage.Check(a_player.id, *change, [&](std::uint32_t a_keeper, std::uint32_t a_item) {
			if ((a_item >> 24) == 0xFF) {
				return false;  // made in their game alone: no other has it
			}
			const auto holder = std::ranges::find_if(players, [&](const auto& a_entry) { return a_entry.second.welcomed && a_entry.second.id == a_keeper; });
			if (holder == players.end() || holder->second.plugins.empty() || sessionTable.empty()) {
				return true;
			}
			// Their game's numbering against the session's, made once (it changes only as the session's table grows).
			auto& player = holder->second;
			if (!player.storageMapper || player.storageMapperFor != sessionTableVersion) {
				player.storageMapper.emplace(player.plugins, sessionTable);
				player.storageMapperFor = sessionTableVersion;
			}
			return player.storageMapper->ToLocal(a_item).has_value();
		}, a_late);
		const bool checked = verdict == StorageBook::Verdict::kAccept;
		switch (verdict) {
		case StorageBook::Verdict::kClosed:
			refuse(Protocol::StorageReason::kClosed);
			return;
		case StorageBook::Verdict::kNotViewing:
			refuse(Protocol::StorageReason::kNotViewing);
			return;
		case StorageBook::Verdict::kNotHeld:
			refuse(Protocol::StorageReason::kNotHeld);
			return;
		case StorageBook::Verdict::kCantHold:
			refuse(Protocol::StorageReason::kCantHold);
			return;
		default:
			break;
		}
		if (!allowed || containerChanges.size() >= maxWorldEntries) {
			if (checked) {
				refuse(Protocol::StorageReason::kCantHold);
			}
			return;
		}
		// Their share of the session's table, by player ID (saved with the session) and by budget (this run).
		// The host's own game has none (see withinShare).
		if (!a_player.local && containersAuthored[a_player.id] >= options.maxContainerChangesPerPlayer) {
			if (!std::exchange(budgetOf(a_player).shareLogged[kContainers], true)) {
				log(std::format("server: player {} has made {} container changes this session, their share; no more of theirs are kept", a_player.id, containersAuthored[a_player.id]));
			}
			if (checked) {
				refuse(Protocol::StorageReason::kCantHold);
			}
			return;
		}
		if (!withinShare(a_player, kContainers)) {
			if (checked) {
				refuse(Protocol::StorageReason::kCantHold);
			}
			return;
		}
		++containersAuthored[a_player.id];
		++budgetOf(a_player).added[kContainers];
		const auto index = static_cast<std::uint32_t>(containerChanges.size());
		containerChanges.push_back({ index, a_player.id, *change });
		if (!storage.Numbered(containerChanges.back())) {
			// The keeper's own take of what a friend's take, numbered first, had: its game finds the friend's short.
			log(std::format("server: storage {:08X} held less of {:08X} than player {} took", change->container, change->item, a_player.id));
		}
		noteFill(kContainers, containerChanges.size());
		touched();
		// Not to a player owed the world state: it carries this change, in order after the ones before it.
		// Nor to one whose hello waits for their budget: their game restarted, and the hello's world state
		// carries it. Their new game takes what comes before its Welcome too, and an echo would move its
		// count of what it has past changes its save lacks.
		const auto packet = Protocol::Encode(containerChanges.back());
		for (const auto& [key, other] : players) {
			if (other.welcomed && !other.worldStateOwed && other.waitingHello.empty()) {
				send(key, packet, true);
			}
		}
	};

	const auto handleContainer = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		takeContainerChange(a_player, a_player.welcomed ? Protocol::DecodeContainerChange(a_data) : std::nullopt, false);
	};

	// Shared storage (common/StorageBook.h). The host's game lists what one of its storage containers holds, and so claims
	// it; any other game's listing is nobody's truth and is ignored. The friends viewing it see it as it is now.
	bool storageFullLogged = false;
	const auto handleReportStorage = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!options.rules.sharedStorage || !allowEvent(a_player)) {
			return;
		}
		const auto listing = Protocol::DecodeStorageListing(a_data);
		if (!listing) {
			return;
		}
		bool changed = false;
		const auto listed = storage.List(a_player.id, a_player.local, *listing, containerChanges, &changed);
		const bool claimed = listed == StorageBook::Listed::kClaimed;
		switch (listed) {
		case StorageBook::Listed::kNotHost:
			return;
		case StorageBook::Listed::kFull:
			if (!std::exchange(storageFullLogged, true)) {
				log(std::format("server: {} storage containers are shared, the most there may be; others stay each player's own", storage.Size()));
			}
			return;
		case StorageBook::Listed::kClaimed:
		case StorageBook::Listed::kTaken:
			break;
		}
		if (changed) {
			touched();
			sendStorageOwners({ { listing->container, a_player.id, storage.Since(listing->container) } }, 0);
		}
		if (claimed) {
			// What friends put in it before (ordinary loot then) comes back out of the true copy, after the games know who keeps
			// it (so theirs skip it) and before anyone views it: those items are in their own copies, or back in a world they
			// loaded, already (StorageBook::BeforeClaim). Numbered as changes by nobody, counted against the friend who put most.
			std::size_t unput = 0;
			for (const auto& [change, player] : storage.BeforeClaim(listing->container, containerChanges)) {
				if (containerChanges.size() >= maxWorldEntries) {
					break;
				}
				++containersAuthored[player];
				const auto numbered = static_cast<std::uint32_t>(containerChanges.size());
				containerChanges.push_back({ numbered, 0, change });
				storage.Numbered(containerChanges.back());
				noteFill(kContainers, containerChanges.size());
				touched();
				unput += static_cast<std::size_t>(-static_cast<std::int64_t>(change.count));
				const auto packet = Protocol::Encode(containerChanges.back());
				for (const auto& [key, other] : players) {
					if (other.welcomed && !other.worldStateOwed && other.waitingHello.empty()) {
						send(key, packet, true);
					}
				}
			}
			if (unput != 0) {
				log(std::format("server: storage {:08X} claimed: {} items friends put in it before go back out of the host's copy", listing->container, unput));
			}
		}
		const auto viewers = storage.Viewers(listing->container);
		if (!viewers.empty()) {
			const auto packet = Protocol::Encode(storage.ContentsOf(listing->container, static_cast<std::uint32_t>(containerChanges.size())), Protocol::MessageType::kStorageContents);
			for (const auto& [key, other] : players) {
				if (other.welcomed && std::ranges::contains(viewers, other.id)) {
					send(key, packet, true);
				}
			}
		}
	};

	// A friend opened (or closed) their copy of storage another game keeps: what it holds, to show, or that it is closed.
	const auto handleViewStorage = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!options.rules.sharedStorage || !allowEvent(a_player)) {
			return;
		}
		const auto view = Protocol::DecodeStorageView(a_data);
		if (!view) {
			return;
		}
		if (!view->open) {
			storage.Close(a_player.id, view->container);
			return;
		}
		// Contents are up to ~8 KB: a few a second at most (their game asks again only once an answer has come). One past
		// that is answered closed, as their game can't be left waiting with its menu open.
		if (!budgetOf(a_player).storageViews.Take(Clock::now())) {
			storage.Close(a_player.id, view->container);
			if (budgetOf(a_player).storageRefusals.Take(Clock::now())) {
				send(a_player.key, Protocol::Encode(Protocol::StorageRefusal{ view->container, 0, 0, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed }), true);
			}
			return;
		}
		// Their game hasn't settled its moves since their world last loaded: no view until it has (see takeContainerChange).
		const auto shown = a_player.storageSettled ? storage.Open(a_player.id, view->container, static_cast<std::uint32_t>(containerChanges.size())) :
		                                             StorageBook::Viewed{ storage.KeeperOf(view->container) ? StorageBook::Viewed::Kind::kClosed : StorageBook::Viewed::Kind::kNotStorage };
		switch (shown.kind) {
		case StorageBook::Viewed::Kind::kShown:
			send(a_player.key, Protocol::Encode(shown.contents, Protocol::MessageType::kStorageContents), true);
			break;
		case StorageBook::Viewed::Kind::kClosed:
		case StorageBook::Viewed::Kind::kNotStorage:
			send(a_player.key, Protocol::Encode(Protocol::StorageRefusal{ view->container, 0, 0, Protocol::STORAGE_NOT_NUMBERED, Protocol::StorageReason::kClosed }), true);
			break;
		case StorageBook::Viewed::Kind::kOwn:
			break;
		}
	};

	// The keeper's game applied a friend's numbered take and its container held less: the friend's game gives back what
	// was missing (its player has an item the session's storage never had).
	const auto handleStorageShort = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!options.rules.sharedStorage || !allowEvent(a_player)) {
			return;
		}
		const auto report = Protocol::DecodeStorageShort(a_data);
		const auto refusal = report ? storage.Short(a_player.id, *report, containerChanges) : std::nullopt;
		if (!refusal) {
			return;
		}
		if (refusal->first == 0) {
			log(std::format("server: storage {:08X} held {} fewer of {:08X} than given up for change #{}", refusal->second.container, report->missing, refusal->second.item,
				report->index));
			return;
		}
		// The friend's change as it was in the end: a catch-up (a load, a rejoin) brings the take of what the container had,
		// so a game that applies it again gives no more than that, and the true copy undoing it (a settle) puts back only
		// that. A change by nobody stays as numbered: no game counts it as its own.
		containerChanges[report->index].change.count += report->missing;
		touched();
		const auto taker = std::ranges::find_if(players, [&](const auto& a_entry) { return a_entry.second.welcomed && a_entry.second.id == refusal->first; });
		if (taker == players.end()) {
			log(std::format("server: player {} took {} of {:08X} from storage {:08X} that held less (change #{}); they are away, so they keep it", refusal->first,
				-refusal->second.count, refusal->second.item, refusal->second.container, refusal->second.index));
			return;
		}
		log(std::format("server: player {} took {} of {:08X} from storage {:08X} that held less (change #{}); their game is asked to give it back", refusal->first,
			-refusal->second.count, refusal->second.item, refusal->second.container, refusal->second.index));
		send(taker->first, Protocol::Encode(refusal->second), true);
	};

	// A friend's game settles its moves at storage another game keeps, once the world state a load or a rejoin asked for
	// has come (Protocol::StorageSettle). The true copy is put right, never their inventory (a player can always move an
	// item out of their own inventory first, never out of the host's container without the server): each of their
	// numbered moves their world lacks is undone there, and each undone before that their world has after all is done
	// again, as changes by nobody (player 0) that every game applies as another's. What goes into the true copy goes
	// first, so what comes out is held back only by what others took meanwhile. Their moves the session never numbered
	// (lost with the connection) go through late. Until it comes, their views and moves there are refused.
	const auto handleStorageSettle = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed || !options.rules.sharedStorage || a_player.storageSettled || a_player.worldStateOwed) {
			return;
		}
		const auto settle = Protocol::DecodeStorageSettle(a_data);
		if (!settle || settle->asked != a_player.worldAsked) {
			return;  // a marker for a world state before their last request: the settle for the last comes after it
		}
		const auto to = (std::min<std::uint32_t>)(settle->next, static_cast<std::uint32_t>(containerChanges.size()));
		const auto from = (std::min)(settle->lacksFrom, to);
		std::set<std::uint32_t> lacking;
		for (const auto index : settle->lacking) {
			if (index >= from && index < to) {
				lacking.insert(index);
			}
		}
		std::vector<std::pair<std::uint32_t, bool>> work;  // (index, their world has it)
		for (const auto index : lacking) {
			work.push_back({ index, false });
		}
		for (const auto index : storage.ReversedOf(a_player.id, from, to)) {
			if (!lacking.contains(index)) {
				work.push_back({ index, true });
			}
		}
		// Into the true copy first (a take undone, a put done again), then out of it.
		const auto adds = [&](const std::pair<std::uint32_t, bool>& a_work) { return (containerChanges[a_work.first].change.count < 0) != a_work.second; };
		std::ranges::stable_partition(work, adds);
		std::size_t undone = 0;
		std::size_t redone = 0;
		// Each change is counted against the settler's own share of the log, like their moves: undoing and doing again
		// their moves between two saves can't fill the table for everyone. All of it goes, or none: a settle cut part way
		// could put back a take's items and leave the matching put's in, in two worlds at once. Past their share (or a full
		// log) they stay unsettled (no view, no move at storage others keep) until a later settle, with room, puts it right.
		std::size_t planned = 0;
		for (const auto& [index, has] : work) {
			planned += storage.Settle(a_player.id, index, has, containerChanges, false) ? 1 : 0;
		}
		const bool shared = a_player.local || planned == 0 ||
		                    (containersAuthored[a_player.id] + planned <= options.maxContainerChangesPerPlayer &&
								budgetOf(a_player).added[kContainers] + planned <= options.maxContainerChangesPerPlayer);
		const bool cut = !shared || containerChanges.size() + planned > maxWorldEntries;
		if (cut) {
			work.clear();
		}
		constexpr std::size_t kShortfallLinesPerSettle = 8;
		std::size_t           shortfalls = 0;
		for (const auto& [index, has] : work) {
			// What the move calls for before the true copy's own count caps it. Nothing for a move already undone (or not the
			// settler's own to undo here): the true copy does nothing for it, and that is no shortfall.
			const auto wanted = storage.Settle(a_player.id, index, has, containerChanges, false);
			const auto change = storage.Settle(a_player.id, index, has, containerChanges);
			// A settle may hold thousands of moves (MAX_STORAGE_LACKING): the first few get a line each, the rest are counted.
			if (wanted && wanted->count < 0 && (change ? change->count : 0) > wanted->count && ++shortfalls <= kShortfallLinesPerSettle) {
				log(std::format("server: storage {:08X} held {} fewer of {:08X} than {} player {}'s move #{} needed to take out; only what it held went", containerChanges[index].change.container,
					(change ? change->count : 0) - wanted->count, containerChanges[index].change.item, has ? "doing again" : "undoing", a_player.id, index));
			}
			if (!change) {
				continue;
			}
			if (!a_player.local) {
				++containersAuthored[a_player.id];
				++budgetOf(a_player).added[kContainers];
			}
			const auto numbered = static_cast<std::uint32_t>(containerChanges.size());
			containerChanges.push_back({ numbered, 0, *change });
			storage.Numbered(containerChanges.back());
			noteFill(kContainers, containerChanges.size());
			touched();
			++(has ? redone : undone);
			const auto packet = Protocol::Encode(containerChanges.back());
			for (const auto& [key, other] : players) {
				if (other.welcomed && !other.worldStateOwed && other.waitingHello.empty()) {
					send(key, packet, true);
				}
			}
		}
		if (shortfalls > kShortfallLinesPerSettle) {
			log(std::format("server: and {} more of player {}'s moves found storage holding less than they needed to take out", shortfalls - kShortfallLinesPerSettle,
				a_player.id));
		}
		a_player.storageSettled = !cut;
		if (cut) {
			log(std::format("server: player {}'s storage moves can't all be put right ({} changes, past their share of the container table or a full one); "
							"none is made, and storage others keep stays closed to them until a later settle can",
				a_player.id, planned));
		}
		if (undone + redone != 0) {
			storageUndone += undone + redone;
			log(std::format("server: storage undid {} and did again {} of player {}'s moves for their loaded world ({} this run)", undone, redone, a_player.id, storageUndone));
		}
		if (!settle->late.empty()) {
			log(std::format("server: player {}'s game sends {} storage moves the session never numbered", a_player.id, settle->late.size()));
		}
		for (const auto& change : settle->late) {
			takeContainerChange(a_player, change, true);
		}
	};

	const auto handlePickup = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto pickup = Protocol::DecodeRefPickedUp(a_data);
		if (!pickup || !Protocol::IsShareableRef(pickup->refId) || pickedUp.size() >= maxWorldEntries || pickedUp.contains(pickup->refId) ||
			!withinShare(a_player, kPickups)) {
			return;
		}
		pickedUp.insert(pickup->refId);
		objectPlaces.Forget(pickup->refId);  // in someone's pockets now, at no place
		++budgetOf(a_player).added[kPickups];
		noteFill(kPickups, pickedUp.size());
		touched();
		broadcast(Protocol::Encode(*pickup, Protocol::MessageType::kRefPickedUp), true, a_player.key);
	};

	const auto handleRefState = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto state = Protocol::DecodeRefState(a_data);
		if (!state || !Protocol::IsShareableRef(state->refId)) {
			return;
		}
		// A door the session doesn't know yet takes a place in the table, and from the player's share.
		if (!refStates.contains(state->refId)) {
			if (refStates.size() >= maxWorldEntries || !withinShare(a_player, kRefStates)) {
				return;
			}
			++budgetOf(a_player).added[kRefStates];
			noteFill(kRefStates, refStates.size() + 1);
		}
		auto& stored = refStates[state->refId];
		if (stored.SameState(*state)) {
			return;
		}
		stored = *state;
		touched();
		stored.playerId = 0;  // a world state names nobody: a joiner takes it as it is
		Protocol::RefState relayed = *state;
		relayed.playerId = a_player.id;  // the others take it only if this player is near the thing
		broadcast(Protocol::Encode(relayed, Protocol::MessageType::kRefStateChanged), true, a_player.key);
	};

	// Who runs an NPC's AI:
	// - nobody yet: the first player to claim it;
	// - a companion follows its player, so that player takes it over, unless it is also another
	//   player's companion (then the first keeps it);
	// - a player fighting or talking to it takes it over, unless it is someone's companion, or it changed hands or
	//   its runner claimed it again (with a hit of theirs, or while they talk to it) within Options::takeoverCooldown:
	//   two players fighting it would otherwise trade it every cooldown, and its AI started over in the taker's game
	//   each time. It stays with whoever hit it first until they stop hitting it; the other's hits reach it as damage
	//   (handleHealth's kReportDamage), as any non-runner's do.
	const auto ownedBy = [&](std::uint32_t a_playerId) {
		return static_cast<std::size_t>(std::ranges::count_if(actorOwners, [&](const auto& a_entry) { return a_entry.second.playerId == a_playerId; }));
	};
	const auto companionsOf = [&](std::uint32_t a_playerId) {
		return static_cast<std::size_t>(std::ranges::count_if(actorOwners, [&](const auto& a_entry) {
			return a_entry.second.playerId == a_playerId && a_entry.second.reason == Protocol::ClaimReason::kCompanion;
		}));
	};

	const auto handleClaims = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto claims = Protocol::DecodeClaims(a_data);
		if (!claims) {
			return;
		}
		using Reason = Protocol::ClaimReason;
		const auto                        now = Clock::now();
		std::vector<Protocol::ActorOwner> changed;
		std::vector<Protocol::ActorOwner> current;
		// How many NPCs (and companions) the player runs: counted once a packet, when a claim on an NPC
		// nobody runs needs it, and kept up to date as claims are granted (counting for every claim made a
		// packet of new IDs cost a scan of the whole table each).
		std::optional<std::size_t> owned;
		std::optional<std::size_t> companions;
		// Whether the player may run one more NPC nobody runs.
		const auto roomFor = [&](Reason a_reason) {
			if (!owned) {
				owned = ownedBy(a_player.id);
				companions = companionsOf(a_player.id);
			}
			return actorOwners.size() < Protocol::MAX_WORLD_STATE_ACTORS && *owned < Protocol::MAX_OWNED_PER_PLAYER && (a_reason != Reason::kCompanion || *companions < MAX_COMPANIONS_PER_PLAYER);
		};
		for (const auto& claim : *claims) {
			// A spawn is run by the game that made it, for good: it is in no other game to run. A player fighting their
			// copy of it is noted, so their kill is believed (handleSpawnDeath), and told who runs it.
			if (const auto spawn = spawns.find(claim.refId); spawn != spawns.end()) {
				if (claim.reason == Reason::kInteract) {
					noteFighting(a_player, claim.refId, now);
				}
				if (spawn->second.playerId != a_player.id) {
					current.push_back({ claim.refId, spawn->second.playerId });
				}
				continue;
			}
			if (!Protocol::IsShareableRef(claim.refId)) {
				continue;
			}
			// The dead can't be claimed, except by the host's game when a cell reset brought them back: the raiders of
			// a camp cleared days ago. Its game offers one only alive and loaded (NpcSync, never one it has dead:
			// WorldSync::MayClaim), and the host's world is the session's, so it is alive again for everyone and runs
			// in the host's game. Only well after the host's game last asked for the world and after the death, so a
			// copy alive only in an older save, or one the news of its death has not reached yet, is not taken for it.
			// A joiner's game can't do this (its world is not the session's), nor can anyone on a dedicated server. Nor
			// can the host's game while its last request for the world is unanswered (owed while its budget refills, or
			// dropped): what it has alive may be from the save it loaded.
			// An NPC the claiming game keeps essential never died there (its game holds the death: WorldSync::TryKill), so it
			// can't have come back. Its claim undoes only a death an older build saved by mistake (healable: those builds
			// took Dogmeat going down for dead, and he then went unrun and dead in everyone else's game), and only from the
			// host's game, whose world is the session's, offering to run it (not a hit or a press), with the claim granted:
			// the owner it gets tells every game it lives (WorldSync::OnActorOwners). A death seen in any run of this layout
			// stays: a quest's script kills even an essential NPC, and a friend's game can still keep it essential for a while.
			if (deadActors.contains(claim.refId) && claim.essential) {
				// With no room to grant it, it stays dead until the host's game offers it again.
				if (!a_player.local || claim.reason == Reason::kInteract || !healable.contains(claim.refId) || !roomFor(claim.reason)) {
					continue;
				}
				deadActors.erase(claim.refId);
				healable.erase(claim.refId);
				deathPositions.erase(claim.refId);
				restFrom.erase(claim.refId);
				diedAt.erase(claim.refId);
				touched();
				log(std::format("server: {:08X}, dead in the saved session, is alive in the host's world and can't die there (essential); no longer dead in the session", claim.refId));
			}
			if (deadActors.contains(claim.refId)) {
				const auto died = diedAt.find(claim.refId);
				const bool cameBack = a_player.local && !a_player.worldStale && claim.reason == Reason::kUnowned && now - a_player.worldAskedAt >= options.aliveAgainGrace &&
				                      (died == diedAt.end() || now - died->second >= options.aliveAgainGrace);
				if (!cameBack) {
					// A joiner's game has it alive (its own cell reset, or an older save): told it is dead, so it kills its copy
					// instead of fighting it alone and offering it every few seconds. The host's game is left to offer it again.
					remindDead(a_player, claim.refId, now);
					continue;
				}
				deadActors.erase(claim.refId);
				healable.erase(claim.refId);
				deathPositions.erase(claim.refId);
				restFrom.erase(claim.refId);
				if (died != diedAt.end()) {
					diedAt.erase(died);
				}
				++aliveAgain;
				touched();
				log(std::format("server: {:08X} is alive again in the host's world (its cell reset); no longer dead in the session", claim.refId));
			}
			if (claim.reason != Reason::kInteract) {
				noteHostHadAlive(a_player, claim.refId);  // offered because it is loaded and alive (a hit or a press can be on a corpse)
			} else {
				noteFighting(a_player, claim.refId, now);  // granted or not: a refused takeover is the kill handleDeath must not lose
			}
			const auto it = actorOwners.find(claim.refId);
			bool       grant = false;
			if (it == actorOwners.end()) {
				grant = roomFor(claim.reason);
			} else if (it->second.playerId == a_player.id) {
				if (companions && it->second.reason != Reason::kCompanion && claim.reason == Reason::kCompanion) {
					++*companions;
				}
				it->second.reason = (std::max)(it->second.reason, claim.reason);
				// The owner talking to it, or hitting it (NpcSync::OnLocalInteraction claims it again with each hit, 2 s
				// apart at most): nobody takes it over mid-conversation, nor mid-fight while its runner keeps hitting it.
				if (claim.reason == Reason::kInteract) {
					it->second.since = now;
				}
				continue;
			} else if (claim.reason == Reason::kCompanion) {
				grant = it->second.reason != Reason::kCompanion;
			} else if (claim.reason == Reason::kInteract) {
				grant = it->second.reason != Reason::kCompanion && now - it->second.since >= options.takeoverCooldown;
			}
			if (grant) {
				if (it != actorOwners.end()) {
					npcEquipment.erase(claim.refId);  // the old runner's gear report; the new one reports its own
					noteRan(it->second.playerId, claim.refId, now);
				}
				actorOwners[claim.refId] = { a_player.id, claim.reason, now };
				changed.push_back({ claim.refId, a_player.id });
				if (owned) {
					++*owned;
					*companions += claim.reason == Reason::kCompanion;
				}
			} else if (it != actorOwners.end()) {
				current.push_back({ claim.refId, it->second.playerId });
			}
		}
		sendOwners(changed, 0);
		sendOwners(current, a_player.key);
	};

	const auto handleFace = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto face = Protocol::DecodeFace(a_data);
		if (!face) {
			return;
		}
		face->playerId = a_player.id;
		a_player.face = *face;  // the latest is what a newcomer gets
		// Every change makes a new base form in each game and respawns the stand-in, so the others hear
		// about a change at most every few seconds; the latest one follows when the time is up (the tick).
		const auto now = Clock::now();
		auto&      budget = budgetOf(a_player);
		if (now - budget.lastFace < options.faceInterval) {
			a_player.faceDirty = true;
			return;
		}
		budget.lastFace = now;
		a_player.faceDirty = false;
		broadcast(Protocol::Encode(*face, Protocol::MessageType::kPlayerFace), true, a_player.key);
	};

	// Something a player built, moved or scrapped at a settlement: everyone else's copy follows.
	const auto handleWorkshopItem = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto item = Protocol::DecodeWorkshopItem(a_data);
		if (!item) {
			return;
		}
		item->playerId = a_player.id;
		// Past their share, what they already have kept can still move or go; nothing new is kept.
		if (workshop.Adds(*item) && !withinShare(a_player, kWorkshop)) {
			return;
		}
		const auto before = workshop.Size();
		const auto outcome = workshop.Report(*item);
		auto&      added = budgetOf(a_player).added[kWorkshop];
		added = workshop.Size() >= before ? added + (workshop.Size() - before) : added - (std::min)(added, before - workshop.Size());
		// Logged once a run: past the cap, every report would be.
		if (const bool scrap = item->op == Protocol::WorkshopOp::kScrapped; outcome.full && !std::exchange(scrap ? scrapsFullLogged : builtFullLogged, true)) {
			log(std::format("server: the session keeps at most {} {}; not keeping {:08X} from player {}, nor any more", scrap ? MAX_WORLD_SCRAPS : MAX_WORKSHOP_ITEMS,
				scrap ? "scrapped pieces of the world" : "built objects", item->refId, a_player.id));
		}
		if (outcome.relay) {
			touched();  // relayed: what is kept changed
			broadcast(Protocol::Encode(*item, Protocol::MessageType::kWorkshopItem), true, a_player.key);
		}
	};

	// Items given between players (common/DropStore.h). A drop is offered to its dropper's game first: it swaps the item
	// it dropped for a copy like everyone's, and says so (kPlaced), and only then is the drop told to the others, so nobody
	// can take an item still in its dropper's world. Each change to what lies there is saved right away: an item given lies
	// in no game's save, only in the session file.
	//
	// Reports and claims are not counted against the event budget (as records aren't): a dropped one would leave an item
	// in no inventory, or in two. Each game sends a handful at most (its claims wait for their answers), and the book caps
	// what one player can have lying there.
	const auto dropsChanged = [&] {
		touched();
		saveRequested = true;
	};

	const auto handleReportDrop = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}
		const auto item = Protocol::DecodeDroppedItem(a_data);
		if (!item) {
			return;
		}
		const auto refuse = [&] {
			// Not kept: their game keeps the item as it is (an ordinary item on its ground, or in its container). Said by a
			// kDropPlaced with no drop ID.
			auto refused = *item;
			refused.dropId = 0;
			refused.ownerId = a_player.id;
			refused.takerId = 0;
			send(a_player.key, Protocol::Encode(refused, Protocol::MessageType::kDropPlaced), true);
		};
		if (item->holder != 0 && !options.rules.containerGifts) {
			refuse();  // a gift, without the host's bContainerGifts: the weapon stays in its player's own copy of the container
			return;
		}
		const auto added = drops.Add(a_player.id, *item);
		switch (added.outcome) {
		case DropStore::Outcome::kAdded:
			send(a_player.key, Protocol::Encode(*added.drop, Protocol::MessageType::kDropPlaced), true);  // the offer
			if (added.drop->holder != 0) {
				dropsChanged();  // a gift's offer is saved (DropStore::DropOffers)
			}
			break;
		case DropStore::Outcome::kRepeat:
			if (added.drop) {
				send(a_player.key, Protocol::Encode(*added.drop, Protocol::MessageType::kDropPlaced), true);
			}
			break;
		case DropStore::Outcome::kFull:
		case DropStore::Outcome::kOwnerFull:
			{
				refuse();
				if (!std::exchange(dropsFullLogged, true)) {
					log(std::format("server: {} items dropped for others lie in the session ({} of player {}); not keeping more", drops.LyingCount(), drops.LyingOf(a_player.id),
						a_player.id));
				}
				break;
			}
		case DropStore::Outcome::kInvalid:
			break;
		}
	};

	const auto handleClaimDrop = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}
		const auto claim = Protocol::DecodeClaimDrop(a_data);
		if (!claim) {
			return;
		}
		switch (claim->op) {
		case Protocol::DropClaim::kGiveBack:
			if (const auto back = drops.GiveBack(a_player.id, claim->dropId)) {
				dropsChanged();
				broadcast(Protocol::Encode(*back, Protocol::MessageType::kDropPlaced), true, 0);
			}
			return;
		case Protocol::DropClaim::kPlaced:
			// Their game swapped its item for a copy: it lies there for the others now. Their game hears it too: until it does,
			// it gives the item back to its world if it leaves (its kPlaced may have gone with the link, and the offer with it).
			if (const auto laid = drops.Confirm(a_player.id, claim->dropId)) {
				dropsChanged();
				broadcast(Protocol::Encode(*laid, Protocol::MessageType::kDropPlaced), true, 0);
			}
			return;
		case Protocol::DropClaim::kKept:
			drops.Keep(a_player.id, claim->dropId);  // nobody else heard of it
			return;
		case Protocol::DropClaim::kTake:
			break;
		}
		if (const auto gone = drops.Take(a_player.id, claim->dropId)) {
			dropsChanged();
			broadcast(Protocol::Encode(*gone), true, 0);
			return;
		}
		// Someone else's already, or gone: their copy goes (naming who has it, as everyone heard).
		send(a_player.key, Protocol::Encode(Protocol::DropGone{ claim->dropId, drops.TakerOf(claim->dropId) }), true);
	};

	const auto answerDropRecord = [&](Player& a_player, const Protocol::DropRecord& a_record, Clock::time_point a_now) {
		a_player.dropRecordAt = a_now;
		const auto out = drops.Reconcile(a_player.id, a_record, sessionId);
		for (const auto& gone : out.withdrawn) {
			broadcast(Protocol::Encode(gone), true, 0);
		}
		for (const auto& drop : out.returned) {
			broadcast(Protocol::Encode(drop, Protocol::MessageType::kDropPlaced), true, 0);
		}
		// Their offers: one their world swapped out lies there for the others now; one it still has never lay anywhere.
		for (const auto& drop : out.confirmed) {
			broadcast(Protocol::Encode(drop, Protocol::MessageType::kDropPlaced), true, a_player.key);
		}
		for (const auto& gone : out.unoffered) {
			send(a_player.key, Protocol::Encode(gone), true);
		}
		if (out.changed) {
			dropsChanged();
		}
		if (!out.withdrawn.empty() || !out.returned.empty()) {
			log(std::format("server: player {}'s world holds {} items that lay in the session (now theirs) and lacks {} it was said to hold (lying there again)", a_player.id,
				out.withdrawn.size(), out.returned.size()));
		}
		if (out.dupRisk != 0) {
			log(std::format("server: player {}'s world holds {} items someone else holds too (a save from before they dropped them)", a_player.id, out.dupRisk));
		}
		// What lies there: after their welcome, and then no more often than DROP_LIST_INTERVAL (see there).
		if (a_player.dropListAt && a_now - *a_player.dropListAt < DROP_LIST_INTERVAL) {
			return;
		}
		a_player.dropListAt = a_now;
		const auto  lying = drops.Lying();
		std::size_t i = 0;
		do {
			const auto end = (std::min)(lying.size(), i + Protocol::MAX_DROPS_PER_PACKET);
			send(a_player.key, Protocol::Encode(Protocol::DropList{ end == lying.size(), { lying.begin() + static_cast<std::ptrdiff_t>(i), lying.begin() + static_cast<std::ptrdiff_t>(end) } }), true);
			i = end;
		} while (i < lying.size());
	};

	// Not counted against the event budget (sent once after a welcome or a load, and a dropped one would leave their game
	// with stale copies); held instead when it comes too often.
	const auto handleDropRecord = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}
		auto record = Protocol::DecodeDropRecord(a_data);
		if (!record) {
			return;
		}
		if (const auto now = Clock::now(); now - a_player.dropRecordAt >= DROP_RECORD_INTERVAL) {
			a_player.heldDropRecord.reset();
			answerDropRecord(a_player, *record, now);
		} else {
			a_player.heldDropRecord = std::move(*record);
		}
	};

	// Which of a player's own workshop pieces their game powers: passed on as theirs, and not kept (the player sends all
	// of them again when someone joins, and now and then).
	const auto handleWorkshopPower = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto power = Protocol::DecodePowerStates(a_data);
		if (!power) {
			return;
		}
		power->playerId = a_player.id;
		broadcast(Protocol::Encode(*power, Protocol::MessageType::kWorkshopPower), true, a_player.key);
	};

	// An NPC's gear, from the player whose game runs it (each game rolls NPC gear on its own).
	const auto handleNpcEquipment = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto equipment = Protocol::DecodeNpcEquipment(a_data);
		if (!equipment) {
			return;
		}
		const auto owner = actorOwners.find(equipment->refId);
		if (owner == actorOwners.end() || owner->second.playerId != a_player.id) {
			return;
		}
		equipment->playerId = a_player.id;
		npcEquipment[equipment->refId] = equipment->items;
		broadcast(Protocol::Encode(*equipment, Protocol::MessageType::kNpcEquipment), true, a_player.key);
	};

	// What an NPC was rolled as, from the player whose game runs it: kept (the first runner's, see npcIdentities) and
	// passed on. The same report again, or another runner's while the first one's stands, goes no further.
	const auto handleNpcIdentity = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto identity = Protocol::DecodeNpcIdentity(a_data);
		if (!identity) {
			return;
		}
		const auto owner = actorOwners.find(identity->refId);
		if (owner == actorOwners.end() || owner->second.playerId != a_player.id) {
			return;
		}
		identity->playerId = a_player.id;
		const auto known = npcIdentities.find(identity->refId);
		if (known != npcIdentities.end() && (known->second.playerId != a_player.id || known->second == *identity)) {
			return;
		}
		npcIdentities[identity->refId] = *identity;
		broadcast(Protocol::Encode(*identity, Protocol::MessageType::kNpcIdentity), true, a_player.key);
	};

	const auto handleRelease = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto refs = Protocol::DecodeRelease(a_data);
		if (!refs) {
			return;
		}
		std::vector<Protocol::ActorOwner> released;
		const auto                        now = Clock::now();
		for (const auto ref : *refs) {
			if (const auto it = actorOwners.find(ref); it != actorOwners.end() && it->second.playerId == a_player.id) {
				actorOwners.erase(it);
				npcEquipment.erase(ref);  // its next runner reports its gear (NpcSync forgets what it sent on a release)
				npcIdentities.erase(ref);  // nobody runs it: whoever runs it next has it as it is there (the roll made the session's, if it took)
				spawns.erase(ref);        // a spawn its owner's game let go of (unloaded) is gone: the copies go
				noteFighting(a_player, ref, now);  // it ran in their game until now (see noteRan)
				released.push_back({ ref, 0 });
			}
		}
		sendOwners(released, 0);
		endLoansOfGoneSpawns();
	};

	// An NPC the player's game made at runtime (a random encounter's raider, an ambush): every other game places a copy
	// of it, and the game that made it runs it for good. Under the spawn id its game picked: one another player's spawn
	// has is refused (the answer names that player, and the game picks another id), and so is one past the player's or the
	// session's share, or past their budget (the answer names nobody, and the game asks again later). A granted one is
	// answered to everyone with its owner, after the copy's details have gone to the others.
	const auto handleSpawn = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto spawn = Protocol::DecodeSpawn(a_data);
		if (!spawn) {
			return;
		}
		spawn->playerId = a_player.id;
		if (const auto it = spawns.find(spawn->refId); it != spawns.end()) {
			sendOwners({ { spawn->refId, it->second.playerId } }, a_player.key);
			return;
		}
		const auto now = Clock::now();
		auto&      budget = budgetOf(a_player);
		const auto theirs = static_cast<std::size_t>(std::ranges::count_if(spawns, [&](const auto& a_entry) { return a_entry.second.playerId == a_player.id; }));
		const bool room = theirs < Protocol::MAX_SPAWNS_PER_PLAYER && spawns.size() < MAX_SPAWNS && actorOwners.size() < Protocol::MAX_WORLD_STATE_ACTORS &&
		                  !actorOwners.contains(spawn->refId);
		if (!room || !budget.spawns.Take(now)) {
			++budget.spawnsRefused;
			if (now - budget.spawnsLogged >= BUDGET_LOG_INTERVAL) {
				budget.spawnsLogged = now;
				log(std::format("server: spawn {:08X} from player {} refused: {} ({} refused since the last such line)", spawn->refId, a_player.id,
					room ? "they share them faster than any fight makes them" : "they or the session share as many as they may", std::exchange(budget.spawnsRefused, 0)));
			}
			sendOwners({ { spawn->refId, 0 } }, a_player.key);
			return;
		}
		spawns[spawn->refId] = *spawn;
		actorOwners[spawn->refId] = { a_player.id, Protocol::ClaimReason::kUnowned, now };
		broadcast(Protocol::Encode(*spawn, Protocol::MessageType::kSpawned), true, a_player.key);
		sendOwners({ { spawn->refId, a_player.id } }, 0);
	};

	// A friend asks to borrow a frame another game shares as a spawn (src/game/FrameLoan.h). Passed on to the frame's owner
	// alone, when their latest status says they lend, nobody else has asked for or holds that frame, and the friend has no
	// other: else the friend is told why not. The owner's game answers with kLendFrame.
	const auto handleBorrow = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto ask = Protocol::DecodeFrameBorrow(a_data);
		if (!ask) {
			return;
		}
		const auto refuse = [&](std::uint32_t a_owner, Protocol::LoanRefusal a_why) {
			send(a_player.key, Protocol::Encode(Protocol::FrameLending{ a_owner, a_player.id, ask->spawnId, a_why, {} }, Protocol::MessageType::kFrameLent), true);
		};
		const auto spawn = spawns.find(ask->spawnId);
		const auto ownerId = spawn != spawns.end() ? spawn->second.playerId : 0;
		const auto ownerKey = ownerId != 0 && ownerId != a_player.id ? keyOfPlayer(ownerId) : 0;
		if (!ownerKey) {
			refuse(ownerId, Protocol::LoanRefusal::kUnavailable);
			return;
		}
		const auto& owner = players.at(ownerKey);
		if (!owner.status || !owner.status->lends) {
			refuse(ownerId, Protocol::LoanRefusal::kNotLending);
			return;
		}
		const auto held = loans.find(ask->spawnId);
		const bool another = std::ranges::any_of(loans, [&](const auto& a_entry) { return a_entry.first != ask->spawnId && a_entry.second.lending.borrowerId == a_player.id; });
		if (another || (held != loans.end() && held->second.lending.borrowerId != a_player.id)) {
			refuse(ownerId, Protocol::LoanRefusal::kLent);
			return;
		}
		if (held != loans.end() && held->second.granted) {
			// Theirs already (their game missed the grant): told again.
			send(a_player.key, Protocol::Encode(held->second.lending, Protocol::MessageType::kFrameLent), true);
			return;
		}
		loans[ask->spawnId].lending = Protocol::FrameLending{ ownerId, a_player.id, ask->spawnId, Protocol::LoanRefusal::kNone, {} };
		send(ownerKey, Protocol::Encode(Protocol::FrameBorrow{ a_player.id, ask->spawnId }, Protocol::MessageType::kLoanAsked), true);
	};

	// The owner's game answers a friend's ask: the frame lent, with its kit (to everyone: the borrower fills its copy, every
	// game hides its own of the frame while the borrower wears it), or refused (to the friend who asked alone). Only an
	// answer to an ask the server passed on, for a spawn still theirs.
	// Not on the event budget: one answer per ask the server passed on counts (any other is dropped below), and a dropped
	// one would leave the ask, and the owner's frame, waiting for good.
	const auto handleLend = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}
		auto lending = Protocol::DecodeFrameLending(a_data);
		if (!lending) {
			return;
		}
		lending->ownerId = a_player.id;
		const auto it = loans.find(lending->spawnId);
		const auto spawn = spawns.find(lending->spawnId);
		if (it == loans.end() || it->second.granted || it->second.lending.ownerId != a_player.id || it->second.lending.borrowerId != lending->borrowerId ||
			spawn == spawns.end() || spawn->second.playerId != a_player.id) {
			// An answer to an ask that is over: whatever ended that ask told the owner's game so already (tellAskOver), on
			// the same reliable channel and after the ask it answers.
			return;
		}
		if (lending->refusal != Protocol::LoanRefusal::kNone) {
			loans.erase(it);
			if (const auto asker = keyOfPlayer(lending->borrowerId)) {
				send(asker, Protocol::Encode(*lending, Protocol::MessageType::kFrameLent), true);
			}
			return;
		}
		if (!keyOfPlayer(lending->borrowerId)) {
			loans.erase(it);  // gone since they asked: the owner's game hears the loan ended
			broadcast(Protocol::Encode(Protocol::FrameReturn{ lending->borrowerId, a_player.id, lending->spawnId, true }, Protocol::MessageType::kFrameReturned), true, 0);
			return;
		}
		it->second.lending = *lending;
		it->second.granted = true;
		log(std::format("server: player {} lent frame {:08X} ({} items) to player {}", a_player.id, lending->spawnId, lending->kit.size(), lending->borrowerId));
		broadcast(Protocol::Encode(*lending, Protocol::MessageType::kFrameLent), true, 0);
	};

	// The borrower got out of a frame lent to them (or gave it up unworn): the loan is over, and everyone hears where the
	// frame stands and what came back of its pieces. Only the borrower's word for it counts.
	// Not on the event budget either: one return per loan counts (a second finds none), and a dropped one would leave the
	// loan, and the owner's frame hidden, for as long as the borrower stays.
	const auto handleReturn = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}
		auto back = Protocol::DecodeFrameReturn(a_data);
		if (!back) {
			return;
		}
		const auto it = loans.find(back->spawnId);
		if (it == loans.end() || it->second.lending.borrowerId != a_player.id) {
			return;
		}
		back->borrowerId = a_player.id;
		back->ownerId = it->second.lending.ownerId;
		const bool granted = it->second.granted;
		const auto asked = it->second.lending;
		loans.erase(it);
		if (!granted) {
			tellAskOver(asked);  // an ask taken back: the owner's game forgets it (an answer to it is dropped: handleLend finds no ask)
			return;
		}
		if (!back->ended) {
			// The frame is moved to where the borrower says they left it, but only near where they were last seen: a
			// spot far off, or in another place, is moved to their last state; with no state at all it stays put.
			if (!a_player.hasState) {
				back->cell = 0;
				back->worldspace = 0;
			} else {
				const auto& seen = a_player.state;
				const float dx = back->x - seen.x;
				const float dy = back->y - seen.y;
				const float dz = back->z - seen.z;
				const bool  samePlace = back->cell == seen.cell && back->worldspace == seen.worldspace;
				if (!samePlace || !(dx * dx + dy * dy + dz * dz <= RETURN_REACH * RETURN_REACH)) {
					back->cell = seen.cell;
					back->worldspace = seen.worldspace;
					back->x = seen.x;
					back->y = seen.y;
					back->z = seen.z;
				}
			}
		}
		log(std::format("server: player {} returned frame {:08X} to player {}{}", a_player.id, back->spawnId, back->ownerId, back->ended ? " (unworn)" : ""));
		broadcast(Protocol::Encode(*back, Protocol::MessageType::kFrameReturned), true, 0);
	};

	const auto handleActorStates = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}
		a_player.rate.Roll(Clock::now());
		// Owners send one batch per 64 NPCs up to 20 times a second.
		if (++a_player.rate.actorStates > MAX_ACTOR_BATCHES_PER_SECOND) {
			return;
		}
		auto states = Protocol::DecodeActorStates(a_data);
		if (!states) {
			return;
		}
		// Only the owner's view of an NPC counts.
		std::erase_if(*states, [&](const Protocol::ActorState& a_state) {
			const auto it = actorOwners.find(a_state.refId);
			return it == actorOwners.end() || it->second.playerId != a_player.id;
		});
		if (!states->empty()) {
			broadcast(Protocol::Encode(*states, Protocol::MessageType::kActorStatesRelay), false, a_player.key);
		}
	};

	const auto handlePlayerHit = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto hit = Protocol::DecodePlayerHit(a_data);
		if (!hit || hit->playerId == a_player.id || (hit->byPlayer && !options.friendlyFire)) {
			return;
		}
		// A dead NPC hurts nobody. One the session knows is dead can still be alive for a moment in the
		// reporter's game (dead in the host's save, alive in a joiner's until the news reaches it), or for good (its
		// cell reset there): the reporter's game is told again (remindDead). An attacker of 0 is one the reporter's
		// game can't name on the wire (a plugin the session's table lacks): passed on, the victim judges it by the
		// reporter's distance.
		if (!hit->byPlayer && deadActors.contains(hit->attacker)) {
			++deadHitsDropped;
			const auto now = Clock::now();
			if (now - deadHitLogged >= DROPPED_DEATH_LOG_INTERVAL) {
				deadHitLogged = now;
				log(std::format("server: a hit by {:08X} from player {} dropped: the session has it dead ({} since the last such line)", hit->attacker, a_player.id,
					std::exchange(deadHitsDropped, 0)));
			}
			remindDead(a_player, hit->attacker, now);
			return;
		}
		// A spawn hits a stand-in only in its owner's game: the reporter's own (the victim's game has a copy of it).
		if (!hit->byPlayer && Protocol::IsSpawnRef(hit->attacker)) {
			const auto spawn = spawns.find(hit->attacker);
			if (spawn == spawns.end() || spawn->second.playerId != a_player.id) {
				return;
			}
		}
		// No single hit beyond what the strongest weapon does, and no more per second from one
		// reporter than a fight can produce; the rest is dropped.
		const float damage = (std::min)(hit->damage, MAX_HIT_DAMAGE);
		if (a_player.rate.hitDamage + damage > MAX_HIT_DAMAGE_PER_SECOND) {
			++hitBudgetDropped;
			if (const auto now = Clock::now(); now - hitBudgetLogged >= DROPPED_DEATH_LOG_INTERVAL) {
				hitBudgetLogged = now;
				log(std::format("server: a hit from player {} dropped: more damage a second than a fight deals ({} since the last such line)", a_player.id,
					std::exchange(hitBudgetDropped, 0)));
			}
			return;
		}
		a_player.rate.hitDamage += damage;
		for (const auto& [key, other] : players) {
			if (other.welcomed && other.id == hit->playerId) {
				send(key, Protocol::Encode(Protocol::PlayerHit{ a_player.id, damage, hit->byPlayer, hit->attacker }, Protocol::MessageType::kPlayerDamaged), true);
			}
		}
	};

	// Shots by the player, or by an NPC they run (anyone else's view of that NPC doesn't count).
	const auto handleShot = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}
		a_player.rate.Roll(Clock::now());
		if (++a_player.rate.shots > MAX_SHOTS_PER_SECOND) {
			return;
		}
		const auto shot = Protocol::DecodeShot(a_data);
		if (!shot) {
			return;
		}
		if (shot->refId != 0) {
			const auto it = actorOwners.find(shot->refId);
			if (it == actorOwners.end() || it->second.playerId != a_player.id) {
				return;
			}
		}
		broadcast(Protocol::Encode(Protocol::Shot{ a_player.id, shot->refId, shot->action, shot->sound }, Protocol::MessageType::kShotFired), false, a_player.key);
	};

	const auto handleStatus = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto status = Protocol::DecodePlayerStatus(a_data);
		if (!status) {
			return;
		}
		status->playerId = a_player.id;
		a_player.status = *status;
		broadcast(Protocol::Encode(*status, Protocol::MessageType::kPlayerStatus), true, a_player.key);
	};

	// The session's clock is the host's: the host's own game, else the player here longest.
	const auto handleTime = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		for (const auto& [key, other] : players) {
			if (other.welcomed && key != a_player.key && (other.local > a_player.local || (other.local == a_player.local && other.id < a_player.id))) {
				return;
			}
		}
		auto time = Protocol::DecodeWorldTime(a_data);
		if (!time) {
			return;
		}
		time->playerId = a_player.id;
		worldTime = *time;
		broadcast(Protocol::Encode(*time, Protocol::MessageType::kWorldTime), false, a_player.key);
	};

	const auto handlePing = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		const auto now = Clock::now();
		if (!allowEvent(a_player) || now - a_player.lastPing < Protocol::PING_SERVER_GAP) {
			return;
		}
		auto ping = Protocol::DecodePing(a_data);
		if (!ping) {
			return;
		}
		a_player.lastPing = now;
		ping->playerId = a_player.id;
		broadcast(Protocol::Encode(*ping, Protocol::MessageType::kPinged), true, a_player.key);
	};

	// The name a player asked for last (pendingName) becomes theirs and is told to the others, two a second at most:
	// one that comes sooner waits for the tick that may send it, so the last name sent is always the one that stays,
	// even when the network brings two of a game's once-a-second changes together. A flood costs the others two
	// renames a second. A name that cleans to the one they have already changes nothing.
	const auto flushRename = [&](Player& a_player, Clock::time_point a_now) {
		if (!a_player.welcomed || !a_player.pendingName || a_now - a_player.lastRename < RENAME_INTERVAL) {
			return;
		}
		auto name = *std::exchange(a_player.pendingName, std::nullopt);
		if (name == a_player.name) {
			return;
		}
		a_player.lastRename = a_now;
		a_player.name = std::move(name);
		log(std::format("server: player {} renamed", a_player.id));  // never the name: the log is public
		broadcast(Protocol::Encode(Protocol::Rename{ a_player.id, a_player.name }, Protocol::MessageType::kRenamed), true, a_player.key);
	};

	// Their name changed (a joiner named their new character in the opening after connecting): cleaned as a hello's
	// name is, kept for later joiners' welcomes, and told to the others (flushRename).
	const auto handleRename = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto rename = Protocol::DecodeRename(a_data);
		if (!rename) {
			return;
		}
		a_player.pendingName = Name::Shown(rename->name, a_player.id);
		flushRename(a_player, Clock::now());
	};

	const auto handleActivation = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto activation = Protocol::DecodeActivation(a_data);
		if (!activation) {
			return;
		}
		activation->playerId = a_player.id;
		broadcast(Protocol::Encode(*activation, Protocol::MessageType::kActivation), true, a_player.key);
	};

	// A loose object in the player's hands (ObjectMovers: the first grab wins). Refused, they are told who holds it, so
	// their game lets go of its copy. Unreliable, on a budget of its own: a game sends 15 a second while holding.
	const auto handleObjectHold = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}
		const auto now = Clock::now();
		a_player.rate.Roll(now);
		if (++a_player.rate.objectHolds > Protocol::HOLDS_PER_SECOND) {
			return;
		}
		auto hold = Protocol::DecodeObjectHold(a_data);
		if (!hold) {
			return;
		}
		hold->playerId = a_player.id;
		switch (objectMovers.Hold(*hold, a_player.id, now)) {
		case ObjectMovers::Verdict::kRelay:
			broadcast(Protocol::Encode(*hold, Protocol::MessageType::kObjectHeld), false, a_player.key);
			break;
		case ObjectMovers::Verdict::kRefused:
			if (const auto holder = objectMovers.Holder(hold->refId, now)) {
				send(a_player.key, Protocol::Encode(*holder, Protocol::MessageType::kObjectHeld), true);
			}
			break;
		case ObjectMovers::Verdict::kDropped:
			break;
		}
	};

	// An object the player let go of, or that came to rest after they moved it: to the others, unless another player
	// moves it now (ObjectMovers). A joiner's budget (kept across rejoining) bounds a flood of made-up ones. Where it came
	// to rest is kept (objectPlaces), for a game that loads it later. An object the session has no place for yet takes one
	// from the player's share: a full Places forgets the one moved longest ago, so past it a joiner's made-up objects
	// would push out the host's. Theirs past it still go to the others, as a door's state does.
	const auto handleObjectRest = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto rest = Protocol::DecodeObjectRest(a_data);
		const auto now = Clock::now();
		if (!rest || !budgetOf(a_player).objectRests.Take(now)) {
			return;
		}
		rest->playerId = a_player.id;
		if (objectMovers.Rest(*rest, a_player.id, now) == ObjectMovers::Verdict::kRelay) {
			broadcast(Protocol::Encode(*rest, Protocol::MessageType::kObjectRested), true, a_player.key);
			if (rest->kind != Protocol::ObjectRestKind::kRest || pickedUp.contains(rest->refId)) {
				return;
			}
			if (!objectPlaces.Find(rest->refId)) {
				if (!withinShare(a_player, kObjectPlaces)) {
					return;
				}
				++budgetOf(a_player).added[kObjectPlaces];
			}
			objectPlaces.Rested(*rest);
			touched();
		}
	};

	const auto handleExplosion = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto explosion = Protocol::DecodeExplosion(a_data);
		if (!explosion) {
			return;
		}
		// Only near where the reporter is, or near another player in the same space: an explosion an NPC of the reporter's
		// game set off far from its own player but beside a friend's hurts that friend in their game alone (only a player's
		// own game deals their blows), so the reporter sends it wherever any player could be hit (WeaponFire's
		// NearestPlayer). Far from everyone it is no report: one is otherwise a way to set off anything anywhere. A player
		// who hasn't said where they are yet is near nothing. (Each game also judges the explosion itself: WeaponFire's
		// ApplyExplosion.)
		const auto now = Clock::now();
		if (!a_player.hasState) {
			return;
		}
		const auto near = [&](const Player& a_near, bool a_sameSpace) {
			if (!a_near.hasState || (a_sameSpace && (a_near.state.cell != explosion->cell || a_near.state.worldspace != explosion->worldspace))) {
				return false;
			}
			const float dx = explosion->x - a_near.state.x;
			const float dy = explosion->y - a_near.state.y;
			const float dz = explosion->z - a_near.state.z;
			return dx * dx + dy * dy + dz * dz <= EXPLOSION_RANGE * EXPLOSION_RANGE;  // NaN fails
		};
		if (!near(a_player, false) && std::ranges::none_of(players, [&](const auto& a_entry) { return a_entry.first != a_player.key && a_entry.second.welcomed && near(a_entry.second, true); })) {
			return;
		}
		if (!a_player.local) {
			if (!budgetOf(a_player).rates.explosions.Take(now) || !joinersExplosions.Take(now)) {
				auto& budget = budgetOf(a_player);
				++budget.explosionsDropped;
				if (now - budget.explosionsLogged >= BUDGET_LOG_INTERVAL) {
					budget.explosionsLogged = now;
					log(std::format("server: player {} reports explosions faster than anyone throws; dropping them until their budget refills ({} dropped since the last such line)", a_player.id, std::exchange(budget.explosionsDropped, 0)));
				}
				return;
			}
		}
		explosion->playerId = a_player.id;
		broadcast(Protocol::Encode(*explosion, Protocol::MessageType::kExplosion), true, a_player.key);  // reliable: see EXPLOSION_RANGE
	};

	// Whether a spot is within a_range of where the player last said they were, in the same interior cell or worldspace. A
	// player who hasn't said where they are yet is near nothing.
	const auto nearPlayer = [](const Player& a_player, std::uint32_t a_cell, std::uint32_t a_worldspace, float a_x, float a_y, float a_z, float a_range) {
		if (!a_player.hasState || a_player.state.cell != a_cell || a_player.state.worldspace != a_worldspace) {
			return false;
		}
		const float dx = a_x - a_player.state.x;
		const float dy = a_y - a_player.state.y;
		const float dz = a_z - a_player.state.z;
		return dx * dx + dy * dy + dz * dz <= a_range * a_range;  // NaN fails
	};

	// Destructible objects the player's game damaged past a stage (a car they shot, a barrel their grenade blew). The
	// session keeps each one's lowest health and destroyed for good; only news is relayed (one a friend's report already
	// took further is dropped), so a wreck is never mended and the same stage never comes twice. Each new object takes a
	// place in the table, and from the player's share. Its explosion goes with it whoever reports it, with no window after
	// a shot and the explosion budget not spent: each game sets its own off with the stage, and deals its blows on the
	// NPCs it runs, so a stage passed on without it was a blow dealt nowhere (docs/DESIGN.md, "Who sets off an
	// explosion"). A generous budget of its own (STAGE_EXPLOSIONS_PER_SECOND) bounds only a broken or modified game. A
	// stage comes once in the session, for an object near its reporter (each game's copy of a burning car burns on its
	// own, so a friend who never shot can pass a stage first).
	const auto handleDestruction = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto report = Protocol::DecodeDestructions(a_data);
		if (!report || !a_player.hasState) {
			return;
		}
		const auto now = Clock::now();
		Protocol::Destructions relayed{ a_player.id, {} };
		for (const auto& entry : report->entries) {
			// Only an object near where the reporter is (where the session first heard it stands, once it has): a report
			// is otherwise a way to wreck any car, and set it off, at anyone.
			auto       it = destructions.find(entry.refId);
			const bool placed = it != destructions.end() && (it->second.cell != 0 || it->second.worldspace != 0);
			const auto& at = placed ? it->second : entry;
			if (!nearPlayer(a_player, at.cell, at.worldspace, at.x, at.y, at.z, DESTRUCTION_RANGE)) {
				continue;
			}
			if (it == destructions.end()) {
				if (destructions.size() >= maxWorldEntries || !withinShare(a_player, kDestructions)) {
					continue;
				}
				++budgetOf(a_player).added[kDestructions];
				noteFill(kDestructions, destructions.size() + 1);
				it = destructions.emplace(entry.refId, Protocol::Destruction{ entry.refId, entry.health, entry.destroyed, false, entry.cell, entry.worldspace, entry.x, entry.y, entry.z }).first;
			} else {
				auto& stored = it->second;
				const bool lower = entry.health < stored.health;
				const bool newlyDestroyed = entry.destroyed && !stored.destroyed;
				if (!placed) {
					// Known from the session file, which keeps no positions: where it stands from now on.
					stored.cell = entry.cell, stored.worldspace = entry.worldspace, stored.x = entry.x, stored.y = entry.y, stored.z = entry.z;
				}
				if (!lower && !newlyDestroyed) {
					continue;
				}
				stored.health = (std::min)(stored.health, entry.health);
				stored.destroyed = stored.destroyed || entry.destroyed;
			}
			touched();
			auto out = it->second;
			out.explode = entry.explode && (a_player.local || budgetOf(a_player).rates.stageExplosions.Take(now));
			relayed.entries.push_back(out);
		}
		if (!relayed.entries.empty()) {
			broadcast(Protocol::Encode(relayed, Protocol::MessageType::kDestructions), true, a_player.key);
		}
	};

	// A world trap spent in the reporter's game (TrapState): kept, first come first kept (a take still replaces a disarm),
	// and passed on to everyone else live, so each game whose copy is still armed sets it off there. Only one near where
	// the reporter is (where the session has it, once it has it): a report is otherwise a way to set off any mine at
	// anyone. One taken or disarmed is one the reporter stood at. (Each game judges it again: TrapRules::Live, from where
	// the trap and the reporter's stand-in are there.)
	const auto handleTrap = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto trap = Protocol::DecodeTrapState(a_data);
		if (!trap || !a_player.hasState) {
			return;
		}
		const auto  known = traps.find(trap->refId);
		const auto& at = known != traps.end() ? known->second : *trap;
		const float reach = trap->state == Protocol::TrapState::kFired ? TRAP_RANGE : TRAP_REACH;
		if (!nearPlayer(a_player, at.cell, at.worldspace, at.x, at.y, at.z, reach)) {
			return;
		}
		if (!Protocol::TrapSupersedes(trap->state, known != traps.end() ? std::optional{ known->second.state } : std::nullopt)) {
			return;
		}
		if (known == traps.end()) {
			if (traps.size() >= maxWorldEntries || !withinShare(a_player, kTraps)) {
				return;
			}
			++budgetOf(a_player).added[kTraps];
			noteFill(kTraps, traps.size() + 1);
		} else {
			trap->cell = at.cell, trap->worldspace = at.worldspace, trap->x = at.x, trap->y = at.y, trap->z = at.z;  // where it is stays where it was
		}
		trap->playerId = a_player.id;
		traps[trap->refId] = *trap;
		touched();
		broadcast(Protocol::Encode(Protocol::TrapStates{ true, { *trap } }), true, a_player.key);
	};

	const auto handleLine = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		const auto now = Clock::now();
		if (!allowEvent(a_player) || now - budgetOf(a_player).lastLine < std::chrono::milliseconds(250)) {
			return;
		}
		auto line = Protocol::DecodeLine(a_data);
		if (!line) {
			return;
		}
		budgetOf(a_player).lastLine = now;
		line->playerId = a_player.id;
		broadcast(Protocol::Encode(*line, Protocol::MessageType::kLineSpoken), true, a_player.key);
	};

	const auto handleXp = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto gain = Protocol::DecodeXpGain(a_data);
		if (!gain) {
			return;
		}
		const auto now = Clock::now();
		auto&      budget = budgetOf(a_player);  // kept across a reconnect: leaving and coming back doesn't refill it
		if (budget.xpRefilled == Clock::time_point{}) {
			budget.xpRefilled = now;
		}
		const float elapsed = std::chrono::duration<float>(now - budget.xpRefilled).count();
		budget.xpRefilled = now;
		budget.xpBudget = (std::min)(Protocol::MAX_XP_SHARE, budget.xpBudget + elapsed * Protocol::MAX_XP_SHARE / std::chrono::duration<float>(XP_REFILL_TIME).count());
		budget.xpReports = (std::min)(static_cast<float>(XP_REPORT_BURST), budget.xpReports + elapsed / std::chrono::duration<float>(XP_REPORT_INTERVAL).count());
		gain->xp = (std::min)(gain->xp, budget.xpBudget);
		if (budget.xpReports < 1.0f || gain->xp < 1.0f) {
			return;
		}
		budget.xpReports -= 1.0f;
		budget.xpBudget -= gain->xp;
		gain->playerId = a_player.id;
		broadcast(Protocol::Encode(*gain, Protocol::MessageType::kPartyXp), true, a_player.key);
	};

	// Map markers: only what's new to the session is passed on.
	const auto handleMarkers = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto found = Protocol::DecodeMarkers(a_data);
		if (!found) {
			return;
		}
		Protocol::MarkersFound news{ a_player.id, {} };
		for (const auto& marker : found->markers) {
			const auto it = markers.find(marker.refId);
			if (it == markers.end() && markers.size() >= Protocol::MAX_SESSION_MARKERS) {
				continue;
			}
			auto& flags = markers[marker.refId];
			if ((marker.flags & ~flags) != 0) {
				flags |= marker.flags;
				touched();
				news.markers.push_back({ marker.refId, flags });
			}
		}
		if (!news.markers.empty()) {
			broadcast(Protocol::Encode(news, Protocol::MessageType::kMarkersFound), true, a_player.key);
		}
	};

	// Voice goes to everyone else; each listener sets the volume by distance.
	const auto handleVoice = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}
		a_player.rate.Roll(Clock::now());
		a_player.rate.voiceBytes += static_cast<int>(a_data.size());
		if (a_player.rate.voiceBytes > MAX_VOICE_BYTES_PER_SECOND) {
			return;
		}
		auto voice = Protocol::DecodeVoice(a_data);
		if (!voice) {
			return;
		}
		voice->playerId = a_player.id;
		broadcast(Protocol::Encode(*voice, Protocol::MessageType::kVoiceRelay), false, a_player.key);
	};

	const auto handleRevive = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto revive = Protocol::DecodeRevive(a_data);
		if (!revive || revive->playerId == a_player.id) {
			return;
		}
		for (const auto& [key, other] : players) {
			if (other.welcomed && other.id == revive->playerId) {
				send(key, Protocol::Encode(Protocol::Revive{ a_player.id }, Protocol::MessageType::kRevived), true);
			}
		}
	};

	const auto handleQuestDone = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto done = Protocol::DecodeQuestDone(a_data);
		if (!done) {
			return;
		}
		done->playerId = a_player.id;
		broadcast(Protocol::Encode(*done, Protocol::MessageType::kQuestDone), true, a_player.key);
	};

	const auto handleQuestStage = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto stage = Protocol::DecodeQuestStage(a_data);
		if (!stage || !Protocol::IsShareableRef(stage->quest) || questStages.size() >= maxWorldEntries ||
			questStageKeys.contains(questKey(*stage)) || !withinShare(a_player, kQuestStages)) {
			return;
		}
		// Kept without a reporter: a WorldState (and so the session file) carries none, and a stage caught
		// up from the session moves nobody along with whoever did it.
		stage->playerId = 0;
		questStages.push_back(*stage);
		questStageKeys.insert(questKey(*stage));
		// Who did it, stamped whatever the sender said: a stage that moves the player is only followed by a
		// friend standing with the one who did it.
		stage->playerId = a_player.id;
		++budgetOf(a_player).added[kQuestStages];
		noteFill(kQuestStages, questStages.size());
		touched();
		broadcast(Protocol::Encode(*stage, Protocol::MessageType::kQuestStage), true, a_player.key);
	};

	// A radiant quest's run (RadiantRules) is relayed as it comes and not kept: each run has its own fills, and a
	// stage of one run isn't a stage of the next (a radiant runs again and again), so neither goes in the session's
	// state, whose stages are kept once per quest. A player who joins later doesn't share a run already going.
	const auto handleRadiantStart = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto start = Protocol::DecodeRadiantStart(a_data);
		if (!start) {
			return;
		}
		start->playerId = a_player.id;  // the run's origin
		broadcast(Protocol::Encode(*start, Protocol::MessageType::kRadiantStarted), true, a_player.key);
	};

	const auto handleRadiantStage = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto stage = Protocol::DecodeRadiantStage(a_data);
		if (!stage) {
			return;
		}
		stage->playerId = a_player.id;
		broadcast(Protocol::Encode(*stage, Protocol::MessageType::kRadiantStage), true, a_player.key);
	};

	const auto handleEquipment = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto equipment = Protocol::DecodeEquipment(a_data);
		if (!equipment) {
			return;
		}
		// The same as they reported last: nothing to tell anyone (a game sending it over and over had everyone else
		// dress its stand-in again each time). A newcomer is sent their last report as they join.
		if (a_player.equipmentReported && a_player.equipment == equipment->items && a_player.equipmentMods == equipment->mods) {
			return;
		}
		equipment->playerId = a_player.id;
		a_player.equipment = equipment->items;
		a_player.equipmentMods = equipment->mods;
		a_player.equipmentReported = true;
		broadcast(Protocol::Encode(*equipment, Protocol::MessageType::kPlayerEquipment), true, a_player.key);
	};

	// Tells the others a player loaded a save (kPlayerReloaded), or owes it until Options::reloadToldEvery after the last.
	const auto tellReloaded = [&](Player& a_player, Clock::time_point a_now) {
		if (a_player.reloadToldAt != Clock::time_point{} && a_now - a_player.reloadToldAt < options.reloadToldEvery) {
			a_player.reloadOwed = true;
			return;
		}
		a_player.reloadOwed = false;
		a_player.reloadToldAt = a_now;
		broadcast(Protocol::Encode(Protocol::PlayerReloaded{ a_player.id }), true, a_player.key);
	};

	const auto handleWorldRequest = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		// Its world was just loaded, and is stale until a world state is sent to it (see handleClaims).
		if (!a_player.welcomed) {
			return;
		}
		a_player.worldAskedAt = Clock::now();
		a_player.worldStale = true;
		// What its game keeps in storage closes to friends until its loaded world lists it again (StorageBook::Stale), and
		// what it does at storage others keep waits for its settle after this world state (handleStorageSettle).
		tellViewsClosed(storage.Stale(a_player.id));
		closeOwnViews(a_player);
		++a_player.worldAsked;
		a_player.storageSettled = !options.rules.sharedStorage;
		// The others' games send their radiant runs going again: the loaded game knows none of them now. At most once in
		// Options::reloadToldEvery for a player (a game asking over and over has every other game resend its runs); one asked for
		// sooner goes when that time is up (a second load at once threw away what the first had resent).
		tellReloaded(a_player, Clock::now());
		// A frame lent to them goes back as it was lent: the loaded game no longer wears it (FrameLoan::AfterLoad).
		endLoans([&](const Protocol::FrameLending& a_loan) { return a_loan.borrowerId == a_player.id; });
		// Not counted against the event budget: its game asks once after a load and never again, so one dropped
		// in a busy second (a scrap-all, a big "take all") left its world without the session's changes and
		// the others' building until it rejoined. The world-state budget holds a game asking too often.
		if (const auto request = Protocol::DecodeWorldRequest(a_data)) {
			giveWorldState(a_player, request->sessionId == sessionId ? request->containerFrom : 0);
			// The copies of what others built went with the world the game threw away: they come with the
			// world state, and wait with it while it is owed.
			if (a_player.worldStateOwed) {
				a_player.workshopOwed = true;
			} else {
				sendWorkshop(a_player);
			}
		}
	};

	const auto welcome = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		const bool again = a_player.welcomed;
		if (a_player.welcomed) {
			// Hello again on the same connection: their game restarted before it timed out
			// (Steam keeps one connection per friend). Start over as a new arrival.
			leave(a_player);
			Player fresh;
			fresh.key = a_player.key;
			fresh.id = a_player.id;
			fresh.budgetKey = a_player.budgetKey;  // saying hello again doesn't refill the budgets
			fresh.connectedAt = Clock::now();
			fresh.rate = a_player.rate;
			fresh.transportId = a_player.transportId;
			fresh.hostRefusedLogged = a_player.hostRefusedLogged;
			fresh.hostRefused = a_player.hostRefused;
			a_player = std::move(fresh);
		}
		handleHello(a_player, a_data, again);
	};

	// --- Host controls: the host's own game (kHostCommand) or the dedicated server's console. a_by says which, in the
	// log ("the host", "the console"). Every line names players by number and bans by alias, never by name.

	const auto nowUnix = [] {
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
	};

	// Drops the player of that number with the reason (and, a_ban, bans them first, with every other connection of theirs).
	const auto removePlayer = [&](std::uint32_t a_id, bool a_ban, std::string_view a_by) {
		using Protocol::HostResult;
		const auto it = std::ranges::find_if(players, [&](const auto& a_entry) { return a_entry.second.welcomed && !a_entry.second.kicked && a_entry.second.id == a_id; });
		if (it == players.end()) {
			return HostResult::kNoSuchPlayer;
		}
		if (it->second.local) {
			return HostResult::kNotTheHost;
		}
		auto& target = it->second;
		auto  result = HostResult::kDone;
		// Removed, not dropped: a locked session doesn't take them back (see handleHello).
		if (target.helloId != 0) {
			removed.insert(target.helloId);
		}
		if (target.transportId != 0) {
			removedAccounts.insert(target.transportId);
		}
		if (!a_ban) {
			log(std::format("server: player {} was kicked by {}", target.id, a_by));
		} else if (const auto entry = bans.Add(target.transportId, target.helloId, nowUnix(), target.name); !entry) {
			log(std::format("server: player {} could not be banned (their game and connection name no account{}); kicked instead", target.id,
				bans.Size() >= BanList::MAX_BANS ? ", or the ban list is full" : ""));
			result = HostResult::kNoIdentity;
		} else {
			log(std::format("server: player {} was banned by {} ({}; {} banned)", target.id, a_by, entry->Alias(), bans.Size()));
			if (!saveBans()) {
				result = HostResult::kNotSaved;
			}
		}
		const auto reason = a_ban && result != HostResult::kNoIdentity ? Protocol::RemovedReason::kBanned : Protocol::RemovedReason::kKicked;
		const auto tell = [&](Player& a_player) {
			send(a_player.key, Protocol::Encode(Protocol::Removed{ reason }), true);
			disconnect(a_player);
		};
		// Their other connections (the same account, still connected another way) go with a ban.
		if (reason == Protocol::RemovedReason::kBanned) {
			for (auto& [key, other] : players) {
				if (key != target.key && !other.local && !other.kicked && other.welcomed && bans.Find(other.transportId, other.helloId)) {
					log(std::format("server: player {} is the same account; dropped with the ban", other.id));
					tell(other);
				}
			}
		}
		tell(target);
		return result;
	};

	const auto unban = [&](std::uint64_t a_id, std::string_view a_by) {
		if (a_id == 0) {
			const auto lifted = bans.Clear();
			log(std::format("server: every ban lifted by {} ({})", a_by, lifted));
		} else {
			const auto alias = BanList::AliasOf(a_id);
			if (!bans.Remove(a_id)) {
				return Protocol::HostResult::kNoSuchBan;
			}
			log(std::format("server: {} unbanned by {} ({} banned)", alias, a_by, bans.Size()));
		}
		return saveBans() ? Protocol::HostResult::kDone : Protocol::HostResult::kNotSaved;
	};

	const auto setLocked = [&](bool a_locked, std::string_view a_by) {
		if (std::exchange(locked, a_locked) != a_locked) {
			log(a_locked ? std::format("server: the session was locked by {}: nobody new can join", a_by) : std::format("server: the session was unlocked by {}", a_by));
		}
		return Protocol::HostResult::kDone;
	};

	// Never the password itself in the log.
	const auto setPassword = [&](std::string_view a_password, std::string_view a_by) {
		password = std::string{ a_password.substr(0, Protocol::MAX_PASSWORD_LENGTH) };
		log(password.empty() ? std::format("server: the password was removed by {}", a_by) : std::format("server: the password was changed by {}", a_by));
		return Protocol::HostResult::kDone;
	};

	const auto hostCommand = [&](const Protocol::HostCommand& a_command, std::string_view a_by) {
		using Protocol::HostAction;
		switch (a_command.action) {
		case HostAction::kKick:
			return removePlayer(a_command.playerId, false, a_by);
		case HostAction::kBan:
			return removePlayer(a_command.playerId, true, a_by);
		case HostAction::kUnban:
			return unban(a_command.ban, a_by);
		case HostAction::kLock:
			return setLocked(true, a_by);
		case HostAction::kUnlock:
			return setLocked(false, a_by);
		case HostAction::kSetPassword:
			return setPassword(a_command.password, a_by);
		}
		return Protocol::HostResult::kNone;
	};

	// A host command or request from someone other than the host: refused, and said in the log (at most once per
	// BUDGET_LOG_INTERVAL a player, with how many came since).
	const auto refuseNotHost = [&](Player& a_player) {
		++a_player.hostRefused;
		if (const auto now = Clock::now(); a_player.hostRefusedLogged == Clock::time_point{} || now - a_player.hostRefusedLogged >= BUDGET_LOG_INTERVAL) {
			log(std::format("server: player {} is not the host: their host command{} refused", a_player.id,
				a_player.hostRefused > 1 ? std::format("s ({} since the last line) were", a_player.hostRefused) : std::string{ " was" }));
			a_player.hostRefusedLogged = now;
			a_player.hostRefused = 0;
		}
	};

	const auto handleHostCommand = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}
		if (!a_player.local) {
			refuseNotHost(a_player);
			return;
		}
		if (const auto command = Protocol::DecodeHostCommand(a_data)) {
			tellHost(hostCommand(*command, "the host"), command->action);
		}
	};

	// The dedicated server's console (Server::Console). The answers go to the log, so they name nobody.
	const auto console = [&](std::string_view a_line) {
		std::vector<std::string_view> words;
		for (std::string_view rest = a_line; !rest.empty();) {
			const auto start = rest.find_first_not_of(" \t\r\n");
			if (start == std::string_view::npos) {
				break;
			}
			rest.remove_prefix(start);
			const auto end = rest.find_first_of(" \t\r\n");
			words.push_back(rest.substr(0, end));
			rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
		}
		if (words.empty()) {
			return;
		}
		const auto command = words[0];
		const auto number = [&](std::uint32_t& a_out) {
			return words.size() == 2 && std::from_chars(words[1].data(), words[1].data() + words[1].size(), a_out).ec == std::errc{};
		};
		const auto said = [&](Protocol::HostResult a_result, std::string_view a_what) {
			switch (a_result) {
			case Protocol::HostResult::kNoSuchPlayer:
				log(std::format("server: console: no player {} in the session (players lists them)", a_what));
				break;
			case Protocol::HostResult::kNotTheHost:
				log("server: console: that is the host's own game");
				break;
			case Protocol::HostResult::kNoSuchBan:
				log(std::format("server: console: no ban {} (bans lists them)", a_what));
				break;
			default:
				break;
			}
		};
		std::uint32_t id = 0;
		if (command == "help") {
			log("server: console: players | kick <n> | ban <n> | bans | unban <n | ~alias | all> | password <text> (- removes it) | lock | unlock");
		} else if (command == "players") {
			log(std::format("server: console: {} in the session{}{}", welcomedCount(), locked ? ", locked" : "", password.empty() ? ", no password" : ", password set"));
			const auto now = Clock::now();
			for (const auto& [key, player] : players) {
				if (player.welcomed && !player.kicked) {
					log(std::format("server: console: player {}{}: {}, here {} min", player.id, player.local ? " (the host)" : "", describe(key),
						std::chrono::duration_cast<std::chrono::minutes>(now - player.connectedAt).count()));
				}
			}
		} else if ((command == "kick" || command == "ban") && number(id)) {
			const auto result = removePlayer(id, command == "ban", "the console");
			said(result, words[1]);
			tellHost(Protocol::HostResult::kNone, command == "ban" ? Protocol::HostAction::kBan : Protocol::HostAction::kKick);
		} else if (command == "bans" || (command == "unban" && words.size() == 1)) {
			log(std::format("server: console: {} banned{}", bans.Size(), bans.Size() ? " (unban <n> lifts one)" : ""));
			for (std::size_t i = 0; i < bans.Entries().size(); ++i) {
				const auto& entry = bans.Entries()[i];
				const std::chrono::year_month_day day{ std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds{ std::chrono::seconds{ entry.bannedAt } }) };
				log(std::format("server: console: ban {}: {}, banned {:04}-{:02}-{:02}", i + 1, entry.Alias(), static_cast<int>(day.year()), static_cast<unsigned>(day.month()),
					static_cast<unsigned>(day.day())));
			}
		} else if (command == "unban" && words.size() == 2) {
			std::uint64_t target = 0;
			if (words[1] == "all") {
				target = 0;
			} else if (const auto entry = bans.FindAlias(words[1])) {
				target = entry->Id();
			} else if (std::uint32_t index = 0; std::from_chars(words[1].data(), words[1].data() + words[1].size(), index).ec == std::errc{} && index >= 1 && index <= bans.Size()) {
				target = bans.Entries()[index - 1].Id();
			} else {
				said(Protocol::HostResult::kNoSuchBan, words[1]);
				return;
			}
			unban(target, "the console");
			tellHost(Protocol::HostResult::kNone, Protocol::HostAction::kUnban);
		} else if (command == "password") {
			if (words.size() == 1) {
				log(password.empty() ? "server: console: no password (password <text> sets one)" : "server: console: a password is set (password - removes it)");
				return;
			}
			// The rest of the line, as typed (spaces and all).
			auto text = a_line.substr(a_line.find("password") + 8);
			text.remove_prefix((std::min)(text.size(), text.find_first_not_of(" \t")));
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
				text.remove_suffix(1);
			}
			setPassword(text == "-" ? std::string_view{} : text, "the console");
			tellHost(Protocol::HostResult::kNone, Protocol::HostAction::kSetPassword);
		} else if ((command == "lock" || command == "unlock") && words.size() == 1) {
			setLocked(command == "lock", "the console");
			tellHost(Protocol::HostResult::kNone, command == "lock" ? Protocol::HostAction::kLock : Protocol::HostAction::kUnlock);
		} else {
			log("server: console: unknown command (help lists them)");
		}
	};

	const auto handlePacket = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		using Protocol::MessageType;
		// On its way out (refused, kicked or banned): nothing more of theirs is taken.
		if (a_player.kicked) {
			return;
		}
		switch (Protocol::PeekType(a_data).value_or(MessageType{})) {
		case MessageType::kHello:
			// Every hello brings a world state and the rest of a welcome. A game saying hello again and again
			// (rejoining in a loop) gets that no more often than its budget has world states: the latest
			// hello waits until it refills (the tick). When the transport can't name them (UDP), the budget is the
			// one kept for the identity their game says hello with, from their first hello on this connection
			// (a new connection doesn't come with a fresh one; saying hello again under another identity
			// doesn't renew it).
			if (a_player.budgetKey.first == BudgetKind::kConnection) {
				if (const auto hello = Protocol::DecodeHello(a_data); hello && hello->identity != 0) {
					bindBudget(a_player, { BudgetKind::kHello, hello->identity });
				}
			}
			if (!a_player.waitingHello.empty() || !worldStateReady(a_player)) {
				if (a_player.welcomed) {
					a_player.worldStale = true;  // its game restarted and has its own save's world now (see handleClaims)
					a_player.worldAskedAt = Clock::now();
					a_player.storageSettled = !options.rules.sharedStorage;  // and settles again once its hello is answered
				}
				if (a_player.waitingHello.empty()) {
					log(std::format("server: {} says hello too often; answering when their budget refills", describe(a_player.key)));
				}
				a_player.waitingHello.assign(a_data.begin(), a_data.end());
				break;
			}
			welcome(a_player, a_data);
			break;
		case MessageType::kPlayerState:
			handleState(a_player, a_data);
			break;
		case MessageType::kReportDeath:
			handleDeath(a_player, a_data);
			break;
		case MessageType::kReportHealth:
			handleHealth(a_player, a_data, MessageType::kActorHealth);
			break;
		case MessageType::kReportDamage:
			handleHealth(a_player, a_data, MessageType::kActorDamaged);
			break;
		case MessageType::kReportContainer:
			handleContainer(a_player, a_data);
			break;
		case MessageType::kReportStorage:
			handleReportStorage(a_player, a_data);
			break;
		case MessageType::kViewStorage:
			handleViewStorage(a_player, a_data);
			break;
		case MessageType::kReportStorageShort:
			handleStorageShort(a_player, a_data);
			break;
		case MessageType::kStorageSettle:
			handleStorageSettle(a_player, a_data);
			break;
		case MessageType::kReportPickup:
			handlePickup(a_player, a_data);
			break;
		case MessageType::kReportRefState:
			handleRefState(a_player, a_data);
			break;
		case MessageType::kEquipment:
			handleEquipment(a_player, a_data);
			break;
		case MessageType::kClaimActors:
			handleClaims(a_player, a_data);
			break;
		case MessageType::kReleaseActors:
			handleRelease(a_player, a_data);
			break;
		case MessageType::kReportNpcEquipment:
			handleNpcEquipment(a_player, a_data);
			break;
		case MessageType::kReportNpcIdentity:
			handleNpcIdentity(a_player, a_data);
			break;
		case MessageType::kReportFace:
			handleFace(a_player, a_data);
			break;
		case MessageType::kReportWorkshopItem:
			handleWorkshopItem(a_player, a_data);
			break;
		case MessageType::kReportDrop:
			handleReportDrop(a_player, a_data);
			break;
		case MessageType::kClaimDrop:
			handleClaimDrop(a_player, a_data);
			break;
		case MessageType::kDropRecord:
			handleDropRecord(a_player, a_data);
			break;
		case MessageType::kReportWorkshopPower:
			handleWorkshopPower(a_player, a_data);
			break;
		case MessageType::kActorStates:
			handleActorStates(a_player, a_data);
			break;
		case MessageType::kPlayerHit:
			handlePlayerHit(a_player, a_data);
			break;
		case MessageType::kReportQuestStage:
			handleQuestStage(a_player, a_data);
			break;
		case MessageType::kReportRadiantStart:
			handleRadiantStart(a_player, a_data);
			break;
		case MessageType::kReportRadiantStage:
			handleRadiantStage(a_player, a_data);
			break;
		case MessageType::kRequestWorldState:
			handleWorldRequest(a_player, a_data);
			break;
		case MessageType::kReportShot:
			handleShot(a_player, a_data);
			break;
		case MessageType::kReportStatus:
			handleStatus(a_player, a_data);
			break;
		case MessageType::kReportTime:
			handleTime(a_player, a_data);
			break;
		case MessageType::kPing:
			handlePing(a_player, a_data);
			break;
		case MessageType::kReportLine:
			handleLine(a_player, a_data);
			break;
		case MessageType::kReportExplosion:
			handleExplosion(a_player, a_data);
			break;
		case MessageType::kReportDestruction:
			handleDestruction(a_player, a_data);
			break;
		case MessageType::kReportTrap:
			handleTrap(a_player, a_data);
			break;
		case MessageType::kReportActivation:
			handleActivation(a_player, a_data);
			break;
		case MessageType::kReportObjectHold:
			handleObjectHold(a_player, a_data);
			break;
		case MessageType::kReportObjectRest:
			handleObjectRest(a_player, a_data);
			break;
		case MessageType::kReportSpawn:
			handleSpawn(a_player, a_data);
			break;
		case MessageType::kBorrowFrame:
			handleBorrow(a_player, a_data);
			break;
		case MessageType::kLendFrame:
			handleLend(a_player, a_data);
			break;
		case MessageType::kReturnFrame:
			handleReturn(a_player, a_data);
			break;
		case MessageType::kRename:
			handleRename(a_player, a_data);
			break;
		case MessageType::kRequestLoadOrder:
			// A player in the session asks to compare their mods with the host's again. Big: once in a while only.
			if (const auto now = Clock::now(); a_player.welcomed && Protocol::DecodeLoadOrderRequest(a_data) &&
			                                   (a_player.loadOrderSent == Clock::time_point{} || now - a_player.loadOrderSent >= LOAD_ORDER_INTERVAL)) {
				sendLoadOrder(a_player, now);
			}
			break;
		case MessageType::kHostCommand:
			handleHostCommand(a_player, a_data);
			break;
		case MessageType::kRequestHostInfo:
			if (!a_player.welcomed || !Protocol::DecodeHostInfoRequest(a_data)) {
				break;
			}
			if (!a_player.local) {
				refuseNotHost(a_player);
				break;
			}
			send(a_player.key, Protocol::Encode(hostInfo(Protocol::HostResult::kNone, Protocol::HostAction::kKick)), true);
			break;
		case MessageType::kReportXp:
			handleXp(a_player, a_data);
			break;
		case MessageType::kReportQuestDone:
			handleQuestDone(a_player, a_data);
			break;
		case MessageType::kRevive:
			handleRevive(a_player, a_data);
			break;
		case MessageType::kVoice:
			handleVoice(a_player, a_data);
			break;
		case MessageType::kReportMarkers:
			handleMarkers(a_player, a_data);
			break;
		case MessageType::kHeartbeat:
			// Own budget: players send one a second, and the answer must not wait behind events.
			if (const auto beat = Protocol::DecodeHeartbeat(a_data); beat && a_player.welcomed) {
				a_player.rate.Roll(Clock::now());
				if (++a_player.rate.heartbeats <= 10) {
					send(a_player.key, Protocol::Encode(*beat, Protocol::MessageType::kHeartbeatAck), true);
				}
			}
			break;
		default:
			break;
		}
	};

	auto                        nextTick = Clock::now();
	std::vector<TransportEvent> events;
	while (!stopRequested) {
		for (std::size_t t = 0; t < transports.size(); ++t) {
			events.clear();
			// Only the first transport waits; the rest are checked without blocking.
			transports[t]->Poll(events, t == 0 ? 2 : 0);
			for (auto& event : events) {
				const auto key = MakeKey(t, event.peer);
				switch (event.type) {
				case TransportEvent::Type::kConnect:
					{
						if (const auto old = players.find(key); old != players.end()) {
							unbindBudget(old->second);
						}
						Player player;
						player.key = key;
						player.id = nextId++;
						player.connectedAt = Clock::now();
						const auto identity = transports[t]->Identity(event.peer);
						player.transportId = identity;
						bindBudget(player, identity ? BudgetKey{ BudgetKind::kTransport, identity } : BudgetKey{ BudgetKind::kConnection, key });
						players.insert_or_assign(key, std::move(player));
					}
					break;
				case TransportEvent::Type::kPacket:
					if (const auto it = players.find(key); it != players.end()) {
						handlePacket(it->second, event.data);
					}
					break;
				case TransportEvent::Type::kDisconnect:
					if (const auto it = players.find(key); it != players.end()) {
						// The host's game going: it may be quitting, so what it did is written now.
						if (it->second.local && it->second.welcomed) {
							saveRequested = true;
						}
						leave(it->second);
						unbindBudget(it->second);
						players.erase(it);
						// The clock belonged to someone who's gone; the next player brings their own.
						if (welcomedCount() == 0) {
							worldTime.reset();
						}
					}
					break;
				}
			}
		}

		const auto now = Clock::now();
		if (now < nextTick) {
			continue;
		}
		nextTick = now + TICK_INTERVAL;
		++tick;
		if (tick % 64 == 0) {
			objectMovers.Prune(now);  // moves over (about once a second)
		}

		for (const auto& line : TakeConsole()) {
			console(line);
		}

		for (auto& [key, player] : players) {
			if (!player.welcomed && !player.kicked && player.waitingHello.empty() && now - player.connectedAt > HELLO_TIMEOUT) {
				log(std::format("server: dropping {} (no hello)", describe(key)));
				disconnect(player);
			}
		}

		// What waits for a player's budget to refill: a hello, the world state, their latest face.
		{
			std::vector<ConnectionKey> waiting;
			for (const auto& [key, player] : players) {
				if (!player.waitingHello.empty()) {
					waiting.push_back(key);
				}
			}
			// (A hello can drop another connection, so each is looked up again.)
			for (const auto key : waiting) {
				if (const auto it = players.find(key); it != players.end() && !it->second.kicked && worldStateReady(it->second)) {
					const auto hello = std::exchange(it->second.waitingHello, {});
					welcome(it->second, hello);
				}
			}
		}
		for (auto& [key, player] : players) {
			if (player.welcomed && player.worldStateOwed && allowWorldState(player)) {
				player.worldStateOwed = false;
				sendWorldState(key, player.owedFrom);
				sendCaughtUp(player);
				player.worldStale = false;
				if (std::exchange(player.workshopOwed, false)) {
					sendWorkshop(player);
				}
				log(std::format("server: sent player {} the world they asked for", player.id));
			}
			if (player.welcomed && player.faceDirty && player.face && now - budgetOf(player).lastFace >= options.faceInterval) {
				player.faceDirty = false;
				budgetOf(player).lastFace = now;
				broadcast(Protocol::Encode(*player.face, Protocol::MessageType::kPlayerFace), true, key);
			}
			flushRename(player, now);
			if (player.welcomed && player.reloadOwed && now - player.reloadToldAt >= options.reloadToldEvery) {
				tellReloaded(player, now);
			}
			if (player.welcomed && player.heldDropRecord && now - player.dropRecordAt >= DROP_RECORD_INTERVAL) {
				const auto record = std::exchange(player.heldDropRecord, std::nullopt);
				answerDropRecord(player, *record, now);
			}
		}

		// Relay changed player states to everyone else.
		for (const auto& [key, receiver] : players) {
			if (!receiver.welcomed) {
				continue;
			}
			Protocol::PlayerStates batch{ tick, {} };
			for (const auto& [otherKey, other] : players) {
				if (otherKey != key && other.welcomed && other.hasState && other.stateDirty) {
					batch.players.push_back({ other.id, other.state });
				}
			}
			if (!batch.players.empty()) {
				send(key, Protocol::Encode(batch), false);
			}
		}
		for (auto& [key, player] : players) {
			player.stateDirty = false;
		}

		// The session goes to disk when it changed (checked every saveInterval), when asked (the host's game
		// saves, or its connection goes), when a save failed and its wait is over, and when the server stops.
		if (saveRequested.exchange(false)) {
			nextSave = now + options.saveInterval;
			saveSession();
		} else if (saveRetry.Due(now)) {
			nextSave = now + options.saveInterval;
			saveSession();
		} else if (now >= nextSave) {
			nextSave = now + options.saveInterval;
			// While a save is failing the retry above decides when to try, not each check.
			if (revision != lastSavedRevision && !saveRetry.Failing()) {
				saveSession();
			}
		}
	}
	// One last try, whatever the wait: the file may be free again by now.
	if (!saveSession(true)) {
		log("server: could not save the session when the server stopped; the last save that worked is what it keeps");
	}

	for (auto& transport : transports) {
		transport->Close();
	}
}
