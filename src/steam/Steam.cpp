#include "steam/Steam.h"

namespace Steam
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using SteamApi::SteamId;

		constexpr auto LOBBY_RETRY = 30s;
		constexpr auto LOBBY_KEY = "f4mp";

		// Steam reports callbacks on the thread that runs SteamAPI_RunCallbacks (the game's main
		// thread); they're queued and handled in Frame.
		struct LobbyEntered
		{
			SteamId       lobby;
			std::uint32_t response;
		};
		struct JoinRequested
		{
			SteamId lobby;
		};
		struct ConnectRequested
		{
			std::string connect;
		};
		struct SessionRequested
		{
			SteamId user;
		};
		using Event = std::variant<LobbyEntered, JoinRequested, ConnectRequested, SessionRequested>;

		std::mutex         eventLock;
		std::vector<Event> events;
		std::atomic<int>   callbacksSeen{ 0 };

		class Callback final : public SteamApi::CallbackBase
		{
		public:
			using Handler = void (*)(void*);

			Callback(int a_id, int a_size, Handler a_handler) :
				size(a_size),
				handler(a_handler)
			{
				callbackId = a_id;
			}

			void Run(void* a_param) override { Handle(a_param); }
			void Run(void* a_param, bool, std::uint64_t) override { Handle(a_param); }
			int  GetCallbackSizeBytes() override { return size; }

		private:
			void Handle(void* a_param)
			{
				++callbacksSeen;
				if (a_param) {
					handler(a_param);
				}
			}

			int     size;
			Handler handler;
		};

		void Push(Event a_event)
		{
			std::scoped_lock l{ eventLock };
			events.push_back(std::move(a_event));
		}

		Callback lobbyEnterCallback{ SteamApi::kLobbyEnter, sizeof(SteamApi::LobbyEnter), [](void* a_param) {
										const auto data = static_cast<const SteamApi::LobbyEnter*>(a_param);
										Push(LobbyEntered{ data->lobby, data->response });
									} };
		Callback lobbyJoinCallback{ SteamApi::kGameLobbyJoinRequested, sizeof(SteamApi::GameLobbyJoinRequested), [](void* a_param) {
									   Push(JoinRequested{ static_cast<const SteamApi::GameLobbyJoinRequested*>(a_param)->lobby });
								   } };
		Callback richPresenceJoinCallback{ SteamApi::kGameRichPresenceJoinRequested, sizeof(SteamApi::GameRichPresenceJoinRequested), [](void* a_param) {
											  const auto data = static_cast<const SteamApi::GameRichPresenceJoinRequested*>(a_param);
											  Push(ConnectRequested{ std::string(data->connect, strnlen(data->connect, sizeof(data->connect))) });
										  } };
		Callback sessionRequestCallback{ SteamApi::kSessionRequest, sizeof(SteamApi::SessionRequest), [](void* a_param) {
											Push(SessionRequested{ SteamApi::SteamIdOf(static_cast<const SteamApi::SessionRequest*>(a_param)->remote) });
										} };

		const SteamApi::Api* api = nullptr;
		bool                 setUp = false;

		bool              hosting = false;
		int               maxMembers = 4;
		bool              creatingLobby = false;
		Clock::time_point nextLobbyAttempt{};
		SteamId           lobby = 0;  // the lobby we're in (ours when hosting)
		SteamId           joiningLobby = 0;

		std::optional<SteamId> joinTarget;
		std::string            lastEvent;

		bool IsLobbyId(SteamId a_id) { return ((a_id >> 52) & 0xF) == 8; }  // k_EAccountTypeChat
		bool IsUserId(SteamId a_id) { return ((a_id >> 52) & 0xF) == 1; }   // k_EAccountTypeIndividual

		void Note(std::string a_text)
		{
			REX::INFO("Steam: {}", a_text);
			lastEvent = std::move(a_text);
		}

		void LeaveLobby()
		{
			if (lobby) {
				api->LeaveLobby(api->matchmaking, lobby);
				lobby = 0;
			}
			api->ClearRichPresence(api->friends);
		}

		// Our friends see "Join Game" for the lobby we're in.
		void AdvertiseLobby()
		{
			const auto connect = std::format("+f4mp_lobby {}", lobby);
			api->SetRichPresence(api->friends, "connect", connect.c_str());
			api->SetRichPresence(api->friends, "status", hosting ? "Hosting Fallout 4 co-op" : "Playing Fallout 4 co-op");
		}

		bool IsLobbyMember(SteamId a_user)
		{
			if (!lobby) {
				return false;
			}
			const int count = api->GetNumLobbyMembers(api->matchmaking, lobby);
			for (int i = 0; i < count; ++i) {
				if (api->GetLobbyMemberByIndex(api->matchmaking, lobby, i) == a_user) {
					return true;
				}
			}
			return false;
		}

		void JoinLobby(SteamId a_lobby)
		{
			if (a_lobby == lobby) {
				return;
			}
			if (hosting) {
				Note("can't join another game while hosting (set bHost = false to join friends)");
				return;
			}
			joiningLobby = a_lobby;
			api->JoinLobby(api->matchmaking, a_lobby);
			Note(std::format("joining lobby {}", a_lobby));
		}

		void Handle(const LobbyEntered& a_event)
		{
			if (a_event.response != 1) {
				Note(std::format("could not enter lobby {} (response {})", a_event.lobby, a_event.response));
				if (creatingLobby) {
					creatingLobby = false;
					nextLobbyAttempt = Clock::now() + LOBBY_RETRY;
				}
				joiningLobby = 0;
				return;
			}

			if (creatingLobby) {
				creatingLobby = false;
				if (!hosting) {
					api->LeaveLobby(api->matchmaking, a_event.lobby);
					return;
				}
				lobby = a_event.lobby;
				api->SetLobbyData(api->matchmaking, lobby, LOBBY_KEY, "1");
				AdvertiseLobby();
				Note(std::format("hosting lobby {}", lobby));
				return;
			}

			if (a_event.lobby != joiningLobby) {
				return;
			}
			joiningLobby = 0;

			const auto owner = api->GetLobbyOwner(api->matchmaking, a_event.lobby);
			const auto tag = api->GetLobbyData(api->matchmaking, a_event.lobby, LOBBY_KEY);
			if (!tag || !*tag || owner == 0 || owner == LocalId()) {
				Note(std::format("lobby {} is not a Fallout 4 co-op game", a_event.lobby));
				api->LeaveLobby(api->matchmaking, a_event.lobby);
				return;
			}

			if (lobby) {
				api->LeaveLobby(api->matchmaking, lobby);
			}
			lobby = a_event.lobby;
			AdvertiseLobby();
			joinTarget = owner;
			Note(std::format("joined lobby {}; host is {}", lobby, owner));
		}

		void Handle(const JoinRequested& a_event)
		{
			JoinLobby(a_event.lobby);
		}

		void Handle(const ConnectRequested& a_event)
		{
			constexpr auto PREFIX = "+f4mp_lobby "sv;
			const auto     start = a_event.connect.find(PREFIX);
			if (start == std::string::npos) {
				Note(std::format("ignored join request '{}'", a_event.connect));
				return;
			}
			std::uint64_t id = 0;
			const auto    digits = std::string_view{ a_event.connect }.substr(start + PREFIX.size());
			std::from_chars(digits.data(), digits.data() + digits.size(), id);
			if (IsLobbyId(id)) {
				JoinLobby(id);
			}
		}

		// Requests from players who may still be entering our lobby get another look for a while.
		constexpr auto                                 RECHECK_WINDOW = 10s;
		std::unordered_map<SteamId, Clock::time_point> undecided;

		bool MayConnect(SteamId a_user)
		{
			return hosting && a_user != 0 &&
			       (IsLobbyMember(a_user) || api->GetFriendRelationship(api->friends, a_user) == SteamApi::FRIEND_RELATIONSHIP_FRIEND);
		}

		void Accept(SteamId a_user)
		{
			const auto identity = SteamApi::IdentityOf(a_user);
			api->AcceptSessionWithUser(api->messages, &identity);
		}

		void Handle(const SessionRequested& a_event)
		{
			// Only the host accepts connections: from players in its lobby and from Steam friends.
			// Players connecting to a host accept the host's replies implicitly by sending first.
			const bool allowed = MayConnect(a_event.user);
			if (allowed) {
				Accept(a_event.user);
				undecided.erase(a_event.user);
			} else if (hosting && a_event.user != 0) {
				undecided.try_emplace(a_event.user, Clock::now() + RECHECK_WINDOW);
			}
			// Requests repeat (for every message while connecting to ourselves); report each user once.
			static std::set<std::pair<SteamId, bool>> reported;
			if (reported.insert({ a_event.user, allowed }).second) {
				Note(std::format("{} connection from {}", allowed ? "accepted" : "ignored", a_event.user));
			}
		}

		void RecheckUndecided()
		{
			const auto now = Clock::now();
			std::erase_if(undecided, [&](const auto& a_entry) {
				if (MayConnect(a_entry.first)) {
					Accept(a_entry.first);
					Note(std::format("accepted connection from {}", a_entry.first));
					return true;
				}
				return now >= a_entry.second;
			});
		}

		bool SetUp()
		{
			if (setUp) {
				return true;
			}
			api = SteamApi::Get();
			if (!api) {
				return false;
			}
			api->RegisterCallback(&lobbyEnterCallback, SteamApi::kLobbyEnter);
			api->RegisterCallback(&lobbyJoinCallback, SteamApi::kGameLobbyJoinRequested);
			api->RegisterCallback(&richPresenceJoinCallback, SteamApi::kGameRichPresenceJoinRequested);
			api->RegisterCallback(&sessionRequestCallback, SteamApi::kSessionRequest);
			api->InitRelayNetworkAccess(api->utils);
			setUp = true;
			REX::INFO("Steam: ready as {} ({})", PersonaName(), LocalId());
			return true;
		}
	}

	void Frame()
	{
		if (!SetUp()) {
			return;
		}

		std::vector<Event> pending;
		{
			std::scoped_lock l{ eventLock };
			pending.swap(events);
		}
		for (const auto& event : pending) {
			std::visit([](const auto& a_event) { Handle(a_event); }, event);
		}
		RecheckUndecided();

		if (hosting && !lobby && !creatingLobby && Clock::now() >= nextLobbyAttempt) {
			creatingLobby = true;
			api->CreateLobby(api->matchmaking, SteamApi::LOBBY_FRIENDS_ONLY, maxMembers);
		}
	}

	bool Available()
	{
		return SetUp();
	}

	SteamApi::SteamId LocalId()
	{
		return Available() ? api->GetSteamID(api->user) : 0;
	}

	std::string PersonaName()
	{
		const auto name = Available() ? api->GetPersonaName(api->friends) : nullptr;
		return name ? name : "";
	}

	void StartHosting(std::size_t a_maxPlayers)
	{
		hosting = true;
		maxMembers = static_cast<int>(std::clamp<std::size_t>(a_maxPlayers, 2, 250));
		nextLobbyAttempt = {};
		if (Available() && lobby) {
			// We were in someone else's lobby.
			LeaveLobby();
		}
	}

	void StopHosting()
	{
		hosting = false;
		if (Available()) {
			LeaveLobby();
		}
	}

	std::optional<SteamApi::SteamId> TakeJoinTarget()
	{
		return std::exchange(joinTarget, std::nullopt);
	}

	bool OpenInviteDialog()
	{
		if (!Available() || !lobby) {
			return false;
		}
		api->ActivateGameOverlayInviteDialog(api->friends, lobby);
		return true;
	}

	void Join(SteamApi::SteamId a_id)
	{
		if (!Available()) {
			return;
		}
		if (IsLobbyId(a_id)) {
			JoinLobby(a_id);
		} else if (IsUserId(a_id) && a_id != LocalId()) {
			joinTarget = a_id;
		}
	}

	std::string Describe()
	{
		if (!Available()) {
			return "steam=unavailable";
		}
		return std::format("steam={} name='{}' relay={} hosting={} lobby={} members={} callbacks={} last='{}'",
			LocalId(), PersonaName(), api->GetRelayNetworkStatus(api->utils, nullptr), hosting, lobby,
			lobby ? api->GetNumLobbyMembers(api->matchmaking, lobby) : 0, callbacksSeen.load(), lastEvent);
	}
}

namespace Steam
{
	std::string SelfTest(bool a_send)
	{
		if (!Available()) {
			return "error: Steam unavailable";
		}
		constexpr int CHANNEL = 9;
		const auto    identity = SteamApi::IdentityOf(LocalId());
		if (a_send) {
			constexpr char TEXT[] = "f4mp self test";
			const int      result = api->SendMessageToUser(api->messages, &identity, TEXT, sizeof(TEXT), SteamApi::SEND_RELIABLE, CHANNEL);
			return std::format("send result {}", result);
		}
		SteamApi::NetworkingMessage* messages[8];
		const int                    count = api->ReceiveMessagesOnChannel(api->messages, CHANNEL, messages, 8);
		std::string                  out = std::format("received {}", count);
		for (int i = 0; i < count; ++i) {
			out += std::format(" [{} bytes from {}]", messages[i]->size, SteamApi::SteamIdOf(messages[i]->peer));
			api->ReleaseMessage(messages[i]);
		}
		return out;
	}
}
