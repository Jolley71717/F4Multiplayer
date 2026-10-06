#include "Server.h"

#include "Net.h"
#include "Protocol.h"

#include <chrono>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace
{
	using Clock = std::chrono::steady_clock;

	constexpr auto TICK_INTERVAL = std::chrono::milliseconds(33);  // ~30 Hz state broadcast
	constexpr int  MAX_STATES_PER_SECOND = 60;
	constexpr int  MAX_EVENTS_PER_SECOND = 100;

	struct Player
	{
		ENetPeer*             peer = nullptr;
		std::uint32_t         id = 0;
		std::string           name;
		std::uint32_t         appearance = 0;
		std::vector<std::uint32_t> equipment;
		bool                  welcomed = false;
		bool                  hasState = false;
		bool                  stateDirty = false;
		Protocol::PlayerState state;
		Clock::time_point     rateWindowStart{};
		int                   statesInWindow = 0;
		int                   eventsInWindow = 0;
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
}

Server::Server(LogFn a_log) :
	log(std::move(a_log))
{}

Server::~Server()
{
	Stop();
}

bool Server::Start(const Options& a_options)
{
	if (running) {
		return true;
	}

	if (!Net::Initialize()) {
		log("server: failed to initialize networking");
		return false;
	}

	options = a_options;

	ENetAddress address{};
	address.host = ENET_HOST_ANY;
	address.port = options.port;

	// Allow a couple of extra connections so full servers can still send a rejection.
	const auto host = enet_host_create(&address, options.maxPlayers + 2, Protocol::CHANNEL_COUNT, 0, 0);
	if (!host) {
		log(std::format("server: could not bind UDP port {} (already in use?)", options.port));
		Net::Shutdown();
		return false;
	}

	stopRequested = false;
	running = true;
	thread = std::thread([this, host] { Run(host); });
	log(std::format("server: listening on UDP port {} (max {} players)", options.port, options.maxPlayers));
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
	running = false;
	Net::Shutdown();
	log("server: stopped");
}

void Server::Run(void* a_host)
{
	const auto host = static_cast<ENetHost*>(a_host);

	std::unordered_map<ENetPeer*, Player> players;
	std::uint32_t                          nextId = 1;
	std::uint32_t                          tick = 0;
	std::uint32_t                          sessionContentHash = 0;  // set by the first player

	// Shared world state for this session.
	std::unordered_set<std::uint32_t>         deadActors;
	std::vector<Protocol::ContainerChange>    containerChanges;
	std::unordered_set<std::uint32_t>         pickedUp;
	std::unordered_map<std::uint32_t, Protocol::RefState> refStates;

	const auto shareable = [](std::uint32_t a_id) { return a_id != 0 && (a_id >> 24) != 0xFF; };

	const auto relayToOthers = [&](const Player& a_from, const std::vector<std::uint8_t>& a_packet) {
		for (const auto& [peer, other] : players) {
			if (peer != a_from.peer && other.welcomed) {
				Net::Send(peer, a_packet, true);
			}
		}
	};
	auto                                   nextTick = Clock::now();

	const auto welcomedCount = [&] {
		std::size_t count = 0;
		for (const auto& [peer, player] : players) {
			count += player.welcomed;
		}
		return count;
	};

	const auto reject = [&](ENetPeer* a_peer, std::string_view a_reason) {
		Net::Send(a_peer, Protocol::Encode(Protocol::Reject{ std::string{ a_reason } }), true);
		enet_peer_disconnect_later(a_peer, 0);
	};

	const auto handleHello = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		const auto hello = Protocol::DecodeHello(a_data);
		if (!hello || hello->magic != Protocol::MAGIC) {
			reject(a_player.peer, "Not an F4Multiplayer client");
			return;
		}
		if (hello->version != Protocol::VERSION) {
			reject(a_player.peer, std::format("Version mismatch: server uses protocol {}, you have {}. Install the same mod version.", Protocol::VERSION, hello->version));
			return;
		}
		if (welcomedCount() >= options.maxPlayers) {
			reject(a_player.peer, "Server is full");
			return;
		}
		if (!options.password.empty() && hello->password != options.password) {
			reject(a_player.peer, "Wrong password");
			return;
		}
		if (welcomedCount() == 0) {
			sessionContentHash = hello->contentHash;
		} else if (hello->contentHash != sessionContentHash) {
			reject(a_player.peer, "Your load order is different from the other players. Everyone needs the same mods in the same order.");
			return;
		}

		a_player.name = SanitizeName(hello->name, a_player.id);
		a_player.appearance = hello->appearance;
		a_player.welcomed = true;
		Net::Send(a_player.peer, Protocol::Encode(Protocol::Welcome{ a_player.id }), true);
		if (!deadActors.empty() || !containerChanges.empty() || !pickedUp.empty() || !refStates.empty()) {
			Protocol::WorldState world;
			world.deadActors.assign(deadActors.begin(), deadActors.end());
			world.containerChanges = containerChanges;
			world.pickedUp.assign(pickedUp.begin(), pickedUp.end());
			for (const auto& [id, state] : refStates) {
				world.refStates.push_back(state);
			}
			Net::Send(a_player.peer, Protocol::Encode(world), true);
		}

		for (auto& [peer, other] : players) {
			if (!other.welcomed || peer == a_player.peer) {
				continue;
			}
			Net::Send(a_player.peer, Protocol::Encode(Protocol::PlayerJoined{ other.id, other.name, other.appearance }), true);
			Net::Send(peer, Protocol::Encode(Protocol::PlayerJoined{ a_player.id, a_player.name, a_player.appearance }), true);
			if (!other.equipment.empty()) {
				Net::Send(a_player.peer, Protocol::Encode(Protocol::Equipment{ other.id, other.equipment }, Protocol::MessageType::kPlayerEquipment), true);
			}
			// Let the newcomer see players who are standing still right away.
			other.stateDirty = other.hasState;
		}

		log(std::format("server: '{}' joined as player {} ({} online)", a_player.name, a_player.id, welcomedCount()));
	};

	const auto handleState = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed) {
			return;
		}

		const auto now = Clock::now();
		if (now - a_player.rateWindowStart >= std::chrono::seconds(1)) {
			a_player.rateWindowStart = now;
			a_player.statesInWindow = 0;
			a_player.eventsInWindow = 0;
		}
		if (++a_player.statesInWindow > MAX_STATES_PER_SECOND) {
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
		if (!a_player.welcomed || ++a_player.eventsInWindow > MAX_EVENTS_PER_SECOND) {
			return;
		}
		const auto death = Protocol::DecodeActorDeath(a_data);
		// Runtime-created references (0xFF......) differ between games and can't be shared.
		if (!death || death->refId == 0 || (death->refId >> 24) == 0xFF) {
			return;
		}
		if (deadActors.size() >= Protocol::MAX_WORLD_STATE_ACTORS || !deadActors.insert(death->refId).second) {
			return;
		}
		const auto relay = Protocol::Encode(*death, Protocol::MessageType::kActorDied);
		for (const auto& [peer, other] : players) {
			if (peer != a_player.peer && other.welcomed) {
				Net::Send(peer, relay, true);
			}
		}
	};

	const auto handleHealth = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed || ++a_player.eventsInWindow > MAX_EVENTS_PER_SECOND) {
			return;
		}
		const auto health = Protocol::DecodeActorHealth(a_data);
		if (!health || health->refId == 0 || (health->refId >> 24) == 0xFF || deadActors.contains(health->refId)) {
			return;
		}
		const auto relay = Protocol::Encode(*health, Protocol::MessageType::kActorHealth);
		for (const auto& [peer, other] : players) {
			if (peer != a_player.peer && other.welcomed) {
				Net::Send(peer, relay, true);
			}
		}
	};

	const auto handleContainer = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed || ++a_player.eventsInWindow > MAX_EVENTS_PER_SECOND) {
			return;
		}
		const auto change = Protocol::DecodeContainerChange(a_data);
		if (!change || !shareable(change->container) || change->item == 0 || change->count == 0 ||
			containerChanges.size() >= Protocol::MAX_WORLD_STATE_ACTORS) {
			return;
		}
		containerChanges.push_back(*change);
		relayToOthers(a_player, Protocol::Encode(*change, Protocol::MessageType::kContainerChanged));
	};

	const auto handlePickup = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed || ++a_player.eventsInWindow > MAX_EVENTS_PER_SECOND) {
			return;
		}
		const auto pickup = Protocol::DecodeRefPickedUp(a_data);
		if (!pickup || !shareable(pickup->refId) || pickedUp.size() >= Protocol::MAX_WORLD_STATE_ACTORS ||
			!pickedUp.insert(pickup->refId).second) {
			return;
		}
		relayToOthers(a_player, Protocol::Encode(*pickup, Protocol::MessageType::kRefPickedUp));
	};

	const auto handleRefState = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed || ++a_player.eventsInWindow > MAX_EVENTS_PER_SECOND) {
			return;
		}
		const auto state = Protocol::DecodeRefState(a_data);
		if (!state || !shareable(state->refId) || (refStates.size() >= Protocol::MAX_WORLD_STATE_ACTORS && !refStates.contains(state->refId))) {
			return;
		}
		auto& stored = refStates[state->refId];
		if (stored == *state) {
			return;
		}
		stored = *state;
		relayToOthers(a_player, Protocol::Encode(*state, Protocol::MessageType::kRefStateChanged));
	};

	const auto handleEquipment = [&](Player& a_player, std::span<const std::uint8_t> a_data) {
		if (!a_player.welcomed || ++a_player.eventsInWindow > MAX_EVENTS_PER_SECOND) {
			return;
		}
		auto equipment = Protocol::DecodeEquipment(a_data);
		if (!equipment) {
			return;
		}
		equipment->playerId = a_player.id;
		a_player.equipment = equipment->items;
		relayToOthers(a_player, Protocol::Encode(*equipment, Protocol::MessageType::kPlayerEquipment));
	};

	while (!stopRequested) {
		ENetEvent event;
		while (enet_host_service(host, &event, 2) > 0) {
			switch (event.type) {
			case ENET_EVENT_TYPE_CONNECT:
				{
					Player player;
					player.peer = event.peer;
					player.id = nextId++;
					players.emplace(event.peer, std::move(player));
					enet_peer_timeout(event.peer, 0, 10000, 20000);
				}
				break;

			case ENET_EVENT_TYPE_RECEIVE:
				{
					const std::span<const std::uint8_t> data{ event.packet->data, event.packet->dataLength };
					if (const auto it = players.find(event.peer); it != players.end()) {
						switch (Protocol::PeekType(data).value_or(Protocol::MessageType{})) {
						case Protocol::MessageType::kHello:
							if (!it->second.welcomed) {
								handleHello(it->second, data);
							}
							break;
						case Protocol::MessageType::kPlayerState:
							handleState(it->second, data);
							break;
						case Protocol::MessageType::kReportDeath:
							handleDeath(it->second, data);
							break;
						case Protocol::MessageType::kReportHealth:
							handleHealth(it->second, data);
							break;
						case Protocol::MessageType::kReportContainer:
							handleContainer(it->second, data);
							break;
						case Protocol::MessageType::kReportPickup:
							handlePickup(it->second, data);
							break;
						case Protocol::MessageType::kReportRefState:
							handleRefState(it->second, data);
							break;
						case Protocol::MessageType::kEquipment:
							handleEquipment(it->second, data);
							break;
						default:
							break;
						}
					}
					enet_packet_destroy(event.packet);
				}
				break;

			case ENET_EVENT_TYPE_DISCONNECT:
				if (const auto it = players.find(event.peer); it != players.end()) {
					if (it->second.welcomed) {
						const auto left = Protocol::Encode(Protocol::PlayerLeft{ it->second.id });
						for (const auto& [peer, other] : players) {
							if (peer != event.peer && other.welcomed) {
								Net::Send(peer, left, true);
							}
						}
						log(std::format("server: '{}' left", it->second.name));
					}
					players.erase(it);
				}
				break;

			default:
				break;
			}
		}

		const auto now = Clock::now();
		if (now < nextTick) {
			continue;
		}
		nextTick = now + TICK_INTERVAL;
		++tick;

		// Relay changed player states to everyone else.
		for (auto& [peer, receiver] : players) {
			if (!receiver.welcomed) {
				continue;
			}
			Protocol::PlayerStates batch{ tick, {} };
			for (const auto& [otherPeer, other] : players) {
				if (otherPeer != peer && other.welcomed && other.hasState && other.stateDirty) {
					batch.players.push_back({ other.id, other.state });
				}
			}
			if (!batch.players.empty()) {
				Net::Send(peer, Protocol::Encode(batch), false);
			}
		}
		for (auto& [peer, player] : players) {
			player.stateDirty = false;
		}
	}

	for (const auto& [peer, player] : players) {
		enet_peer_disconnect_now(peer, 0);
	}
	enet_host_flush(host);
	enet_host_destroy(host);
}
