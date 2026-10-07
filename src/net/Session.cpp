#include "net/Session.h"

#include "Config.h"
#include "Protocol.h"
#include "Server.h"
#include "game/Compass.h"
#include "game/Conversations.h"
#include "game/Downed.h"
#include "game/Equipment.h"
#include "game/Hud.h"
#include "game/MapShare.h"
#include "game/Voice.h"
#include "game/Party.h"
#include "game/WorldClock.h"
#include "game/NpcSync.h"
#include "game/QuestSync.h"
#include "game/Puppets.h"
#include "game/RemotePlayers.h"
#include "game/Story.h"
#include "game/WeaponFire.h"
#include "game/WorldSync.h"
#include "net/Identity.h"
#include "net/NetClient.h"
#include "steam/Steam.h"
#include "steam/SteamTransport.h"

namespace Session
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto SEND_INTERVAL = 33ms;  // 30 Hz
		constexpr auto RECONNECT_DELAY = 5s;
		constexpr auto REJECTED_RETRY_DELAY = 60s;

		std::uint32_t           contentHash = 0;
		std::unique_ptr<Server> hostedServer;
		bool                    steamMode = false;  // friends connect through Steam

		// The connection in use: UDP for "ip[:port]" addresses (and the host's own game), Steam for
		// "steam:<id>".
		NetClient         udpClient;
		SteamClient       steamClient;
		ClientConnection* client = &udpClient;
		std::string       serverAddress;

		bool              welcomed = false;
		std::uint32_t     localId = 0;
		std::uint32_t     sequence = 0;
		Clock::time_point nextSend{};

		// Equipment is checked once a second and sent when it changes (and after joining).
		constexpr auto                            EQUIPMENT_INTERVAL = 1s;
		Clock::time_point                         nextEquipmentCheck{};
		std::optional<std::vector<std::uint32_t>> sentEquipment;
		Clock::time_point nextConnectAttempt{};
		std::string       lastRejectReason;

		RE::NiPoint3      lastPosition;
		Clock::time_point lastPositionTime{};

		void Notify(const std::string& a_message)
		{
			Hud::Notify(a_message);
		}

		// FNV-1a over the load order, so players with different mods are told up front.
		std::uint32_t ComputeContentHash()
		{
			std::uint32_t hash = 2166136261u;
			const auto    mix = [&](std::string_view a_text) {
                for (const char c : a_text) {
                    hash ^= static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(c)));
                    hash *= 16777619u;
                }
                hash ^= '|';
                hash *= 16777619u;
			};

			const auto data = RE::TESDataHandler::GetSingleton();
			for (const auto file : data->compiledFileCollection.files) {
				mix(file->GetFilename());
			}
			mix("--light--");
			for (const auto file : data->compiledFileCollection.smallFiles) {
				mix(file->GetFilename());
			}
			return hash;
		}

		std::string PlayerName()
		{
			const auto& name = Config::Get().playerName;
			if (!name.empty()) {
				return name;
			}
			auto steamName = Steam::PersonaName();
			return steamName.empty() ? "Vault Dweller" : steamName;
		}

		bool InGame()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetParentCell();
		}

		std::uint32_t hitsTaken = 0;
		std::uint32_t hitsIgnored = 0;
		float         lastHitMult = 1.0f;  // how much the last hit was scaled (difficulty, VATS)

		// Whether the NPC that hit our stand-in is there to see in our world: loaded, alive and
		// within weapon range. Otherwise we'd take damage from nowhere (it died in our game, or
		// hasn't spawned yet).
		bool AttackerHere(const RE::PlayerCharacter* a_player, std::uint32_t a_attacker)
		{
			constexpr float RANGE = 10000.0f;  // about 140 m: a sniper rifle's reach
			const auto      actor = RE::TESForm::GetFormByID<RE::Actor>(a_attacker);
			if (!actor || !actor->Get3D() || actor->IsDead(false)) {
				return false;
			}
			const auto here = a_player->GetParentCell();
			const auto there = actor->GetParentCell();
			if (!here || !there || (here->IsInterior() || there->IsInterior() ? here != there : here->worldSpace != there->worldSpace)) {
				return false;
			}
			return actor->data.location.GetDistance(a_player->data.location) < RANGE;
		}

		// Menus that stop the game (pause menu, Pip-Boy, ...): no damage lands while it's stopped.
		bool GamePaused()
		{
			const auto ui = RE::UI::GetSingleton();
			return ui && ui->menuMode > 0;
		}

		// Something hit our stand-in in another player's world.
		float GameSettingFloat(const char* a_name, float a_default)
		{
			const auto settings = RE::GameSettingCollection::GetSingleton();
			const auto setting = settings ? settings->GetSetting(a_name) : nullptr;
			return setting && setting->GetType() == RE::Setting::SETTING_TYPE::kFloat ? setting->GetFloat() : a_default;
		}

		// The hit landed on our stand-in in a friend's game, where nothing knew our difficulty or
		// that we're in VATS. Scale it the way our own game scales damage to the player.
		float DamageTakenMult(RE::PlayerCharacter* a_player, bool a_byPlayer)
		{
			static constexpr std::array<const char*, 7> DIFFICULTY{ "fDiffMultHPToPCVE", "fDiffMultHPToPCE", "fDiffMultHPToPCN", "fDiffMultHPToPCH",
				"fDiffMultHPToPCVH", "fDiffMultHPToPCSV", "fDiffMultHPToPCTSV" };
			float mult = 1.0f;
			// Friendly fire was already scaled by the shooter's difficulty.
			if (!a_byPlayer) {
				const auto level = std::clamp(static_cast<int>(a_player->GetDifficultyLevel()), 0, static_cast<int>(DIFFICULTY.size()) - 1);
				mult *= GameSettingFloat(DIFFICULTY[level], 1.0f);
			}
			const auto ui = RE::UI::GetSingleton();
			const auto vats = RE::VATS::GetSingleton();
			if (ui && ui->GetMenuOpen("VATSMenu"sv)) {
				mult *= GameSettingFloat("fVATSPlayerMenuDamageMult", 1.0f);
			} else if (vats && vats->mode.any(RE::VATS::VATS_MODE_ENUM::kPlayback)) {
				mult *= GameSettingFloat("fVATSPlayerDamageMult", 0.1f);
			}
			return mult;
		}

		void ApplyDamageToPlayer(const Protocol::PlayerHit& a_hit)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto values = RE::ActorValue::GetSingleton();
			if (!WorldSync::InWorld() || !player || !values || !values->health || player->IsDead(false) || a_hit.damage <= 0.0f) {
				return;
			}
			if (GamePaused() || Story::InOpening() || (!a_hit.byPlayer && !AttackerHere(player, a_hit.attacker))) {
				++hitsIgnored;
				return;
			}
			++hitsTaken;
			lastHitMult = DamageTakenMult(player, a_hit.byPlayer);
			const float damage = (std::min)(a_hit.damage * lastHitMult, 1000.0f);
			static_cast<RE::ActorValueOwner&>(*player).ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *values->health, -damage);
		}

		void HandlePacket(std::span<const std::uint8_t> a_data)
		{
			using Protocol::MessageType;
			switch (Protocol::PeekType(a_data).value_or(MessageType{})) {
			case MessageType::kWelcome:
				if (const auto msg = Protocol::DecodeWelcome(a_data)) {
					welcomed = true;
					sentEquipment.reset();
					localId = msg->playerId;
					NpcSync::SetLocalPlayer(localId);
					Party::SetLocalPlayer(localId);
					WorldSync::OnWelcome(msg->sessionId, localId, msg->friendlyFire);
					Story::SetShared(msg->sharedStory);
					lastRejectReason.clear();
					Notify(std::format("Multiplayer: connected ({}{})", msg->sharedStory ? "shared story" : "everyone has their own story",
						msg->friendlyFire ? ", friendly fire is on" : ""));
				}
				break;
			case MessageType::kReject:
				if (const auto msg = Protocol::DecodeReject(a_data)) {
					lastRejectReason = msg->reason;
					nextConnectAttempt = Clock::now() + REJECTED_RETRY_DELAY;
					Notify("Multiplayer: " + msg->reason);
				}
				break;
			case MessageType::kPlayerJoined:
				if (const auto msg = Protocol::DecodePlayerJoined(a_data)) {
					RemotePlayers::Add(msg->playerId, msg->name, msg->appearance);
					Notify(msg->name + " joined");
				}
				break;
			case MessageType::kPlayerLeft:
				if (const auto msg = Protocol::DecodePlayerLeft(a_data)) {
					const auto name = RemotePlayers::NameOf(msg->playerId);
					RemotePlayers::Remove(msg->playerId);
					Party::OnPlayerLeft(msg->playerId);
					Voice::OnPlayerLeft(msg->playerId);
					Notify(name.empty() ? "A player left" : name + " left");
				}
				break;
			case MessageType::kActorDied:
				if (const auto msg = Protocol::DecodeActorDeath(a_data)) {
					Party::OnActorDied(*msg);  // before the kill: the victim's name is still readable either way
					WorldSync::ApplyRemoteDeath(msg->refId);
				}
				break;
			case MessageType::kActorHealth:
				if (const auto msg = Protocol::DecodeActorHealth(a_data)) {
					WorldSync::ApplyRemoteHealth(msg->refId, msg->health);
				}
				break;
			case MessageType::kContainerChanged:
				if (const auto msg = Protocol::DecodeIndexedContainerChange(a_data)) {
					WorldSync::ApplyContainerChange(*msg);
				}
				break;
			case MessageType::kRefPickedUp:
				if (const auto msg = Protocol::DecodeRefPickedUp(a_data)) {
					WorldSync::ApplyRemotePickup(msg->refId);
				}
				break;
			case MessageType::kQuestStage:
				if (const auto msg = Protocol::DecodeQuestStage(a_data)) {
					QuestSync::Apply(*msg);
				}
				break;
			case MessageType::kActorOwners:
				if (const auto msg = Protocol::DecodeOwners(a_data)) {
					NpcSync::ApplyOwners(*msg);
				}
				break;
			case MessageType::kActorStatesRelay:
				if (const auto msg = Protocol::DecodeActorStates(a_data)) {
					NpcSync::ApplyStates(*msg);
				}
				break;
			case MessageType::kPlayerDamaged:
				if (const auto msg = Protocol::DecodePlayerHit(a_data)) {
					ApplyDamageToPlayer(*msg);
				}
				break;
			case MessageType::kPlayerStatus:
				if (const auto msg = Protocol::DecodePlayerStatus(a_data)) {
					Party::ApplyStatus(*msg);
				}
				break;
			case MessageType::kWorldTime:
				if (const auto msg = Protocol::DecodeWorldTime(a_data)) {
					WorldClock::Apply(*msg);
				}
				break;
			case MessageType::kPinged:
				if (const auto msg = Protocol::DecodePing(a_data)) {
					Party::ApplyPing(*msg);
				}
				break;
			case MessageType::kPartyXp:
				if (const auto msg = Protocol::DecodeXpGain(a_data)) {
					Party::ApplyXp(*msg);
				}
				break;
			case MessageType::kMarkersFound:
				if (const auto msg = Protocol::DecodeMarkers(a_data)) {
					MapShare::Apply(*msg);
				}
				break;
			case MessageType::kLineSpoken:
				if (const auto msg = Protocol::DecodeLine(a_data)) {
					Conversations::Apply(*msg);
				}
				break;
			case MessageType::kVoiceRelay:
				if (const auto msg = Protocol::DecodeVoice(a_data)) {
					Voice::Apply(*msg);
				}
				break;
			case MessageType::kRevived:
				if (const auto msg = Protocol::DecodeRevive(a_data)) {
					Downed::ApplyRevive(*msg);
				}
				break;
			case MessageType::kQuestDone:
				if (const auto msg = Protocol::DecodeQuestDone(a_data)) {
					QuestSync::ApplyDone(*msg);
				}
				break;
			case MessageType::kHeartbeatAck:
				if (const auto msg = Protocol::DecodeHeartbeat(a_data)) {
					Party::ApplyHeartbeatAck(*msg);
				}
				break;
			case MessageType::kShotFired:
				if (const auto msg = Protocol::DecodeShot(a_data)) {
					if (msg->refId == 0) {
						RemotePlayers::PlayShot(msg->playerId);
					} else {
						NpcSync::ApplyShot(msg->refId);
					}
				}
				break;
			case MessageType::kPlayerEquipment:
				if (auto msg = Protocol::DecodeEquipment(a_data)) {
					RemotePlayers::SetEquipment(msg->playerId, std::move(msg->items));
				}
				break;
			case MessageType::kRefStateChanged:
				if (const auto msg = Protocol::DecodeRefState(a_data)) {
					WorldSync::ApplyRemoteRefState(*msg);
				}
				break;
			case MessageType::kWorldState:
				if (const auto msg = Protocol::DecodeWorldState(a_data)) {
					WorldSync::ApplyWorldState(*msg);
				}
				break;
			case MessageType::kPlayerStates:
				if (const auto msg = Protocol::DecodePlayerStates(a_data)) {
					for (const auto& entry : msg->players) {
						if (entry.playerId != localId) {
							RemotePlayers::PushState(entry.playerId, entry.state);
						}
					}
				}
				break;
			default:
				break;
			}
		}

		std::optional<Protocol::PlayerState> SampleLocalState(Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return std::nullopt;
			}

			const auto& pos = player->data.location;
			float       speed = 0.0f;
			if (lastPositionTime != Clock::time_point{}) {
				const float dt = std::chrono::duration<float>(a_now - lastPositionTime).count();
				if (dt > 0.0f) {
					const float dx = pos.x - lastPosition.x;
					const float dy = pos.y - lastPosition.y;
					speed = std::sqrt(dx * dx + dy * dy) / dt;
				}
			}
			lastPosition = pos;
			lastPositionTime = a_now;

			Protocol::PlayerState state;
			state.sequence = ++sequence;
			state.cell = cell->IsInterior() ? cell->GetFormID() : 0;
			state.worldspace = !cell->IsInterior() && cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
			state.x = pos.x;
			state.y = pos.y;
			state.z = pos.z;
			state.heading = player->data.angle.z;
			state.speed = speed;
			state.moveMode = static_cast<std::uint16_t>(static_cast<const RE::ActorState&>(*player).moveMode);
			// A downed player's stand-in crouches.
			if (player->IsSneaking() || Downed::IsDown()) {
				state.flags |= Protocol::kSneaking;
			}
			if (player->GetWeaponMagicDrawn()) {
				state.flags |= Protocol::kWeaponDrawn;
			}
			using CharacterState = RE::IMovementState::CHARACTER_STATE;
			switch (static_cast<const RE::IMovementState&>(*player).DoGetCharacterState()) {
			case CharacterState::kJumping:
				state.flags |= Protocol::kInAir | Protocol::kJumping;
				break;
			case CharacterState::kInAir:
				state.flags |= Protocol::kInAir;
				break;
			case CharacterState::kSwimming:
				state.flags |= Protocol::kSwimming;
				break;
			default:
				break;
			}
			return state;
		}

		// A settler of the same sex as our character, unless the ini picks someone.
		std::uint32_t DefaultAppearance()
		{
			constexpr std::uint32_t FEMALE_SETTLER = 0x0020A578;
			constexpr std::uint32_t MALE_SETTLER = 0x0020A57B;
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto npc = player ? player->GetNPC() : nullptr;
			return npc && npc->IsFemale() ? FEMALE_SETTLER : MALE_SETTLER;
		}

		// Echo test mode (see SetEcho).
		constexpr std::uint32_t ECHO_ID = 0xFFFFFFFF;
		bool                    echo = false;
		float                   echoOffsetX = 0.0f;
		float                   echoOffsetY = 0.0f;

		void OnDisconnected()
		{
			if (welcomed) {
				Notify("Multiplayer: disconnected");
			}
			welcomed = false;
			localId = 0;
			RemotePlayers::RemoveAll();
			WorldSync::Reset();
			NpcSync::Reset();
			Party::Reset();
			WorldClock::Reset();
			Voice::Reset();
			MapShare::Reset();
			Conversations::Reset();
			if (echo) {
				RemotePlayers::Add(ECHO_ID, "Echo", DefaultAppearance());
				sentEquipment.reset();
			}
		}

		ClientConnection& ConnectionFor(const std::string& a_address)
		{
			if (SteamTransport::ParseAddress(a_address)) {
				return steamClient;
			}
			return udpClient;
		}

		// Leaves the current session (if any) and joins another.
		void SwitchServer(std::string a_address)
		{
			client->Disconnect();
			client->Poll();
			OnDisconnected();
			serverAddress = std::move(a_address);
			client = &ConnectionFor(serverAddress);
			nextConnectAttempt = {};
			lastRejectReason.clear();
		}

		// A Steam invite or "Join Game" was accepted.
		void HandleSteamJoin()
		{
			const auto target = Steam::TakeJoinTarget();
			if (!target) {
				return;
			}
			if (hostedServer) {
				Notify("Multiplayer: you're hosting. Set bHost = false to join friends.");
				return;
			}
			const auto address = SteamTransport::FormatAddress(*target);
			if (address != serverAddress) {
				Notify("Multiplayer: joining your friend's game");
				SwitchServer(address);
			}
		}
	}

	void Initialize()
	{
		contentHash = ComputeContentHash();
		WorldSync::Install();
		WeaponFire::Install();
		REX::INFO("Session: load order hash {:08X}", contentHash);

		const auto& settings = Config::Get();
		const bool  wantSteam = settings.transport == "steam" || settings.transport == "auto";
		steamMode = wantSteam && Steam::Available();
		if (settings.transport == "steam" && !steamMode) {
			Notify("Multiplayer: Steam isn't available");
		}
		REX::INFO("Session: transport '{}' -> {}", settings.transport, steamMode ? "steam" : "udp");

		serverAddress = settings.host ? std::format("127.0.0.1:{}", settings.port) : settings.serverAddress;
		client = &ConnectionFor(serverAddress);

		if (settings.host) {
			hostedServer = std::make_unique<Server>([](std::string_view a_line) {
				REX::INFO("{}", a_line);
			});
			Server::Options options;
			options.port = settings.port;
			options.maxPlayers = settings.maxPlayers;
			options.password = settings.password;
			// With Steam, friends never connect over UDP, so only our own game may (no firewall prompt).
			options.udpLoopbackOnly = steamMode;
			options.friendlyFire = settings.friendlyFire;
			options.sharedStory = settings.sharedStory;
			std::vector<std::unique_ptr<ServerTransport>> extra;
			if (steamMode) {
				extra.push_back(std::make_unique<SteamServerTransport>(settings.maxPlayers + 2));
			}
			if (hostedServer->Start(options, std::move(extra))) {
				if (steamMode) {
					Steam::StartHosting(settings.maxPlayers);
				}
			} else {
				Notify(std::format("Multiplayer: could not host on port {}", settings.port));
				hostedServer.reset();
			}
		}
	}

	void Frame()
	{
		const auto now = Clock::now();
		WeaponFire::Install();  // no-op once hooked; the player may not exist at startup
		Story::Frame();  // first: the rest of the session checks it

		if (steamMode) {
			Steam::Frame();
			HandleSteamJoin();
		}

		// Events first: reconnecting must not happen before a disconnect has been handled.
		for (auto& event : client->Poll()) {
			switch (event.type) {
			case ClientConnection::Event::Type::kConnected:
				{
					const auto& settings = Config::Get();
					Protocol::Hello hello;
					hello.contentHash = contentHash;
					hello.name = PlayerName();
					hello.password = settings.password;
					hello.appearance = settings.myAppearance ? settings.myAppearance : DefaultAppearance();
					hello.world = WorldSync::ResyncPoint();
					hello.identity = Identity::Get();
					client->Send(Protocol::Encode(hello), true);
				}
				break;
			case ClientConnection::Event::Type::kDisconnected:
				OnDisconnected();
				break;
			case ClientConnection::Event::Type::kPacket:
				HandlePacket(event.data);
				break;
			}
		}

		if (!serverAddress.empty() && client->GetStatus() == ClientConnection::Status::kDisconnected && now >= nextConnectAttempt && InGame()) {
			nextConnectAttempt = now + RECONNECT_DELAY;
			client->Connect(serverAddress, Config::Get().port);
		}

		if ((welcomed || echo) && now >= nextSend) {
			// On schedule rather than "a full interval after this frame": at 60 fps that would
			// often wait an extra frame and send at 20 Hz.
			nextSend += SEND_INTERVAL;
			if (nextSend <= now) {
				nextSend = now + SEND_INTERVAL;  // fell behind (a hitch or the first send)
			}
			if (auto state = SampleLocalState(now)) {
				if (welcomed) {
					client->Send(Protocol::Encode(*state), false);
				}
				if (echo) {
					state->x += echoOffsetX;
					state->y += echoOffsetY;
					RemotePlayers::PushState(ECHO_ID, *state);
				}
			}
		}

		// Shots: ours and those of NPCs we run. Unreliable: a late shot is worse than a lost one.
		const auto shots = WeaponFire::TakeShots();
		const auto npcShots = WeaponFire::TakeNpcShots();
		if (welcomed) {
			for (std::uint32_t i = 0; i < shots; ++i) {
				client->Send(Protocol::Encode(Protocol::Shot{}, Protocol::MessageType::kReportShot), false);
			}
			for (const auto id : npcShots) {
				if (NpcSync::RunsLocally(id)) {
					client->Send(Protocol::Encode(Protocol::Shot{ 0, id }, Protocol::MessageType::kReportShot), false);
				}
			}
		}
		if (echo) {
			for (std::uint32_t i = 0; i < shots; ++i) {
				RemotePlayers::PlayShot(ECHO_ID);
			}
		}

		if ((welcomed || echo) && now >= nextEquipmentCheck && InGame()) {
			nextEquipmentCheck = now + EQUIPMENT_INTERVAL;
			auto items = Equipment::Read(RE::PlayerCharacter::GetSingleton());
			if (!sentEquipment || *sentEquipment != items) {
				if (welcomed) {
					client->Send(Protocol::Encode(Protocol::Equipment{ 0, items }, Protocol::MessageType::kEquipment), true);
				}
				if (echo) {
					RemotePlayers::SetEquipment(ECHO_ID, items);
				}
				sentEquipment = std::move(items);
			}
		}

		// Events from our world go to the server only while we're in a session, and not from the
		// opening (see Story::InOpening).
		for (auto& packet : WorldSync::TakeOutgoing()) {
			if (welcomed && !Story::InOpening()) {
				client->Send(std::move(packet), true);
			}
		}
		WorldSync::Frame();

		if (welcomed) {
			NpcSync::Frame();
		}
		QuestSync::Frame();
		for (auto& packet : QuestSync::TakeOutgoing()) {
			if (welcomed) {
				client->Send(std::move(packet), true);
			}
		}
		for (auto& packet : NpcSync::TakeOutgoing()) {
			if (welcomed) {
				client->Send(std::move(packet.data), packet.reliable);
			}
		}

		Party::Frame();
		if (welcomed) {
			WorldClock::Frame();
		}
		for (auto& packet : Party::TakeOutgoing()) {
			if (welcomed) {
				client->Send(std::move(packet), true);
			}
		}
		for (auto& packet : WorldClock::TakeOutgoing()) {
			if (welcomed) {
				client->Send(std::move(packet), false);
			}
		}
		if (welcomed) {
			MapShare::Frame();
		}
		for (auto& packet : MapShare::TakeOutgoing()) {
			if (welcomed) {
				client->Send(std::move(packet), true);
			}
		}
		Conversations::Frame(welcomed);
		for (auto& packet : Conversations::TakeOutgoing()) {
			if (welcomed) {
				client->Send(std::move(packet), true);
			}
		}
		Voice::Frame(welcomed);
		for (auto& packet : Voice::TakeOutgoing()) {
			if (welcomed) {
				client->Send(std::move(packet), false);
			}
		}

		RemotePlayers::Update();
		WeaponFire::Frame();
		Compass::Frame();
		Puppets::Tick();
	}

	void OnBeforeSaveOrLoad()
	{
		RemotePlayers::DespawnAll();
		NpcSync::ReleaseMirrors();
		Downed::OnBeforeSave();
	}

	void OnGameLoaded()
	{
		// What we knew about the world described the game before the load.
		WorldSync::Reset();
		QuestSync::Rebaseline();
		MapShare::Rebaseline();
		Downed::OnGameLoaded();
		Party::OnGameLoaded();
		WorldClock::Rebaseline();
		if (welcomed) {
			client->Send(Protocol::Encode(WorldSync::ResyncPoint()), true);
			REX::INFO("Session: save loaded; asking for the session's changes since #{}", WorldSync::ResyncPoint().containerFrom);
		}
	}

	std::string Describe()
	{
		const char* status = "disconnected";
		switch (client->GetStatus()) {
		case ClientConnection::Status::kConnecting:
			status = "connecting";
			break;
		case ClientConnection::Status::kConnected:
			status = welcomed ? "joined" : "connected";
			break;
		default:
			break;
		}
		return std::format("status={} id={} hosting={} server='{}' hash={:08X} reject='{}' world: {} players: {}",
			       status, localId, hostedServer && hostedServer->Running(), serverAddress, contentHash,
			       lastRejectReason, WorldSync::Describe() + " " + NpcSync::Describe() + " " + QuestSync::Describe() + " " + Story::Describe() + " " + WeaponFire::Describe() + " " + Party::Describe() + " " + WorldClock::Describe() + " " + Voice::Describe() + " " + MapShare::Describe() + " " + Conversations::Describe() + " " + Compass::Describe(), RemotePlayers::Describe()) +
		       std::format(" hits: taken={} ignored={} mult={}", hitsTaken, hitsIgnored, lastHitMult) + " " + (steamMode ? Steam::Describe() : "steam=off");
	}

	void ConnectTo(std::string a_address)
	{
		SwitchServer(std::move(a_address));
	}

	void SetEcho(bool a_enabled, float a_offsetX, float a_offsetY)
	{
		echo = a_enabled;
		echoOffsetX = a_offsetX;
		echoOffsetY = a_offsetY;
		if (a_enabled) {
			RemotePlayers::Add(ECHO_ID, "Echo", DefaultAppearance());
			sentEquipment.reset();
		} else {
			RemotePlayers::Remove(ECHO_ID);
		}
	}
}