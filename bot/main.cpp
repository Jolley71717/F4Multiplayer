// Test client that pretends to be a player: connects to a server and walks in a circle.
//
//   F4MPBot --server 127.0.0.1:7779 --name Bot --cell 0 --worldspace 0000003C
//           --x -80352 --y 89600 --z 7790 [--radius 300] [--speed 150] [--content-hash HEX] [--seconds N]
//           [--shoot WEAPONHEX]  (holds the weapon out and reports shots; with --own, the NPC's)
//           [--status-health PERCENT] [--ping 1] [--xp N]  [--time HOUR --weather HEX]  (health in the status sent after joining, an "over here", a kill XP report)
//           [--quest-done QUESTHEX]  (reports completing this quest once welcomed)
//           [--downed 1] [--revive PLAYERID]  (status says the bot is down; helps that player up once welcomed)
//           [--voice-echo 1]  (sends every piece of voice it hears back as its own)
//           [--voice-silence 1]  (talks: Steam voice packets holding 100 ms of silence, 10 a second)

#include "Net.h"
#include "Protocol.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>

namespace
{
	std::uint32_t Crc32(std::span<const std::uint8_t> a_data)
	{
		std::uint32_t crc = 0xFFFFFFFF;
		for (const auto byte : a_data) {
			crc ^= byte;
			for (int i = 0; i < 8; ++i) {
				crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
			}
		}
		return ~crc;
	}

	// A Steam voice packet: the speaker's Steam ID, a sample rate (type 11) and a run of silent
	// samples (type 0), then a CRC32 of all that.
	std::vector<std::uint8_t> SilencePacket(std::uint16_t a_samples)
	{
		std::vector<std::uint8_t> packet;
		const auto put = [&](const auto a_value) {
			const auto bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
			packet.insert(packet.end(), bytes, bytes + sizeof(a_value));
		};
		put(std::uint64_t{ 0x0110000100000001 });
		put(std::uint8_t{ 11 });
		put(std::uint16_t{ 24000 });
		put(std::uint8_t{ 0 });
		put(a_samples);
		put(Crc32(packet));
		return packet;
	}

	template <class T>
	bool ParseNumber(std::string_view a_str, T& a_out, int a_base = 10)
	{
		if constexpr (std::is_floating_point_v<T>) {
			const auto [ptr, ec] = std::from_chars(a_str.data(), a_str.data() + a_str.size(), a_out);
			return ec == std::errc{} && ptr == a_str.data() + a_str.size();
		} else {
			if (a_base == 16 && (a_str.starts_with("0x") || a_str.starts_with("0X"))) {
				a_str.remove_prefix(2);
			}
			const auto [ptr, ec] = std::from_chars(a_str.data(), a_str.data() + a_str.size(), a_out, a_base);
			return ec == std::errc{} && ptr == a_str.data() + a_str.size();
		}
	}
}

