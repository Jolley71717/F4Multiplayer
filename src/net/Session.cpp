#include "net/Session.h"

#include "Config.h"
#include "Protocol.h"
#include "Server.h"
#include "game/Puppets.h"
#include "game/RemotePlayers.h"
#include "net/NetClient.h"

namespace Session
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto SEND_INTERVAL = 50ms;  // 20 Hz
		constexpr auto RECONNECT_DELAY = 5s;
		constexpr auto REJECTED_RETRY_DELAY = 60s;

		std::uint32_t           contentHash = 0;
		std::unique_ptr<Server> hostedServer;
		NetClient               client;

		bool              welcomed = false;
		std::uint32_t     localId = 0;
		std::uint32_t     sequence = 0;
		Clock::time_point nextSend{};
		Clock::time_point nextConnectAttempt{};
		std::string       lastRejectReason;

		RE::NiPoint3      lastPosition;
		Clock::time_point lastPositionTime{};

		void Notify(const std::string& a_message)
		{
			REX::INFO("Session: {}", a_message);
			RE::SendHUDMessage::ShowHUDMessage(a_message.c_str(), "", false, false);
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

		std::string ServerAddress()
		{
			const auto& settings = Config::Get();
			if (settings.host) {
				return std::format("127.0.0.1:{}", settings.port);
			}
			return settings.serverAddress;
		}

		bool InGame()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetParentCell();
		}

		void HandlePacket(std::span<const std::uint8_t> a_data)
		{
			using Protocol::MessageType;
			switch (Protocol::PeekType(a_data).value_or(MessageType{})) {
			case MessageType::kWelcome:
				if (const auto msg = Protocol::DecodeWelcome(a_data)) {
					welcomed = true;
					localId = msg->playerId;
					lastRejectReason.clear();
					Notify("Multiplayer: connected");
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
					RemotePlayers::Add(msg->playerId, msg->name);
					Notify(msg->name + " joined");
				}
				break;
			case MessageType::kPlayerLeft:
				if (const auto msg = Protocol::DecodePlayerLeft(a_data)) {
					RemotePlayers::Remove(msg->playerId);
					Notify("A player left");
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

		void SendLocalState(Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			if (!cell) {
				return;
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
			if (player->IsSneaking()) {
				state.flags |= Protocol::kSneaking;
			}

			client.Send(Protocol::Encode(state), false);
		}
	}

	void Initialize()
	{
		contentHash = ComputeContentHash();
		REX::INFO("Session: load order hash {:08X}", contentHash);

		const auto& settings = Config::Get();
		if (settings.host) {
			hostedServer = std::make_unique<Server>([](std::string_view a_line) {
				REX::INFO("{}", a_line);
			});
			Server::Options options;
			options.port = settings.port;
			options.maxPlayers = settings.maxPlayers;
			options.password = settings.password;
			if (!hostedServer->Start(options)) {
				Notify(std::format("Multiplayer: could not host on port {}", settings.port));
				hostedServer.reset();
			}
		}
	}

	void Frame()
	{
		const auto now = Clock::now();
		const auto address = ServerAddress();

		if (!address.empty() && client.GetStatus() == NetClient::Status::kDisconnected && now >= nextConnectAttempt && InGame()) {
			nextConnectAttempt = now + RECONNECT_DELAY;
			client.Connect(address, Protocol::DEFAULT_PORT);
		}

		for (auto& event : client.Poll()) {
			switch (event.type) {
			case NetClient::Event::Type::kConnected:
				{
					const auto& settings = Config::Get();
					Protocol::Hello hello;
					hello.contentHash = contentHash;
					hello.name = settings.playerName;
					hello.password = settings.password;
					client.Send(Protocol::Encode(hello), true);
				}
				break;
			case NetClient::Event::Type::kDisconnected:
				if (welcomed) {
					Notify("Multiplayer: disconnected");
				}
				welcomed = false;
				localId = 0;
				RemotePlayers::RemoveAll();
				break;
			case NetClient::Event::Type::kPacket:
				HandlePacket(event.data);
				break;
			}
		}

		if (welcomed && now >= nextSend) {
			nextSend = now + SEND_INTERVAL;
			SendLocalState(now);
		}

		RemotePlayers::Update();
		Puppets::Tick();
	}

	void OnBeforeSaveOrLoad()
	{
		RemotePlayers::DespawnAll();
	}

	std::string Describe()
	{
		const char* status = "disconnected";
		switch (client.GetStatus()) {
		case NetClient::Status::kConnecting:
			status = "connecting";
			break;
		case NetClient::Status::kConnected:
			status = welcomed ? "joined" : "connected";
			break;
		default:
			break;
		}
		return std::format("status={} id={} hosting={} server='{}' hash={:08X} reject='{}' players: {}",
			status, localId, hostedServer && hostedServer->Running(), ServerAddress(), contentHash,
			lastRejectReason, RemotePlayers::Describe());
	}
}
