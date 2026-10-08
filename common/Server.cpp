#include "Server.h"

#include "EnetTransport.h"
#include "Protocol.h"
#include "SessionFile.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <random>
#include <unordered_map>
#include <unordered_set>

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
	constexpr int  MAX_WORLD_STATES_PER_SECOND = 4;
	// Hello identities remembered per server run; a client can make up new ones, so there's a cap.
	constexpr std::size_t MAX_IDENTITIES = 1024;
	constexpr std::size_t MAX_WORKSHOP_ITEMS = 20000;  // built objects remembered for joiners (a big settlement is a few thousand)
	// Shared XP: at most MAX_XP_SHARE per XP_REFILL_TIME, in at most XP_REPORT_BURST reports
	// (one more each XP_REPORT_INTERVAL).
	constexpr auto XP_REFILL_TIME = std::chrono::seconds(10);
	constexpr int  XP_REPORT_BURST = 10;
	constexpr auto XP_REPORT_INTERVAL = std::chrono::milliseconds(500);
	// Connections that haven't said Hello by then are dropped (they hold a slot).
	constexpr auto HELLO_TIMEOUT = std::chrono::seconds(5);
	// A player who hits or talks to an NPC another player runs takes it over, at most this often.
	constexpr auto TAKEOVER_COOLDOWN = std::chrono::seconds(5);

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
		int               worldStates = 0;

		void Roll(Clock::time_point a_now)
		{
			if (a_now - start >= std::chrono::seconds(1)) {
				start = a_now;
				states = actorStates = events = shots = heartbeats = voiceBytes = worldStates = 0;
			}
		}
	};

	struct Player
	{
		ConnectionKey              key = 0;
		std::uint32_t              id = 0;
		Clock::time_point          connectedAt{};
		std::string                name;
		std::uint32_t              appearance = 0;
		std::vector<std::uint32_t> equipment;
		std::optional<Protocol::Face> face;
		bool                       welcomed = false;
		bool                       kicked = false;
		bool                       hasState = false;
		bool                       stateDirty = false;
		Protocol::PlayerState      state;
		RateWindow                 rate;
		bool                       floodLogged = false;
		std::optional<Protocol::PlayerStatus> status;
		Clock::time_point          lastPing{};
		Clock::time_point          lastLine{};
		bool                       local = false;  // on the server's machine
		float                      xpBudget = Protocol::MAX_XP_SHARE;
		float                      xpReports = XP_REPORT_BURST;
		Clock::time_point          xpRefilled{};
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

Server::Server(LogFn a_log) :
	log(std::move(a_log))
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
	std::uint32_t                             tick = 0;
	std::uint32_t                             sessionContentHash = 0;  // set by the first player
	std::vector<std::string>                  sessionPlugins;          // their load order, to name the difference
	std::uint64_t                             sessionId = NewSessionId();
	// The password as clients can send it (the wire format caps its length).
	const auto password = options.password.substr(0, Protocol::MAX_PASSWORD_LENGTH);

	// Shared world state for this session.
	std::unordered_set<std::uint32_t>                     deadActors;
	std::vector<Protocol::IndexedContainerChange>         containerChanges;  // index = session index
	std::unordered_set<std::uint32_t>                     pickedUp;
	std::unordered_map<std::uint32_t, Protocol::RefState> refStates;
	std::unordered_map<std::uint32_t, Ownership>          actorOwners;  // which player's game runs each NPC's AI
	std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> npcEquipment;  // each run NPC's gear, from its owner
	std::map<std::pair<std::uint32_t, std::uint32_t>, Protocol::WorkshopItem> workshopItems;  // (player, ref) -> what they built, kept while they're away too
	std::vector<Protocol::QuestStage>                     questStages;  // in order, no repeats
	std::optional<Protocol::WorldTime>                    worldTime;    // from the first player
	std::map<std::uint32_t, std::uint8_t>                 markers;      // discovered map markers (flags)

	// The saved session, if any: adopted as this run's session (the first player's load order must
	// match, or the hello path starts a fresh one). Saved again every few seconds while it changes.
	const std::filesystem::path sessionPath{ options.sessionFile };
	if (!options.sessionFile.empty()) {
		if (const auto saved = SessionFile::Load(sessionPath)) {
			sessionContentHash = saved->contentHash;
			sessionId = saved->sessionId;
			nextId = (std::max)(nextId, saved->nextId);
			for (const auto& [identity, id] : saved->identities) {
				identities[identity] = id;
			}
			deadActors.insert(saved->deadActors.begin(), saved->deadActors.end());
			containerChanges = saved->containerChanges;
			pickedUp.insert(saved->pickedUp.begin(), saved->pickedUp.end());
			for (const auto& state : saved->refStates) {
				refStates[state.refId] = state;
			}
			questStages = saved->questStages;
			worldTime = saved->worldTime;
			for (const auto& [ref, flags] : saved->markers) {
				markers[ref] = flags;
			}
			for (const auto& item : saved->workshopItems) {
				workshopItems[{ item.playerId, item.refId }] = item;
			}
			log(std::format("server: loaded the saved session ({} dead, {} loot changes, {} built)", deadActors.size(), containerChanges.size(), workshopItems.size()));
		}
	}
	std::size_t lastSavedSize = 0;  // a cheap change check: how much state there was at the last save
	const auto  stateSize = [&] { return deadActors.size() + containerChanges.size() + pickedUp.size() + refStates.size() + questStages.size() + markers.size() + workshopItems.size() + (worldTime ? 1 : 0); };
	const auto  saveSession = [&] {
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
		for (const auto& [key, item] : workshopItems) {
			snapshot.workshopItems.push_back(item);
		}
		if (!SessionFile::Save(sessionPath, snapshot)) {
			log("server: could not save the session");
		}
		lastSavedSize = stateSize();
	};
	Clock::time_point nextSave = Clock::now() + std::chrono::seconds(15);

	const auto resetWorld = [&] {
		deadActors.clear();
		containerChanges.clear();
		pickedUp.clear();
		refStates.clear();
		actorOwners.clear();
		npcEquipment.clear();
		workshopItems.clear();
		questStages.clear();
		worldTime.reset();
		markers.clear();
		sessionId = NewSessionId();
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
		if (!a_player.floodLogged) {
			a_player.floodLogged = true;
			log(std::format("server: '{}' sends too many events; dropping some", a_player.name));
		}
		return false;
	};

	// Counts a world state the player asked for (hello again or kRequestWorldState) against their budget.
	const auto allowWorldState = [&](Player& a_player) {
		a_player.rate.Roll(Clock::now());
		return ++a_player.rate.worldStates <= MAX_WORLD_STATES_PER_SECOND;
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
		const std::vector<std::uint32_t> dead(deadActors.begin(), deadActors.end());
		const std::vector<std::uint32_t> picked(pickedUp.begin(), pickedUp.end());
		std::vector<Protocol::RefState>  states;
		for (const auto& [id, state] : refStates) {
			states.push_back(state);
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

	const auto releaseAll = [&](std::uint32_t a_playerId) {
		std::vector<Protocol::ActorOwner> released;
		std::erase_if(actorOwners, [&](const auto& a_entry) {
			if (a_entry.second.playerId != a_playerId) {
				return false;
			}
			released.push_back({ a_entry.first, 0 });
			return true;
		});
		sendOwners(released, 0);
	};

	// The player is gone: the others are told, and the NPCs they ran are free for someone else.
	const auto leave = [&](const Player& a_player) {
		if (a_player.welcomed) {
			broadcast(Protocol::Encode(Protocol::PlayerLeft{ a_player.id }), true, a_player.key);
			log(std::format("server: '{}' left", a_player.name));
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
		if (welcomedCount() == 0) {
			// A different mod list makes the stored form IDs meaningless: start a fresh session.
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
			log(std::format("server: '{}' joins with a load order that differs only in ignored plugins", hello->name));
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
					log(std::format("server: dropping the old connection of '{}' ({})", stale->second.name, describe(stale->first)));
					leave(stale->second);
					disconnect(stale->second);
					players.erase(stale);
				}
				a_player.id = known->second;
				returning = true;
			} else if (identities.size() < MAX_IDENTITIES) {
				identities[hello->identity] = a_player.id;
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
		a_player.local = transports[static_cast<std::size_t>(a_player.key >> 32)]->IsLocal(static_cast<PeerId>(a_player.key));
		a_player.xpRefilled = Clock::now();
		send(a_player.key, Protocol::Encode(Protocol::Welcome{ a_player.id, sessionId, options.friendlyFire, options.sharedStory }), true);
		sendWorldState(a_player.key, hello->world.sessionId == sessionId ? hello->world.containerFrom : 0);

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
		// Everything built at settlements so far, by anyone (their builder may be gone), in batches.
		{
			std::vector<Protocol::WorkshopItem> batch;
			for (const auto& [key, item] : workshopItems) {
				if (key.first == a_player.id) {
					continue;
				}
				batch.push_back(item);
				if (batch.size() == Protocol::MAX_ACTORS_PER_PACKET) {
					send(a_player.key, Protocol::Encode(batch), true);
					batch.clear();
				}
			}
			if (!batch.empty()) {
				send(a_player.key, Protocol::Encode(batch), true);
			}
		}

		if (worldTime) {
			send(a_player.key, Protocol::Encode(*worldTime, Protocol::MessageType::kWorldTime), true);
		}

		log(std::format("server: '{}' {} as player {} from {} ({} online)", a_player.name, returning ? "came back" : "joined", a_player.id, describe(a_player.key),
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

	const auto handleDeath = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		auto death = Protocol::DecodeActorDeath(a_data);
		if (!death || !Protocol::IsShareableRef(death->refId) || deadActors.size() >= Protocol::MAX_WORLD_STATE_ACTORS ||
			!deadActors.insert(death->refId).second) {
			return;
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
		broadcast(Protocol::Encode(*health, Protocol::MessageType::kActorHealth), true, a_player.key);
	};

	// Container changes are numbered and echoed to their sender too, so every player knows exactly
	// which of them its world contains (see Protocol::WorldRequest).
	const auto handleContainer = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto change = Protocol::DecodeContainerChange(a_data);
		if (!change || !Protocol::IsValid(*change) || containerChanges.size() >= Protocol::MAX_WORLD_STATE_ACTORS) {
			return;
		}
		const auto index = static_cast<std::uint32_t>(containerChanges.size());
		containerChanges.push_back({ index, a_player.id, *change });
		broadcast(Protocol::Encode(containerChanges.back()), true, 0);
	};

	const auto handlePickup = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto pickup = Protocol::DecodeRefPickedUp(a_data);
		if (!pickup || !Protocol::IsShareableRef(pickup->refId) || pickedUp.size() >= Protocol::MAX_WORLD_STATE_ACTORS ||
			!pickedUp.insert(pickup->refId).second) {
			return;
		}
		broadcast(Protocol::Encode(*pickup, Protocol::MessageType::kRefPickedUp), true, a_player.key);
	};

	const auto handleRefState = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!allowEvent(a_player)) {
			return;
		}
		const auto state = Protocol::DecodeRefState(a_data);
		if (!state || !Protocol::IsShareableRef(state->refId) ||
			(refStates.size() >= Protocol::MAX_WORLD_STATE_ACTORS && !refStates.contains(state->refId))) {
			return;
		}
		auto& stored = refStates[state->refId];
		if (stored == *state) {
			return;
		}
		stored = *state;
		broadcast(Protocol::Encode(*state, Protocol::MessageType::kRefStateChanged), true, a_player.key);
	};

	// Who runs an NPC's AI:
	// - nobody yet: the first player to claim it;
	// - a companion follows its player, so that player takes it over, unless it is also another
	//   player's companion (then the first keeps it);
	// - a player fighting or talking to it takes it over, unless it is someone's companion or
	//   changed hands moments ago (two players fighting it would otherwise trade it constantly).
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
		for (const auto& claim : *claims) {
			if (!Protocol::IsShareableRef(claim.refId) || deadActors.contains(claim.refId)) {
				continue;
			}
			const auto it = actorOwners.find(claim.refId);
			bool       grant = false;
			if (it == actorOwners.end()) {
				grant = actorOwners.size() < Protocol::MAX_WORLD_STATE_ACTORS;
			} else if (it->second.playerId == a_player.id) {
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
				actorOwners[claim.refId] = { a_player.id, claim.reason, now };
				changed.push_back({ claim.refId, a_player.id });
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
		a_player.face = *face;
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
		const auto key = std::pair{ a_player.id, item->refId };
		if (item->op == Protocol::WorkshopOp::kScrapped) {
			if (workshopItems.erase(key) == 0) {
				return;  // nothing anyone knew about
			}
		} else {
			if (workshopItems.size() >= MAX_WORKSHOP_ITEMS && !workshopItems.contains(key)) {
				return;
			}
			workshopItems[key] = *item;
		}
		broadcast(Protocol::Encode(*item, Protocol::MessageType::kWorkshopItem), true, a_player.key);
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
		for (const auto& [key, other] : players) {
			if (other.welcomed && other.id == hit->playerId) {
				send(key, Protocol::Encode(Protocol::PlayerHit{ a_player.id, hit->damage, hit->byPlayer, hit->attacker }, Protocol::MessageType::kPlayerDamaged), true);
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
		broadcast(Protocol::Encode(Protocol::Shot{ a_player.id, shot->refId, shot->action }, Protocol::MessageType::kShotFired), false, a_player.key);
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

	// The session's clock is the host's: a player on this machine, else the one here longest.
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

	const auto handleLine = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		const auto now = Clock::now();
		if (!allowEvent(a_player) || now - a_player.lastLine < std::chrono::milliseconds(250)) {
			return;
		}
		auto line = Protocol::DecodeLine(a_data);
		if (!line) {
			return;
		}
		a_player.lastLine = now;
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
		const float elapsed = std::chrono::duration<float>(now - a_player.xpRefilled).count();
		a_player.xpRefilled = now;
		a_player.xpBudget = (std::min)(Protocol::MAX_XP_SHARE, a_player.xpBudget + elapsed * Protocol::MAX_XP_SHARE / std::chrono::duration<float>(XP_REFILL_TIME).count());
		a_player.xpReports = (std::min)(static_cast<float>(XP_REPORT_BURST), a_player.xpReports + elapsed / std::chrono::duration<float>(XP_REPORT_INTERVAL).count());
		gain->xp = (std::min)(gain->xp, a_player.xpBudget);
		if (a_player.xpReports < 1.0f || gain->xp < 1.0f) {
			return;
		}
		a_player.xpReports -= 1.0f;
		a_player.xpBudget -= gain->xp;
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
		if (!stage || !Protocol::IsShareableRef(stage->quest) || questStages.size() >= Protocol::MAX_WORLD_STATE_ACTORS ||
			std::ranges::find(questStages, *stage) != questStages.end()) {
			return;
		}
		questStages.push_back(*stage);
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
		if (!allowEvent(a_player) || !allowWorldState(a_player)) {
			return;
		}
		if (const auto request = Protocol::DecodeWorldRequest(a_data)) {
			sendWorldState(a_player.key, request->sessionId == sessionId ? request->containerFrom : 0);
		}
	};

	const auto handlePacket = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		using Protocol::MessageType;
		switch (Protocol::PeekType(a_data).value_or(MessageType{})) {
		case MessageType::kHello:
			if (a_player.kicked) {
				break;
			}
			if (a_player.welcomed) {
				// Hello again on the same connection: their game restarted before it timed out
				// (Steam keeps one connection per friend). Start over as a new arrival.
				if (!allowWorldState(a_player)) {
					break;
				}
				leave(a_player);
				Player fresh;
				fresh.key = a_player.key;
				fresh.id = a_player.id;
				fresh.connectedAt = Clock::now();
				fresh.rate = a_player.rate;  // saying hello again doesn't refill the budgets
				a_player = std::move(fresh);
			}
			handleHello(a_player, a_data);
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
						Player player;
						player.key = key;
						player.id = nextId++;
						player.connectedAt = Clock::now();
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
						leave(it->second);
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
			if (!player.welcomed && !player.kicked && now - player.connectedAt > HELLO_TIMEOUT) {
				log(std::format("server: dropping {} (no hello)", describe(key)));
				disconnect(player);
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

		// The session goes to disk when it changed (checked every 15 s) and when the server stops.
		if (Clock::now() >= nextSave) {
			nextSave = Clock::now() + std::chrono::seconds(15);
			if (stateSize() != lastSavedSize) {
				saveSession();
			}
		}
	}
	saveSession();

	for (auto& transport : transports) {
		transport->Close();
	}
}
