#include "Server.h"

#include "BanList.h"
#include "DropStore.h"
#include "EnetTransport.h"
#include "FormIds.h"
#include "Name.h"
#include "ObjectMovers.h"
#include "Protocol.h"
#include "Redact.h"
#include "SessionFile.h"
#include "TokenBucket.h"
#include "WorkshopStore.h"

#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <random>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
	using Clock = std::chrono::steady_clock;

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
	// The others are told a player loaded a save (kPlayerReloaded) at most this often: each time, every game sends its
	// radiant runs going again. A load takes longer than this.
	constexpr auto RELOAD_TOLD_EVERY = std::chrono::seconds(2);
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
	// Explosions are reported only after a throw, or a shot (at a mine, a car: WeaponFire arms its watch on a hit).
	constexpr auto  EXPLOSION_AFTER_ATTACK = std::chrono::seconds(10);
	// A destructible object a player damaged goes off in the others' games only for this long after their last shot or
	// throw: a car set on fire burns for some 10 s before its big explosion, longer if it was shot only lightly.
	constexpr auto  DESTRUCTION_EXPLODES_AFTER_ATTACK = std::chrono::seconds(30);
	// And for this long after such a report of it, from whoever reports it next: each game's copy of a burning car burns
	// on its own, so the stage the attacker set going can be passed first in a friend's game, who never shot.
	constexpr auto  DESTRUCTION_ARMED_FOR = std::chrono::seconds(30);
	constexpr float EXPLOSION_RANGE = 8000.0f;               // from the reporter's last position
	// A trap is reported by a game that has it loaded: it lies within the loaded cells around the reporter (a mine can be
	// shot from the far side of them), never across the map.
	constexpr float TRAP_RANGE = 16000.0f;
	// Explosions one player may report: a few grenades at once, then four a second, however many throws they
	// report (each throw opens a window; a burst of throws would otherwise buy a burst of explosions).
	constexpr double EXPLOSION_BURST = 16.0;
	constexpr double EXPLOSIONS_PER_SECOND = 4.0;
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
	// A player who hits or talks to an NPC another player runs takes it over, at most this often.
	constexpr auto TAKEOVER_COOLDOWN = std::chrono::seconds(5);
	constexpr std::size_t MAX_FIGHTING = 256;  // NPCs one player is remembered fighting (a big fight is a dozen)
	constexpr auto DROPPED_DEATH_LOG_INTERVAL = std::chrono::seconds(10);
	// A flooder's budget refills and runs out again twice a second: the log says so at most this often.
	constexpr auto BUDGET_LOG_INTERVAL = std::chrono::seconds(10);
	// NPCs games made at runtime and share (Protocol::IsSpawnRef): each game's share at once is
	// Protocol::MAX_SPAWNS_PER_PLAYER, the session's this. A game may share a fight's worth at once, then two a second
	// (each one is a copy placed in every other game).
	constexpr std::size_t MAX_SPAWNS = 128;
	constexpr double      SPAWN_BURST = 32.0;
	constexpr double      SPAWNS_PER_SECOND = 2.0;

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
		kTables
	};
	// For log lines: "the session's {} table", "{} sharing".
	constexpr std::array<std::string_view, kTables> TABLE_NAMES{ "dead NPC", "picked-up item", "door and lock", "quest stage", "container change", "built object", "world trap", "destroyed object" };
	constexpr std::array<std::string_view, kTables> SHARING_NAMES{ "kill", "pickup", "door", "quest", "loot", "building", "trap", "destruction" };

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

		RateBudget() :
			RateBudget(1.0)
		{}
		explicit RateBudget(double a_share) :
			explosions(a_share * EXPLOSION_BURST, a_share * EXPLOSIONS_PER_SECOND), deaths(a_share * DEATH_BURST, a_share * DEATHS_PER_SECOND)
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
		Clock::time_point lastThrow{};  // explosions are believed only for a while after one
		Clock::time_point lastShot{};   // any shot, theirs or an NPC's they run: the same, for what it hit
		Clock::time_point lastLine{};
		bool              floodLogged = false;
		RateBudget        rates;  // explosions and deaths
		Clock::time_point explosionsLogged{};  // when the log last said the explosion budget ran out
		std::size_t       explosionsDropped = 0;  // dropped since then
		Clock::time_point deathsLogged{};
		std::size_t       deathsDropped = 0;
		TokenBucket       spawns{ SPAWN_BURST, SPAWNS_PER_SECOND };  // runtime NPCs shared (the host's own game too)
		TokenBucket       objectRests{ ObjectMovers::REST_BURST, ObjectMovers::RESTS_PER_SECOND };  // objects let go of or come to rest
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
		Clock::time_point          worldAskedAt{};  // its hello, or its last request for the world (after loading a save)
		Clock::time_point          reloadToldAt{};  // when the others were last told it loaded a save (kPlayerReloaded)
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
	// The objects one of those reports came for lately (DESTRUCTION_ARMED_FOR): their explosions are let through from
	// anyone. Not saved: over in seconds.
	std::unordered_map<std::uint32_t, Clock::time_point>    destructionsArmed;
	std::unordered_map<std::uint32_t, Ownership>          actorOwners;  // which player's game runs each NPC's AI
	ObjectMovers::Book                                    objectMovers;  // who moves each loose object right now (not saved: a move is over in seconds)
	// NPCs games made at runtime and share, by spawn id (Protocol::IsSpawnRef): each is run by its owner (in actorOwners
	// too) until it dies, its owner's game lets it go, or its owner leaves. Not saved: it is in no save but its owner's.
	std::unordered_map<std::uint32_t, Protocol::Spawn>    spawns;
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
	bool                                                  aheadLogged = false;  // a player counting more container changes than we have
	std::vector<Protocol::QuestStage>                     questStages;  // in order, no repeats
	std::unordered_set<std::uint64_t>                     questStageKeys;
	std::unordered_map<std::uint32_t, std::uint32_t>      containersAuthored;  // player id -> changes they made this session
	// What all joiners together may report, on top of each one's own (Budget::rates).
	RateBudget                                            joinersBudget{ JOINERS_SHARE };
	// Where an NPC fell, by the report that killed it; a joiner gets these before the dead list so its copy
	// lies where the killer saw it. Not saved: after a restart the list alone is what there is.
	std::unordered_map<std::uint32_t, std::array<float, 3>> deathPositions;
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
			for (const auto& destruction : saved->destructions) {
				destructions[destruction.refId] = destruction;
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
	bool          saveFailing = false;  // said once, not at every check, until a save goes through
	const auto    touched = [&] { ++revision; };
	const auto    saveSession = [&] {
		if (options.sessionFile.empty() || sessionContentHash == 0) {
			return;
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
		for (const auto& [id, destruction] : destructions) {
			snapshot.destructions.push_back(destruction);
		}
		snapshot.plugins = sessionTable;
		const auto saved = revision;
		if (!SessionFile::Save(sessionPath, snapshot)) {
			if (!std::exchange(saveFailing, true)) {
				log(std::format("server: could not save the session to {}; trying again at every check", sessionPath.filename().string()));
			}
			return;  // tried again at the next check
		}
		if (std::exchange(saveFailing, false)) {
			log("server: the session is saved again");
		}
		lastSavedRevision = saved;
	};
	Clock::time_point nextSave = Clock::now() + options.saveInterval;

	const auto resetWorld = [&] {
		deadActors.clear();
		healable.clear();
		containerChanges.clear();
		containersAuthored.clear();
		deathPositions.clear();
		diedAt.clear();
		pickedUp.clear();
		refStates.clear();
		traps.clear();
		actorOwners.clear();
		destructions.clear();
		destructionsArmed.clear();
		spawns.clear();
		npcEquipment.clear();
		npcIdentities.clear();
		workshop.Clear();
		drops.Clear();
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
		const auto share = a_table == kContainers ? options.maxContainerChangesPerPlayer :
		                   a_table == kWorkshop   ? options.maxWorkshopItemsPerPlayer :
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
	};

	// A player gets the world state they need (on joining, after loading a save) now if their budget
	// allows, else when it refills (the tick sends it): owed, never dropped, or their world would miss
	// what happened. Asking again while it is owed only moves where it starts.
	const auto giveWorldState = [&](Player& a_player, std::uint32_t a_containerFrom) {
		if (a_player.worldStateOwed) {
			a_player.owedFrom = (std::min)(a_player.owedFrom, a_containerFrom);
			return;
		}
		if (allowWorldState(a_player)) {
			sendWorldState(a_player.key, a_containerFrom);
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
		// The rolls they gave NPCs someone else runs now: that runner's report is the session's from here on.
		std::erase_if(npcIdentities, [&](const auto& a_entry) { return a_entry.second.playerId == a_playerId; });
		sendOwners(released, 0);
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
		// Offers they never confirmed go: their game may still have those items (common/DropStore.h).
		drops.DropOffers(a_player.id);
		objectMovers.Leave(a_player.id);
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
		a_player.dropListAt.reset();
		a_player.helloId = hello->identity;
		a_player.worldAskedAt = Clock::now();
		send(a_player.key, Protocol::Encode(Protocol::Welcome{ a_player.id, sessionId, options.friendlyFire, options.sharedStory, options.rules.Clamped() }), true);
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
		if (!health || (!spawn && !Protocol::IsShareableRef(health->refId)) || deadActors.contains(health->refId)) {
			return;
		}
		if (a_out == Protocol::MessageType::kActorDamaged && !(health->health > 0.0f && health->health <= 1.0f)) {
			return;  // no damage, or more than all of it
		}
		if (!spawn) {
			noteHostHadAlive(a_player, health->refId);  // hit and still alive
		}
		noteFighting(a_player, health->refId, Clock::now());
		broadcast(Protocol::Encode(*health, a_out), true, a_player.key);
	};

	// Container changes are numbered and echoed to their sender too, so every player knows exactly
	// which of them its world contains (see Protocol::WorldRequest).
	const auto handleContainer = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto change = Protocol::DecodeContainerChange(a_data);
		if (!change || !Protocol::IsValid(*change) || containerChanges.size() >= maxWorldEntries) {
			return;
		}
		// Their share of the session's table, by player ID (saved with the session) and by budget (this run).
		// The host's own game has none (see withinShare).
		if (!a_player.local && containersAuthored[a_player.id] >= options.maxContainerChangesPerPlayer) {
			if (!std::exchange(budgetOf(a_player).shareLogged[kContainers], true)) {
				log(std::format("server: player {} has made {} container changes this session, their share; no more of theirs are kept", a_player.id, containersAuthored[a_player.id]));
			}
			return;
		}
		if (!withinShare(a_player, kContainers)) {
			return;
		}
		++containersAuthored[a_player.id];
		++budgetOf(a_player).added[kContainers];
		const auto index = static_cast<std::uint32_t>(containerChanges.size());
		containerChanges.push_back({ index, a_player.id, *change });
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
	// - a player fighting or talking to it takes it over, unless it is someone's companion or
	//   changed hands moments ago (two players fighting it would otherwise trade it constantly).
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
				diedAt.erase(claim.refId);
				touched();
				log(std::format("server: {:08X}, dead in the saved session, is alive in the host's world and can't die there (essential); no longer dead in the session", claim.refId));
			}
			if (deadActors.contains(claim.refId)) {
				const auto died = diedAt.find(claim.refId);
				const bool cameBack = a_player.local && !a_player.worldStale && claim.reason == Reason::kUnowned && now - a_player.worldAskedAt >= options.aliveAgainGrace &&
				                      (died == diedAt.end() || now - died->second >= options.aliveAgainGrace);
				if (!cameBack) {
					continue;
				}
				deadActors.erase(claim.refId);
				healable.erase(claim.refId);
				deathPositions.erase(claim.refId);
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
				// The owner talking to it: nobody takes it over mid-conversation.
				if (claim.reason == Reason::kInteract) {
					it->second.since = now;
				}
				continue;
			} else if (claim.reason == Reason::kCompanion) {
				grant = it->second.reason != Reason::kCompanion;
			} else if (claim.reason == Reason::kInteract) {
				grant = it->second.reason != Reason::kCompanion && now - it->second.since >= TAKEOVER_COOLDOWN;
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
		const auto added = drops.Add(a_player.id, *item);
		switch (added.outcome) {
		case DropStore::Outcome::kAdded:
			send(a_player.key, Protocol::Encode(*added.drop, Protocol::MessageType::kDropPlaced), true);  // the offer
			break;
		case DropStore::Outcome::kRepeat:
			if (added.drop) {
				send(a_player.key, Protocol::Encode(*added.drop, Protocol::MessageType::kDropPlaced), true);
			}
			break;
		case DropStore::Outcome::kFull:
		case DropStore::Outcome::kOwnerFull:
			{
				// Not kept: their game keeps the item it dropped as it is (an ordinary item on its ground). Said by a
				// kDropPlaced with no drop ID.
				auto refused = *item;
				refused.dropId = 0;
				refused.ownerId = a_player.id;
				refused.takerId = 0;
				send(a_player.key, Protocol::Encode(refused, Protocol::MessageType::kDropPlaced), true);
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
		// reporter's game (dead in the host's save, alive in a joiner's until the news reaches it).
		if (!hit->byPlayer && deadActors.contains(hit->attacker)) {
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
		if (shot->action == Protocol::ShotAction::kShot) {
			budgetOf(a_player).lastShot = Clock::now();  // not a reload or an aim: nothing goes off of those
		}
		if (shot->action == static_cast<std::uint8_t>(Protocol::ShotAction::kThrow)) {
			budgetOf(a_player).lastThrow = Clock::now();
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
	// moves it now (ObjectMovers). A joiner's budget (kept across rejoining) bounds a flood of made-up ones.
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
		// Only what follows one of the reporter's throws or shots (a mine or a car they shot goes off), and near where they
		// are: an explosion report is otherwise a way to set off anything at anyone. A player who hasn't said where they are
		// yet is near nothing. (Each game also judges the explosion itself: WeaponFire's ApplyExplosion.)
		const auto now = Clock::now();
		const auto& attacks = budgetOf(a_player);
		if (now - (std::max)(attacks.lastThrow, attacks.lastShot) > EXPLOSION_AFTER_ATTACK || !a_player.hasState) {
			return;
		}
		const float dx = explosion->x - a_player.state.x;
		const float dy = explosion->y - a_player.state.y;
		const float dz = explosion->z - a_player.state.z;
		if (!(dx * dx + dy * dy + dz * dz <= EXPLOSION_RANGE * EXPLOSION_RANGE)) {
			return;
		}
		if (!a_player.local) {
			if (!spend(a_player, &RateBudget::explosions, now)) {
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
		broadcast(Protocol::Encode(*explosion, Protocol::MessageType::kExplosion), false, a_player.key);
	};

	// Destructible objects the player's game damaged past a stage (a car they shot, a barrel their grenade blew). The
	// session keeps each one's lowest health and destroyed for good; only news is relayed (one a friend's report already
	// took further is dropped), so a wreck is never mended and the same stage never comes twice. Each new object takes a
	// place in the table, and from the player's share. Its explosion is let through only after one of the player's shots
	// or throws, and within their explosion budget (as kReportExplosion): else the others get the wreck alone. Such a
	// report arms its object for a while, so that the next stage's explosion goes through from whoever passes it first
	// (a friend's copy, burning on its own, can get there before the attacker's).
	const auto handleDestruction = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto report = Protocol::DecodeDestructions(a_data);
		if (!report) {
			return;
		}
		const auto  now = Clock::now();
		const auto& attacks = budgetOf(a_player);
		const bool  attacked = now - (std::max)(attacks.lastThrow, attacks.lastShot) <= DESTRUCTION_EXPLODES_AFTER_ATTACK;
		if (destructionsArmed.size() > 4096) {
			std::erase_if(destructionsArmed, [&](const auto& a_entry) { return now >= a_entry.second; });
		}
		Protocol::Destructions relayed{ a_player.id, {} };
		for (const auto& entry : report->entries) {
			const auto armed = destructionsArmed.find(entry.refId);
			const bool mayExplode = attacked || (armed != destructionsArmed.end() && now < armed->second);
			if (attacked) {
				destructionsArmed[entry.refId] = now + DESTRUCTION_ARMED_FOR;
			}
			auto it = destructions.find(entry.refId);
			if (it == destructions.end()) {
				if (destructions.size() >= maxWorldEntries || !withinShare(a_player, kDestructions)) {
					continue;
				}
				++budgetOf(a_player).added[kDestructions];
				noteFill(kDestructions, destructions.size() + 1);
				it = destructions.emplace(entry.refId, Protocol::Destruction{ entry.refId, entry.health, entry.destroyed, false }).first;
			} else {
				auto& stored = it->second;
				const bool lower = entry.health < stored.health;
				const bool newlyDestroyed = entry.destroyed && !stored.destroyed;
				if (!lower && !newlyDestroyed) {
					continue;
				}
				stored.health = (std::min)(stored.health, entry.health);
				stored.destroyed = stored.destroyed || entry.destroyed;
			}
			touched();
			auto out = it->second;
			if (entry.explode && mayExplode) {
				out.explode = a_player.local || spend(a_player, &RateBudget::explosions, now);
				if (!out.explode) {
					auto& budget = budgetOf(a_player);
					++budget.explosionsDropped;
					if (now - budget.explosionsLogged >= BUDGET_LOG_INTERVAL) {
						budget.explosionsLogged = now;
						log(std::format("server: player {} sets off destructible objects faster than anyone throws; the others get them wrecked, not going off, until their budget refills ({} since the last such line)",
							a_player.id, std::exchange(budget.explosionsDropped, 0)));
					}
				}
			}
			relayed.entries.push_back(out);
		}
		if (!relayed.entries.empty()) {
			broadcast(Protocol::Encode(relayed, Protocol::MessageType::kDestructions), true, a_player.key);
		}
	};

	// A world trap spent in the reporter's game (TrapState): kept, first come first kept (a take still replaces a spring or a
	// disarm), and passed on to everyone else live, so each game whose copy is still armed sets it off there. Only one near
	// where the reporter is: a report is otherwise a way to set off any mine at anyone. (Each game judges it again:
	// TrapRules::Live, from where the trap and the reporter's stand-in are there.)
	const auto handleTrap = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto trap = Protocol::DecodeTrapState(a_data);
		if (!trap || !a_player.hasState) {
			return;
		}
		const float dx = trap->x - a_player.state.x;
		const float dy = trap->y - a_player.state.y;
		const float dz = trap->z - a_player.state.z;
		if (!(dx * dx + dy * dy + dz * dz <= TRAP_RANGE * TRAP_RANGE)) {
			return;
		}
		const auto known = traps.find(trap->refId);
		if (!Protocol::TrapSupersedes(trap->state, known != traps.end() ? std::optional{ known->second.state } : std::nullopt)) {
			return;
		}
		if (known == traps.end()) {
			if (traps.size() >= maxWorldEntries || !withinShare(a_player, kTraps)) {
				return;
			}
			++budgetOf(a_player).added[kTraps];
			noteFill(kTraps, traps.size() + 1);
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

	const auto handleWorldRequest = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		// Its world was just loaded, and is stale until a world state is sent to it (see handleClaims).
		if (!a_player.welcomed) {
			return;
		}
		a_player.worldAskedAt = Clock::now();
		a_player.worldStale = true;
		// The others' games send their radiant runs going again: the loaded game knows none of them now. At most once in
		// RELOAD_TOLD_EVERY for a player (a game asking over and over has every other game resend its runs).
		if (const auto now = Clock::now(); a_player.reloadToldAt == Clock::time_point{} || now - a_player.reloadToldAt >= RELOAD_TOLD_EVERY) {
			a_player.reloadToldAt = now;
			broadcast(Protocol::Encode(Protocol::PlayerReloaded{ a_player.id }), true, a_player.key);
		}
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
		// saves, or its connection goes) and when the server stops.
		if (saveRequested.exchange(false)) {
			nextSave = Clock::now() + options.saveInterval;
			saveSession();
		} else if (Clock::now() >= nextSave) {
			nextSave = Clock::now() + options.saveInterval;
			if (revision != lastSavedRevision) {
				saveSession();
			}
		}
	}
	saveSession();

	for (auto& transport : transports) {
		transport->Close();
	}
}