int main(int argc, char* argv[])
{
	std::string   server = "127.0.0.1";
	std::string   name = "Bot";
	std::string   password;
	std::uint32_t cell = 0;
	std::uint32_t worldspace = 0x3C;  // Commonwealth
	std::uint32_t contentHash = 0;
	std::uint32_t appearance = 0;
	float         cx = 0, cy = 0, cz = 0, radius = 300, walkSpeed = 150;
	int           seconds = 0;  // 0 = run until killed
	bool          jump = false;  // jump once during each standing phase
	std::uint32_t shootWeapon = 0;  // hold this weapon drawn and fire it twice a second while standing
	int           statusHealth = 100;
	bool          statusDowned = false;
	bool          voiceEcho = false;
	bool          voiceSilence = false;
	std::uint64_t voiceBytes = 0;
	std::uint32_t reviveTarget = 0;
	bool          sendPing = false;
	float         xpGain = 0.0f;
	std::uint32_t questDone = 0;
	float         timeHour = -1.0f;  // report this time of day (and timeWeather) every 5 s
	std::uint32_t timeWeather = 0;
	std::uint32_t killRef = 0;   // report this actor as killed once welcomed
	std::uint32_t healthRef = 0;  // report this actor's health as healthValue once welcomed
	std::uint32_t pickupRef = 0;      // report picking up this world item once welcomed
	Protocol::RefState doorState;     // report this door state once welcomed (refId 0 = none)
	Protocol::QuestStage questStage;  // report this quest stage once welcomed (quest 0 = none)
	std::uint32_t ownRef = 0;         // take over this NPC and walk it around the circle instead of ourselves
	Protocol::PlayerHit hitPlayer;    // tell this player an NPC hit them (playerId 0 = none)
	std::uint32_t actorStatesSeen = 0;
	std::uint32_t lootContainer = 0;  // take lootCount of lootItem from this container once welcomed
	std::uint32_t lootItem = 0;
	std::int32_t  lootCount = 0;
	float         healthValue = 0;
	Protocol::WorldRequest world;  // what our "world" already has (--session, --from)

	if (argc % 2 == 0) {
		std::cerr << "missing value for " << argv[argc - 1] << '\n';
		return 2;
	}
	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string_view key = argv[i];
		const std::string_view value = argv[i + 1];
		bool                   ok = true;
		if (key == "--server") {
			server = value;
		} else if (key == "--name") {
			name = value;
		} else if (key == "--password") {
			password = value;
		} else if (key == "--cell") {
			ok = ParseNumber(value, cell, 16);
		} else if (key == "--worldspace") {
			ok = ParseNumber(value, worldspace, 16);
		} else if (key == "--appearance") {
			ok = ParseNumber(value, appearance, 16);
		} else if (key == "--content-hash") {
			ok = ParseNumber(value, contentHash, 16);
		} else if (key == "--x") {
			ok = ParseNumber(value, cx);
		} else if (key == "--y") {
			ok = ParseNumber(value, cy);
		} else if (key == "--z") {
			ok = ParseNumber(value, cz);
		} else if (key == "--radius") {
			ok = ParseNumber(value, radius);
		} else if (key == "--speed") {
			ok = ParseNumber(value, walkSpeed);
		} else if (key == "--quest") {
			// quest:stage, hex:decimal
			const auto colon = value.find(':');
			ok = colon != std::string_view::npos && ParseNumber(value.substr(0, colon), questStage.quest, 16) && ParseNumber(value.substr(colon + 1), questStage.stage);
		} else if (key == "--own") {
			ok = ParseNumber(value, ownRef, 16);
		} else if (key == "--hit-player") {
			// id:damage, or id:damage:p for a hit by the bot itself (friendly fire)
			auto colon = value.find(':');
			auto rest = colon != std::string_view::npos ? value.substr(colon + 1) : std::string_view{};
			if (rest.ends_with(":p")) {
				hitPlayer.byPlayer = true;
				rest.remove_suffix(2);
			}
			ok = colon != std::string_view::npos && ParseNumber(value.substr(0, colon), hitPlayer.playerId) && ParseNumber(rest, hitPlayer.damage);
		} else if (key == "--door") {
			// ref:open:locked, hex:0|1|2:0|1|2 (2 = leave alone)
			const auto first = value.find(':');
			const auto second = value.find(':', first + 1);
			ok = first != std::string_view::npos && second != std::string_view::npos &&
			     ParseNumber(value.substr(0, first), doorState.refId, 16) &&
			     ParseNumber(value.substr(first + 1, second - first - 1), doorState.open) &&
			     ParseNumber(value.substr(second + 1), doorState.locked);
		} else if (key == "--pickup") {
			ok = ParseNumber(value, pickupRef, 16);
		} else if (key == "--loot") {
			// container:item:count, hex:hex:decimal (negative count = take)
			const auto first = value.find(':');
			const auto second = value.find(':', first + 1);
			ok = first != std::string_view::npos && second != std::string_view::npos &&
			     ParseNumber(value.substr(0, first), lootContainer, 16) &&
			     ParseNumber(value.substr(first + 1, second - first - 1), lootItem, 16) &&
			     ParseNumber(value.substr(second + 1), lootCount);
		} else if (key == "--health-ref") {
			ok = ParseNumber(value, healthRef, 16);
		} else if (key == "--health") {
			ok = ParseNumber(value, healthValue);
		} else if (key == "--session") {
			ok = ParseNumber(value, world.sessionId, 16);
		} else if (key == "--from") {
			ok = ParseNumber(value, world.containerFrom);
		} else if (key == "--kill") {
			ok = ParseNumber(value, killRef, 16);
		} else if (key == "--jump") {
			jump = value == "1";
		} else if (key == "--shoot") {
			ok = ParseNumber(value, shootWeapon, 16);
		} else if (key == "--status-health") {
			ok = ParseNumber(value, statusHealth) && statusHealth >= 0 && statusHealth <= 100;
		} else if (key == "--voice-echo") {
			voiceEcho = value == "1";
		} else if (key == "--voice-silence") {
			voiceSilence = value == "1";
		} else if (key == "--downed") {
			statusDowned = value == "1";
		} else if (key == "--revive") {
			ok = ParseNumber(value, reviveTarget);
		} else if (key == "--ping") {
			sendPing = value == "1";
		} else if (key == "--xp") {
			ok = ParseNumber(value, xpGain);
		} else if (key == "--quest-done") {
			ok = ParseNumber(value, questDone, 16);
		} else if (key == "--time") {
			ok = ParseNumber(value, timeHour) && timeHour >= 0.0f && timeHour < 24.0f;
		} else if (key == "--weather") {
			ok = ParseNumber(value, timeWeather, 16);
		} else if (key == "--seconds") {
			ok = ParseNumber(value, seconds);
		} else {
			ok = false;
		}
		if (!ok) {
			std::cerr << "bad argument: " << key << ' ' << value << '\n';
			return 2;
		}
	}

	if (!Net::Initialize()) {
		std::cerr << "enet init failed\n";
		return 1;
	}

	ENetAddress address{};
	if (!Net::ResolveAddress(server, Protocol::DEFAULT_PORT, address)) {
		std::cerr << "cannot resolve " << server << '\n';
		return 1;
	}

	const auto host = enet_host_create(nullptr, 1, Protocol::CHANNEL_COUNT, 0, 0);
	const auto peer = enet_host_connect(host, &address, Protocol::CHANNEL_COUNT, 0);
	if (!host || !peer) {
		std::cerr << "enet connect failed\n";
		return 1;
	}

	using Clock = std::chrono::steady_clock;
	const auto start = Clock::now();
	auto       nextSend = start;
	auto       nextVoice = start;
	auto       lastStep = start;
	float      angle = 0.0f;  // position on the circle, radians
	bool       welcomed = false;
	std::uint32_t sequence = 0;

	for (;;) {
		ENetEvent event;
		while (enet_host_service(host, &event, 5) > 0) {
			switch (event.type) {
			case ENET_EVENT_TYPE_CONNECT:
				std::cout << "connected, sending hello\n";
				Net::Send(peer, Protocol::Encode(Protocol::Hello{ .contentHash = contentHash, .name = name, .password = password, .appearance = appearance, .world = world }), true);
				break;
			case ENET_EVENT_TYPE_RECEIVE:
				{
					const std::span<const std::uint8_t> data{ event.packet->data, event.packet->dataLength };
					switch (Protocol::PeekType(data).value_or(Protocol::MessageType{})) {
					case Protocol::MessageType::kWelcome:
						if (const auto msg = Protocol::DecodeWelcome(data)) {
							std::cout << "welcomed as player " << msg->playerId << " in session " << std::hex << msg->sessionId << std::dec << (msg->friendlyFire ? " (friendly fire)" : "") << '\n';
							welcomed = true;
							if (shootWeapon) {
								Net::Send(peer, Protocol::Encode(Protocol::Equipment{ 0, { shootWeapon } }, Protocol::MessageType::kEquipment), true);
							}
							Net::Send(peer, Protocol::Encode(Protocol::PlayerStatus{ 0, 0x0001F228, 0, static_cast<std::uint8_t>(statusDowned ? 0 : statusHealth), 7, statusDowned }, Protocol::MessageType::kReportStatus), true);
							if (reviveTarget) {
								Net::Send(peer, Protocol::Encode(Protocol::Revive{ reviveTarget }, Protocol::MessageType::kRevive), true);
							}
							if (sendPing) {
								Net::Send(peer, Protocol::Encode(Protocol::Ping{ 0, cell, cell ? 0 : worldspace, cx + 2000.0f, cy, cz }, Protocol::MessageType::kPing), true);
							}
							if (xpGain > 0.0f) {
								Net::Send(peer, Protocol::Encode(Protocol::XpGain{ 0, xpGain }, Protocol::MessageType::kReportXp), true);
							}
							if (questDone) {
								Net::Send(peer, Protocol::Encode(Protocol::QuestDone{ 0, questDone }, Protocol::MessageType::kReportQuestDone), true);
							}
							if (questStage.quest) {
								std::cout << "reporting quest stage\n";
								Net::Send(peer, Protocol::Encode(questStage, Protocol::MessageType::kReportQuestStage), true);
							}
							if (ownRef) {
								std::cout << "claiming npc\n";
								Net::Send(peer, Protocol::Encode(std::vector<Protocol::ActorClaim>{ { ownRef, Protocol::ClaimReason::kCompanion } }), true);
							}
							if (hitPlayer.playerId) {
								std::cout << "reporting player hit\n";
								Net::Send(peer, Protocol::Encode(hitPlayer, Protocol::MessageType::kPlayerHit), true);
							}
							if (doorState.refId) {
								std::cout << "reporting door state\n";
								Net::Send(peer, Protocol::Encode(doorState, Protocol::MessageType::kReportRefState), true);
							}
							if (pickupRef) {
								std::cout << "reporting pickup\n";
								Net::Send(peer, Protocol::Encode(Protocol::RefPickedUp{ pickupRef }, Protocol::MessageType::kReportPickup), true);
							}
							if (lootContainer) {
								std::cout << "reporting container change\n";
								Net::Send(peer, Protocol::Encode(Protocol::ContainerChange{ lootContainer, lootItem, lootCount }, Protocol::MessageType::kReportContainer), true);
							}
							if (healthRef) {
								std::cout << "reporting health of " << std::hex << healthRef << std::dec << " = " << healthValue << '\n';
								Net::Send(peer, Protocol::Encode(Protocol::ActorHealth{ healthRef, healthValue }, Protocol::MessageType::kReportHealth), true);
							}
							if (killRef) {
								std::cout << "reporting kill of " << std::hex << killRef << std::dec << '\n';
								Net::Send(peer, Protocol::Encode(Protocol::ActorDeath{ killRef, 0, true }, Protocol::MessageType::kReportDeath), true);
							}
						}
						break;
					case Protocol::MessageType::kReject:
						if (const auto msg = Protocol::DecodeReject(data)) {
							std::cout << "rejected: " << msg->reason << '\n';
						}
						return 3;
					case Protocol::MessageType::kPlayerJoined:
						if (const auto msg = Protocol::DecodePlayerJoined(data)) {
							std::cout << "player joined: " << msg->playerId << " " << msg->name << '\n';
						}
						break;
					case Protocol::MessageType::kActorDied:
						if (const auto msg = Protocol::DecodeActorDeath(data)) {
							std::cout << "actor died: " << std::hex << msg->refId << std::dec << " reported by " << msg->playerId << (msg->killed ? " (killed it)" : "") << std::endl;
						}
						break;
					case Protocol::MessageType::kActorHealth:
						if (const auto msg = Protocol::DecodeActorHealth(data)) {
							std::cout << "actor health: " << std::hex << msg->refId << std::dec << " = " << msg->health << std::endl;
						}
						break;
					case Protocol::MessageType::kContainerChanged:
						if (const auto msg = Protocol::DecodeIndexedContainerChange(data)) {
							std::cout << "container changed #" << msg->index << " by player " << msg->playerId << ": " << std::hex << msg->change.container << " item "
									  << msg->change.item << std::dec << " count " << msg->change.count << std::endl;
						}
						break;
					case Protocol::MessageType::kRefPickedUp:
						if (const auto msg = Protocol::DecodeRefPickedUp(data)) {
							std::cout << "picked up: " << std::hex << msg->refId << std::dec << std::endl;
						}
						break;
					case Protocol::MessageType::kQuestStage:
						if (const auto msg = Protocol::DecodeQuestStage(data)) {
							std::cout << "quest stage: " << std::hex << msg->quest << std::dec << " " << msg->stage << std::endl;
						}
						break;
					case Protocol::MessageType::kPlayerStatus:
						if (const auto msg = Protocol::DecodePlayerStatus(data)) {
							std::cout << "status: player " << msg->playerId << " hp " << int(msg->health) << " level " << msg->level << std::hex << " location " << msg->location << " cell " << msg->cell << std::dec << (msg->downed ? " downed" : "") << std::endl;
						}
						break;
					case Protocol::MessageType::kRevived:
						if (const auto msg = Protocol::DecodeRevive(data)) {
							std::cout << "revived by player " << msg->playerId << std::endl;
						}
						break;
					case Protocol::MessageType::kWorldTime:
						if (const auto msg = Protocol::DecodeWorldTime(data)) {
							std::cout << "time: hour " << msg->gameHour << std::hex << " ws " << msg->worldspace << " weather " << msg->weather << std::dec << std::endl;
						}
						break;
					case Protocol::MessageType::kPinged:
						if (const auto msg = Protocol::DecodePing(data)) {
							std::cout << "ping: player " << msg->playerId << " at " << msg->x << "," << msg->y << std::endl;
						}
						break;
					case Protocol::MessageType::kVoiceRelay:
						if (auto msg = Protocol::DecodeVoice(data)) {
							if (voiceBytes == 0) {
								std::cout << "voice: first piece from player " << msg->playerId << ", " << msg->data.size() << " bytes" << std::endl;
							}
							voiceBytes += msg->data.size();
							if (voiceEcho) {
								msg->playerId = 0;
								Net::Send(peer, Protocol::Encode(*msg, Protocol::MessageType::kVoice), false);
							}
						}
						break;
					case Protocol::MessageType::kQuestDone:
						if (const auto msg = Protocol::DecodeQuestDone(data)) {
							std::cout << "quest done: player " << msg->playerId << " " << std::hex << msg->quest << std::dec << std::endl;
						}
						break;
					case Protocol::MessageType::kPartyXp:
						if (const auto msg = Protocol::DecodeXpGain(data)) {
							std::cout << "xp: player " << msg->playerId << " " << msg->xp << std::endl;
						}
						break;
					case Protocol::MessageType::kShotFired:
						if (const auto msg = Protocol::DecodeShot(data)) {
							std::cout << "shot: player " << msg->playerId << " ref " << std::hex << msg->refId << std::dec << std::endl;
						}
						break;
					case Protocol::MessageType::kActorOwners:
						if (const auto msg = Protocol::DecodeOwners(data)) {
							std::cout << "owners:";
							for (const auto& o : *msg) {
								std::cout << ' ' << std::hex << o.refId << std::dec << '=' << o.playerId;
							}
							std::cout << std::endl;
						}
						break;
					case Protocol::MessageType::kActorStatesRelay:
						if (const auto msg = Protocol::DecodeActorStates(data)) {
							// Print every 20th batch so the output stays readable.
							if (actorStatesSeen++ % 20 == 0) {
								std::cout << "npc states:";
								for (const auto& s : *msg) {
									std::cout << ' ' << std::hex << s.refId << std::dec << '@' << int(s.x) << ',' << int(s.y) << ' ' << int(s.speed);
								}
								std::cout << std::endl;
							}
						}
						break;
					case Protocol::MessageType::kPlayerDamaged:
						if (const auto msg = Protocol::DecodePlayerHit(data)) {
							std::cout << "damaged by player " << msg->playerId << (msg->byPlayer ? " themselves: " : "'s npc: ") << msg->damage << std::endl;
						}
						break;
					case Protocol::MessageType::kRefStateChanged:
						if (const auto msg = Protocol::DecodeRefState(data)) {
							std::cout << "ref state: " << std::hex << msg->refId << std::dec << " open=" << int(msg->open) << " locked=" << int(msg->locked) << std::endl;
						}
						break;
					case Protocol::MessageType::kWorldState:
						if (const auto msg = Protocol::DecodeWorldState(data)) {
							std::cout << "world state: " << msg->containerChanges.size() << " container changes from #" << msg->containerFirstIndex << ", " << msg->pickedUp.size() << " pickups, " << msg->refStates.size() << " ref states, " << msg->questStages.size() << " quest stages, " << msg->deadActors.size() << " dead actors:";
							for (const auto id : msg->deadActors) {
								std::cout << ' ' << std::hex << id << std::dec;
							}
							std::cout << std::endl;
						}
						break;
					case Protocol::MessageType::kPlayerLeft:
						if (const auto msg = Protocol::DecodePlayerLeft(data)) {
							std::cout << "player left: " << msg->playerId << '\n';
						}
						break;
					default:
						break;
					}
					enet_packet_destroy(event.packet);
				}
				break;
			case ENET_EVENT_TYPE_DISCONNECT:
				std::cout << "disconnected\n";
				return 4;
			default:
				break;
			}
		}

		const auto now = Clock::now();
		if (seconds > 0 && now - start > std::chrono::seconds(seconds)) {
			break;
		}

		if (welcomed && voiceSilence && now >= nextVoice) {
			nextVoice = now + std::chrono::milliseconds(100);
			Net::Send(peer, Protocol::Encode(Protocol::VoiceData{ 0, SilencePacket(2400) }, Protocol::MessageType::kVoice), false);
		}

		if (welcomed && now >= nextSend) {
			nextSend = now + std::chrono::milliseconds(50);  // 20 Hz like the game client

			// Move around the circle at --speed (walk ~150, run ~370 units/s) for 4 s, then stand for 3 s,
			// so both locomotion start and stop get exercised.
			const float elapsed = std::chrono::duration<float>(now - start).count();
			const float dt = std::chrono::duration<float>(now - lastStep).count();
			lastStep = now;
			const bool  walking = std::fmod(elapsed, 7.0f) < 4.0f;

			if (walking) {
				angle += dt * walkSpeed / radius;
			}
			const float a = angle;

			Protocol::PlayerState state;
			state.sequence = ++sequence;
			state.cell = cell;
			state.worldspace = cell ? 0 : worldspace;
			state.x = cx + radius * std::sin(a);
			state.y = cy + radius * std::cos(a);
			state.z = cz;

			// Optional jump 1 s into the standing phase: 0.8 s arc, 60 units high.
			const float standTime = std::fmod(elapsed, 7.0f) - 4.0f;
			if (jump && standTime > 1.0f && standTime < 1.8f) {
				const float t = (standTime - 1.0f) / 0.8f;
				state.z += 60.0f * std::sin(t * std::numbers::pi_v<float>);
				state.flags |= Protocol::kInAir;
				if (t < 0.5f) {
					state.flags |= Protocol::kJumping;
				}
			}
			state.heading = a + std::numbers::pi_v<float> / 2;  // tangent to the circle
			state.speed = walking ? walkSpeed : 0.0f;
			// ActorState::moveMode: 0x01 forward, 0x40 walking, 0x80 running.
			state.moveMode = walking ? (walkSpeed > 200.0f ? 0x81 : 0x41) : 0;
			const bool shoot = shootWeapon && !walking && sequence % 10 == 0;
			if (timeHour >= 0.0f && sequence % 100 == 1) {
				Net::Send(peer, Protocol::Encode(Protocol::WorldTime{ timeHour, worldspace, timeWeather }, Protocol::MessageType::kReportTime), false);
			}
			if (shootWeapon) {
				state.flags |= Protocol::kWeaponDrawn;
			}
			if (shoot) {
				Net::Send(peer, Protocol::Encode(Protocol::Shot{ 0, ownRef }, Protocol::MessageType::kReportShot), false);
			}
			if (ownRef) {
				// We walk the NPC instead; our own player stays put far away.
				Protocol::ActorState npc;
				npc.refId = ownRef;
				npc.cell = state.cell;
				npc.worldspace = state.worldspace;
				npc.x = state.x;
				npc.y = state.y;
				npc.z = state.z;
				npc.heading = state.heading;
				npc.speed = state.speed;
				npc.moveMode = state.moveMode;
				npc.flags = state.flags & Protocol::kWeaponDrawn;
				if (sequence % 2 == 0) {  // 10 Hz like the game
					Net::Send(peer, Protocol::Encode(std::vector<Protocol::ActorState>{ npc }, Protocol::MessageType::kActorStates), false);
				}
				state.x = state.y = 0.0f;
			}
			Net::Send(peer, Protocol::Encode(state), false);
		}
	}

	if (voiceBytes) {
		std::cout << "voice: " << voiceBytes << " bytes heard" << std::endl;
	}
	enet_peer_disconnect(peer, 0);
	enet_host_flush(host);
	enet_host_destroy(host);
	Net::Shutdown();
	return 0;
}
