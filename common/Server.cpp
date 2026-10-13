#include "Server.h"

#include "EnetTransport.h"
#include "Protocol.h"
#include "Redact.h"
#include "SessionFile.h"
#include "TokenBucket.h"
#include "WorkshopStore.h"

#include <array>
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
	constexpr auto  EXPLOSION_AFTER_THROW = std::chrono::seconds(10);  // explosions are reported only after a throw
	constexpr float EXPLOSION_RANGE = 8000.0f;               // from the reporter's last position
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
		float             hitDamage = 0.0f;

		void Roll(Clock::time_point a_now)
		{
			if (a_now - start >= std::chrono::seconds(1)) {
				start = a_now;
				states = actorStates = events = shots = heartbeats = voiceBytes = 0;
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
		kTables
	};
	// For log lines: "the session's {} table", "{} sharing".
	constexpr std::array<std::string_view, kTables> TABLE_NAMES{ "dead NPC", "picked-up item", "door and lock", "quest stage", "container change", "built object" };
	constexpr std::array<std::string_view, kTables> SHARING_NAMES{ "kill", "pickup", "door", "quest", "loot", "building" };

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
		Clock::time_point lastLine{};
		bool              floodLogged = false;
		RateBudget        rates;  // explosions and deaths
		bool              explosionsLogged = false;  // the explosion budget ran out, and the log said so
		bool              deathsLogged = false;
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
		bool                       local = false;  // the host's own game (Options::hostIdentity)
		// NPCs this player hurt or tried to take over, and when last: a kill of one another player's game runs is
		// believed for a while after (Options::deathGrace). At most MAX_FIGHTING.
		std::unordered_map<std::uint32_t, Clock::time_point> fighting;
		Clock::time_point          worldAskedAt{};  // its hello, or its last request for the world (after loading a save)
		// Its last request for the world isn't answered yet (owed while its budget refills, or dropped for flooding):
		// its game may have anything alive from the save it loaded, so it brings nobody back (see handleClaims) until
		// a world state is sent to it.
		bool                       worldStale = false;
	};

	struct Ownership
	{
		std::uint32_t         playerId = 0;
		Protocol::ClaimReason reason = Protocol::ClaimReason::kUnowned;
		Clock::time_point     since{};
	};

	std::string SanitizeName(std::string_view a_name, std::uint32_t a_id)
	{
		std::string out;
		for (const char c : a_name) {
			if (c >= 0x20 && c < 0x7F) {
				out += c;
			}
		}
		if (out.empty()) {
			out = std::format("Player {}", a_id);
		}
		return out;
	}

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
	log(std::format("server: listening on UDP port {}{} (max {} players, {} transports)", options.port,
		options.udpLoopbackOnly ? " (this machine only)" : "", options.maxPlayers, transports.size()));
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
	std::uint64_t                             sessionId = NewSessionId();
	// The password as clients can send it (the wire format caps its length).
	const auto password = options.password.substr(0, Protocol::MAX_PASSWORD_LENGTH);
	// A world state can't carry more of a table than this.
	const auto maxWorldEntries = (std::min)(options.maxWorldEntries, Protocol::MAX_WORLD_STATE_ACTORS);

	// Shared world state for this session.
	std::unordered_set<std::uint32_t>                     deadActors;
	std::vector<Protocol::IndexedContainerChange>         containerChanges;  // index = session index
	std::unordered_set<std::uint32_t>                     pickedUp;
	std::unordered_map<std::uint32_t, Protocol::RefState> refStates;
	std::unordered_map<std::uint32_t, Ownership>          actorOwners;  // which player's game runs each NPC's AI
	std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> npcEquipment;  // each run NPC's gear, from its owner
	WorkshopStore                                         workshop{ MAX_WORKSHOP_ITEMS, MAX_WORLD_SCRAPS };  // built and scrapped at settlements, kept while the players are away too
	bool                                                  builtFullLogged = false;
	bool                                                  scrapsFullLogged = false;
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
			sessionId = saved->sessionId;
			nextId = (std::max)(nextId, saved->nextId);
			for (const auto& [identity, id] : saved->identities) {
				identities[identity] = id;
			}
			deadActors.insert(saved->deadActors.begin(), saved->deadActors.end());
			containerChanges = saved->containerChanges;
			containersAuthored.clear();
			for (const auto& change : containerChanges) {
				++containersAuthored[change.playerId];
			}
			pickedUp.insert(saved->pickedUp.begin(), saved->pickedUp.end());
			for (const auto& state : saved->refStates) {
				refStates[state.refId] = state;
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
			log(std::format("server: loaded the saved session ({} dead, {} loot changes, {} built, {} scrapped)", deadActors.size(), containerChanges.size(), workshop.Built().size(),
				workshop.Scraps().size()));
			noteFill(kDeaths, deadActors.size());
			noteFill(kPickups, pickedUp.size());
			noteFill(kRefStates, refStates.size());
			noteFill(kQuestStages, questStages.size());
			noteFill(kContainers, containerChanges.size());
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
		snapshot.containerChanges = containerChanges;
		snapshot.pickedUp.assign(pickedUp.begin(), pickedUp.end());
		for (const auto& [id, state] : refStates) {
			snapshot.refStates.push_back(state);
		}
		snapshot.questStages = questStages;
		snapshot.worldTime = worldTime;
		for (const auto& [ref, flags] : markers) {
			snapshot.markers.emplace_back(ref, flags);
		}
		workshop.ForEach([&](const WorkshopStore::Key&, const Protocol::WorkshopItem& a_item) { snapshot.workshopItems.push_back(a_item); });
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
		containerChanges.clear();
		containersAuthored.clear();
		deathPositions.clear();
		diedAt.clear();
		pickedUp.clear();
		refStates.clear();
		actorOwners.clear();
		npcEquipment.clear();
		workshop.Clear();
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
			log(std::format("server: '{}' has added {} {} entries this run, their share; no more of theirs are kept", a_player.name, budget.added[a_table], TABLE_NAMES[a_table]));
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
		log(std::format("server: '{}' has asked for the world too often; sending it when their budget refills", a_player.name));
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
			return true;
		});
		sendOwners(released, 0);
	};

	// The player is gone: the others are told, and the NPCs they ran are free for someone else.
	const auto leave = [&](const Player& a_player) {
		if (a_player.welcomed) {
			log(std::format("server: player {} left", a_player.id));
			broadcast(Protocol::Encode(Protocol::PlayerLeft{ a_player.id }), true, a_player.key);
		}
		releaseAll(a_player.id);
	};

	const auto handleHello = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		const auto hello = Protocol::DecodeHello(a_data);
		if (!hello || hello->magic != Protocol::MAGIC) {
			reject(a_player, "Not an F4Multiplayer client");
			return;
		}
		if (hello->version != Protocol::VERSION) {
			reject(a_player, std::format("Version mismatch: server uses protocol {}, you have {}. Install the same mod version.", Protocol::VERSION, hello->version));
			return;
		}
		if (!password.empty() && hello->password != password) {
			log(std::format("server: wrong password from {}", describe(a_player.key)));
			reject(a_player, "Wrong password");
			return;
		}
		// The host's own game: the game this server runs in, by its identity, connecting from this machine.
		// The identity alone isn't enough (it isn't a secret), and nor is the address: a bot or a tunnel to
		// another PC connects from this machine too.
		const bool local = options.hostIdentity != 0 && hello->identity == options.hostIdentity &&
		                   transports[static_cast<std::size_t>(a_player.key >> 32)]->IsLocal(static_cast<PeerId>(a_player.key));
		if (welcomedCount() == 0) {
			// A server in a host's game waits for that game: the first arrival decides the session's load
			// order (and a different one wipes the saved session), so a friend whose Join lands while the
			// host is still at the main menu or loading would decide it for the host.
			if (options.hostIdentity != 0 && !local) {
				reject(a_player, Protocol::REJECT_HOST_NOT_READY);
				return;
			}
			// A different mod list makes the stored form IDs meaningless: start a fresh session. (On a
			// dedicated server, which has no local player, the first arrival decides.)
			if (sessionContentHash != 0 && hello->contentHash != sessionContentHash) {
				log("server: load order changed; starting a new session");
				resetWorld();
			}
			sessionContentHash = hello->contentHash;
			sessionPlugins = hello->plugins;
		} else if (hello->contentHash != sessionContentHash) {
			// A difference only in plugins the host allows to differ (texture packs and the like) is fine.
			if (!Protocol::SameLoadOrderIgnoring(hello->plugins, sessionPlugins, options.ignoredPlugins)) {
				reject(a_player, Protocol::LoadOrderDifference(hello->plugins, sessionPlugins));
				return;
			}
			// Not their name, and they have no player number yet: where they come from (masked) says which
			// joiner it was.
			log(std::format("server: a joiner's load order differs only in ignored plugins ({})", describe(a_player.key)));
		}

		// Someone coming back gets their old ID. If their old connection is still here, their game
		// restarted before it timed out: that one is stale, so it goes.
		bool returning = false;
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
			} else if (identities.size() < MAX_IDENTITIES) {
				identities[hello->identity] = a_player.id;
				touched();
			}
		}
		// (After dropping a stale connection: it doesn't take up a place.)
		if (welcomedCount() >= options.maxPlayers) {
			reject(a_player, "Server is full");
			return;
		}

		a_player.name = SanitizeName(hello->name, a_player.id);
		a_player.appearance = hello->appearance;
		a_player.welcomed = true;
		a_player.local = local;
		a_player.worldAskedAt = Clock::now();
		send(a_player.key, Protocol::Encode(Protocol::Welcome{ a_player.id, sessionId, options.friendlyFire, options.sharedStory }), true);
		giveWorldState(a_player, hello->world.sessionId == sessionId ? hello->world.containerFrom : 0);

		for (auto& [key, other] : players) {
			if (!other.welcomed || key == a_player.key) {
				continue;
			}
			send(a_player.key, Protocol::Encode(Protocol::PlayerJoined{ other.id, other.name, other.appearance }), true);
			send(key, Protocol::Encode(Protocol::PlayerJoined{ a_player.id, a_player.name, a_player.appearance }), true);
			if (!other.equipment.empty()) {
				send(a_player.key, Protocol::Encode(Protocol::Equipment{ other.id, other.equipment }, Protocol::MessageType::kPlayerEquipment), true);
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
		sendWorkshop(a_player);

		// Not to the host's own game: its clock is the session's, and a saved time would turn its clock back to
		// when the session was last saved. (A dedicated server has no such game; it restores the saved time.)
		if (worldTime && !a_player.local) {
			send(a_player.key, Protocol::Encode(*worldTime, Protocol::MessageType::kWorldTime), true);
		}

		log(std::format("server: player {} {} from {} ({} online)", a_player.id, returning ? "came back" : "joined", describe(a_player.key),
			welcomedCount()));
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
	Clock::time_point droppedDeathLogged{};
	std::size_t       droppedDeaths = 0;  // since the last line about them

	const auto handleDeath = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto death = Protocol::DecodeActorDeath(a_data);
		if (!death || !Protocol::IsShareableRef(death->refId) || deadActors.size() >= maxWorldEntries || deadActors.contains(death->refId)) {
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
		// one reliable channel, so even a one-shot kill finds the claim here first.
		if (const auto owner = actorOwners.find(death->refId); owner != actorOwners.end() && owner->second.playerId != a_player.id) {
			const auto now = Clock::now();
			const bool foundDead = a_player.local && death->foundDead && !hostHadAlive.contains(death->refId);
			const bool fought = !death->foundDead && wasFighting(a_player, death->refId, now);
			if (!foundDead && !fought) {
				noteHostHadAlive(a_player, death->refId);  // the host's game had it alive, or saw it die
				++droppedDeaths;
				if (now - droppedDeathLogged >= DROPPED_DEATH_LOG_INTERVAL) {
					droppedDeathLogged = now;
					log(std::format("server: death of {:08X} from '{}' dropped: player {} runs it and the reporter wasn't fighting it ({} dropped since the last such line)",
						death->refId, a_player.name, owner->second.playerId, std::exchange(droppedDeaths, 0)));
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
			if (!spend(a_player, &RateBudget::deaths, Clock::now())) {
				if (!std::exchange(budget.deathsLogged, true)) {
					log(std::format("server: player {} reports deaths faster than any fight; dropping them until their budget refills", a_player.id));
				}
				return;
			}
			budget.deathsLogged = false;
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
		death->playerId = a_player.id;
		broadcast(Protocol::Encode(*death, Protocol::MessageType::kActorDied), true, a_player.key);
	};

	const auto handleHealth = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto health = Protocol::DecodeActorHealth(a_data);
		if (!health || !Protocol::IsShareableRef(health->refId) || deadActors.contains(health->refId)) {
			return;
		}
		noteHostHadAlive(a_player, health->refId);  // hit and still alive
		noteFighting(a_player, health->refId, Clock::now());
		broadcast(Protocol::Encode(*health, Protocol::MessageType::kActorHealth), true, a_player.key);
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
				log(std::format("server: '{}' has made {} container changes this session, their share; no more of theirs are kept", a_player.name, containersAuthored[a_player.id]));
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
		for (const auto& claim : *claims) {
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
			if (deadActors.contains(claim.refId)) {
				const auto died = diedAt.find(claim.refId);
				const bool cameBack = a_player.local && !a_player.worldStale && claim.reason == Reason::kUnowned && now - a_player.worldAskedAt >= options.aliveAgainGrace &&
				                      (died == diedAt.end() || now - died->second >= options.aliveAgainGrace);
				if (!cameBack) {
					continue;
				}
				deadActors.erase(claim.refId);
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
				if (!owned) {
					owned = ownedBy(a_player.id);
					companions = companionsOf(a_player.id);
				}
				grant = actorOwners.size() < Protocol::MAX_WORLD_STATE_ACTORS && *owned < Protocol::MAX_OWNED_PER_PLAYER &&
				        (claim.reason != Reason::kCompanion || *companions < MAX_COMPANIONS_PER_PLAYER);
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

	const auto handleRelease = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto refs = Protocol::DecodeRelease(a_data);
		if (!refs) {
			return;
		}
		std::vector<Protocol::ActorOwner> released;
		for (const auto ref : *refs) {
			if (const auto it = actorOwners.find(ref); it != actorOwners.end() && it->second.playerId == a_player.id) {
				actorOwners.erase(it);
				npcEquipment.erase(ref);  // its next runner reports its gear (NpcSync forgets what it sent on a release)
				released.push_back({ ref, 0 });
			}
		}
		sendOwners(released, 0);
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
		if (!allowEvent(a_player) || now - a_player.lastPing < std::chrono::seconds(1)) {
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

	const auto handleExplosion = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto explosion = Protocol::DecodeExplosion(a_data);
		if (!explosion) {
			return;
		}
		// Only what follows one of the reporter's throws, and near where they are: an explosion report
		// is otherwise a way to set off anything at anyone. A player who hasn't said where they are yet
		// is near nothing. (Each game also judges the explosion itself: WeaponFire's ApplyExplosion.)
		const auto now = Clock::now();
		if (now - budgetOf(a_player).lastThrow > EXPLOSION_AFTER_THROW || !a_player.hasState) {
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
				if (!std::exchange(budgetOf(a_player).explosionsLogged, true)) {
					log(std::format("server: player {} reports explosions faster than anyone throws; dropping them until their budget refills", a_player.id));
				}
				return;
			}
			budgetOf(a_player).explosionsLogged = false;
		}
		explosion->playerId = a_player.id;
		broadcast(Protocol::Encode(*explosion, Protocol::MessageType::kExplosion), false, a_player.key);
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
		const auto stage = Protocol::DecodeQuestStage(a_data);
		if (!stage || !Protocol::IsShareableRef(stage->quest) || questStages.size() >= maxWorldEntries ||
			questStageKeys.contains(questKey(*stage)) || !withinShare(a_player, kQuestStages)) {
			return;
		}
		questStages.push_back(*stage);
		questStageKeys.insert(questKey(*stage));
		++budgetOf(a_player).added[kQuestStages];
		noteFill(kQuestStages, questStages.size());
		touched();
		broadcast(Protocol::Encode(*stage, Protocol::MessageType::kQuestStage), true, a_player.key);
	};

	const auto handleEquipment = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto equipment = Protocol::DecodeEquipment(a_data);
		if (!equipment) {
			return;
		}
		equipment->playerId = a_player.id;
		a_player.equipment = equipment->items;
		broadcast(Protocol::Encode(*equipment, Protocol::MessageType::kPlayerEquipment), true, a_player.key);
	};

	const auto handleWorldRequest = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		// Its world was just loaded, and is stale until a world state is sent to it (see handleClaims).
		if (a_player.welcomed) {
			a_player.worldAskedAt = Clock::now();
			a_player.worldStale = true;
		}
		if (!allowEvent(a_player)) {
			return;
		}
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
			a_player = std::move(fresh);
		}
		handleHello(a_player, a_data);
	};

	const auto handlePacket = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		using Protocol::MessageType;
		switch (Protocol::PeekType(a_data).value_or(MessageType{})) {
		case MessageType::kHello:
			if (a_player.kicked) {
				break;
			}
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
			handleHealth(a_player, a_data);
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
		case MessageType::kReportFace:
			handleFace(a_player, a_data);
			break;
		case MessageType::kReportWorkshopItem:
			handleWorkshopItem(a_player, a_data);
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
		case MessageType::kReportActivation:
			handleActivation(a_player, a_data);
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
				log(std::format("server: sent '{}' the world they asked for", player.name));
			}
			if (player.welcomed && player.faceDirty && player.face && now - budgetOf(player).lastFace >= options.faceInterval) {
				player.faceDirty = false;
				budgetOf(player).lastFace = now;
				broadcast(Protocol::Encode(*player.face, Protocol::MessageType::kPlayerFace), true, key);
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
