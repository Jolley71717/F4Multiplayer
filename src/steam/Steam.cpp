#include "steam/Steam.h"

#include "Redact.h"

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
		struct PresenceUpdated
		{
			SteamId       user;
			std::uint32_t appId;
		};
		using Event = std::variant<LobbyEntered, JoinRequested, ConnectRequested, SessionRequested, PresenceUpdated>;

		constexpr std::uint64_t FALLOUT4_APP_ID = 377160;

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
		Callback presenceCallback{ SteamApi::kFriendRichPresenceUpdate, sizeof(SteamApi::FriendRichPresenceUpdate), [](void* a_param) {
									  const auto data = static_cast<const SteamApi::FriendRichPresenceUpdate*>(a_param);
									  Push(PresenceUpdated{ data->friendId, data->appId });
								  } };

		const SteamApi::Api* api = nullptr;
		bool                 setUp = false;
		std::string          lastUnavailable;  // why Steam isn't up, as last logged

		// A friend's invite or Join Game came while we host: refused (see JoinLobby), and the session tells the player.
		bool refusedWhileHosting = false;

		// Whether Steam's overlay is in the game (nullopt: this Steam library can't say).
		std::optional<bool> Overlay()
		{
			if (!api || !api->steamUtils || !api->IsOverlayEnabled) {
				return std::nullopt;
			}
			return api->IsOverlayEnabled(api->steamUtils);
		}

		// The overlay's state for the log, once it matters: hosting has begun, or an invite dialog is asked for. Each
		// state is said once.
		void LogOverlay(std::string_view a_when)
		{
			static std::optional<bool> told;
			const auto                 overlay = Overlay();
			if (!overlay || told == overlay) {
				return;
			}
			told = overlay;
			if (*overlay) {
				REX::INFO("Steam: the overlay is in the game ({})", a_when);
			} else {
				REX::WARN("Steam: the overlay is not in the game ({}): Shift+Tab, Invite to Game and the F1 menu's invite won't open. Friends can still "
						  "right-click you in their Steam friends list and pick Join Game. On Linux, start the game through Steam (INSTALL.md)",
					a_when);
			}
		}

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
				refusedWhileHosting = true;  // the player is told (Session), not only the log
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
				LogOverlay("hosting");
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
			Note(std::format("joined lobby {}; host is {}", lobby, Redact::Alias(owner)));
		}

		void Handle(const JoinRequested& a_event)
		{
			JoinLobby(a_event.lobby);
		}

		std::uint32_t presenceUpdates = 0;

		void Handle(const PresenceUpdated& a_event)
		{
			++presenceUpdates;
			if (a_event.appId == FALLOUT4_APP_ID) {
				REX::INFO("Steam: rich presence from {} arrived", Redact::Alias(a_event.user));
			}
		}

		// The lobby a rich presence "connect" string (see AdvertiseLobby) points at, if it's one of ours.
		std::optional<SteamId> LobbyFromConnect(std::string_view a_connect)
		{
			constexpr auto PREFIX = "+f4mp_lobby "sv;
			const auto     start = a_connect.find(PREFIX);
			if (start == std::string_view::npos) {
				return std::nullopt;
			}
			std::uint64_t id = 0;
			const auto    digits = a_connect.substr(start + PREFIX.size());
			std::from_chars(digits.data(), digits.data() + digits.size(), id);
			return IsLobbyId(id) ? std::optional{ id } : std::nullopt;
		}

		void Handle(const ConnectRequested& a_event)
		{
			if (const auto id = LobbyFromConnect(a_event.connect)) {
				JoinLobby(*id);
			} else {
				Note("ignored a join request that names no lobby of ours");
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
				Note(std::format("{} connection from {}", allowed ? "accepted" : "ignored", Redact::Alias(a_event.user)));
			}
		}

		void RecheckUndecided()
		{
			const auto now = Clock::now();
			std::erase_if(undecided, [&](const auto& a_entry) {
				if (MayConnect(a_entry.first)) {
					Accept(a_entry.first);
					Note(std::format("accepted connection from {}", Redact::Alias(a_entry.first)));
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
				// Said each time the reason changes: no library, the game not starting Steam... Looked at every few seconds
				// at most (finding out asks Steam's library whether Steam runs; this is asked many times a frame).
				static Clock::time_point nextWhy{};
				if (const auto now = Clock::now(); now >= nextWhy) {
					nextWhy = now + 10s;
					if (auto why = SteamApi::Unavailable(); why != lastUnavailable) {
						REX::INFO("Steam: not available yet: {}", why);
						lastUnavailable = std::move(why);
					}
				}
				return false;
			}
			api->RegisterCallback(&lobbyEnterCallback, SteamApi::kLobbyEnter);
			api->RegisterCallback(&lobbyJoinCallback, SteamApi::kGameLobbyJoinRequested);
			api->RegisterCallback(&richPresenceJoinCallback, SteamApi::kGameRichPresenceJoinRequested);
			api->RegisterCallback(&sessionRequestCallback, SteamApi::kSessionRequest);
			api->RegisterCallback(&presenceCallback, SteamApi::kFriendRichPresenceUpdate);
			api->InitRelayNetworkAccess(api->utils);
			setUp = true;
			const auto overlay = Overlay();
			REX::INFO("Steam: ready ({}); app {}, overlay {}{}", Redact::Alias(LocalId()), api->steamUtils ? api->GetAppID(api->steamUtils) : 0,
				!overlay ? "unknown" : *overlay ? "in the game" : "not in the game yet",
				api->steamUtils && api->IsSteamRunningOnSteamDeck && api->IsSteamRunningOnSteamDeck(api->steamUtils) ? ", on a Steam Deck" : "");
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

	std::string Unavailable()
	{
		return Available() ? std::string{} : SteamApi::Unavailable();
	}

	bool TakeRefusedJoin()
	{
		return std::exchange(refusedWhileHosting, false);
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

	std::string NameOf(SteamApi::SteamId a_id)
	{
		const auto name = Available() && a_id != 0 ? api->GetFriendPersonaName(api->friends, a_id) : nullptr;
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
		LogOverlay("an invite dialog was asked for");
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

	void Leave()
	{
		if (Available() && !hosting) {
			LeaveLobby();
		}
	}

	bool LobbyOpen()
	{
		return hosting && lobby != 0;
	}

	namespace
	{
		// What Steam tells us about one friend.
		struct FriendInfo
		{
			SteamId       id = 0;
			std::string   name;
			int           state = 0;   // EPersonaState: 0 = offline
			std::uint64_t gameId = 0;  // 0 = not in a game (or not shared with us)
			SteamId       gameLobby = 0;
			std::string   connect;     // their "connect" rich presence
			SteamId       lobby = 0;   // the lobby they host, from either source
		};

		// Walks the friends list (immediate friends only) while a_each returns true.
		void ForEachFriend(const std::function<bool(const FriendInfo&)>& a_each)
		{
			if (!Available() || !api->GetFriendCount) {
				return;
			}
			const int count = api->GetFriendCount(api->friends, SteamApi::FRIEND_FLAG_IMMEDIATE);
			for (int i = 0; i < count; ++i) {
				FriendInfo info;
				info.id = api->GetFriendByIndex(api->friends, i, SteamApi::FRIEND_FLAG_IMMEDIATE);
				if (info.id == 0 || info.id == LocalId()) {
					continue;
				}
				const char* name = api->GetFriendPersonaName(api->friends, info.id);
				info.name = name && *name ? name : std::format("Friend {}", info.id);
				info.state = api->GetFriendPersonaState(api->friends, info.id);
				SteamApi::FriendGameInfo game{};
				if (api->GetFriendGamePlayed(api->friends, info.id, &game)) {
					info.gameId = game.gameId;
					info.gameLobby = game.lobby;
				}
				// Hosts advertise their lobby in their rich presence (what "Join Game" uses); Steam also
				// knows the lobby a friend in the same game is in, which covers a host whose presence
				// hasn't reached us (yet).
				const char* const connect = api->GetFriendRichPresence(api->friends, info.id, "connect");
				info.connect = connect ? connect : "";
				if (const auto fromPresence = LobbyFromConnect(info.connect)) {
					info.lobby = *fromPresence;
				} else if (info.gameId == FALLOUT4_APP_ID && IsLobbyId(info.gameLobby)) {
					info.lobby = info.gameLobby;
				}
				if (!a_each(info)) {
					return;
				}
			}
		}
	}

	FriendScan ScanFriends()
	{
		constexpr std::size_t MAX_LISTED = 12;  // the menu pages through them two or three at a time
		FriendScan            scan;
		ForEachFriend([&](const FriendInfo& a_friend) {
			++scan.friends;
			scan.online += a_friend.state != 0;
			scan.inGame += a_friend.gameId == FALLOUT4_APP_ID;
			if (a_friend.lobby != 0 && scan.hosting.size() < MAX_LISTED) {
				scan.hosting.push_back({ a_friend.id, a_friend.lobby, a_friend.name });
			}
			// Online friends only, and only with sLogLevel = debug: the join page is the one place this runs.
			// By alias, never by name, and not what else they play: inF4 is what the join page's advice is about.
			if (a_friend.state != 0) {
				REX::DEBUG("Steam: friend {} state={} inF4={} gameLobby={} -> lobby={}", Redact::Alias(a_friend.id), a_friend.state,
					a_friend.gameId == FALLOUT4_APP_ID, a_friend.gameLobby, a_friend.lobby);
			}
			return true;
		});
		// The owner of the lobby we're in (joined through an invite or Join Game) hosts for sure,
		// whatever their presence says; "Join someone else" can still pick them again.
		if (lobby && !hosting && Available()) {
			const auto owner = api->GetLobbyOwner(api->matchmaking, lobby);
			if (owner != 0 && owner != LocalId() && std::ranges::none_of(scan.hosting, [&](const auto& a_host) { return a_host.id == owner; })) {
				auto name = NameOf(owner);
				scan.hosting.insert(scan.hosting.begin(), { owner, lobby, name.empty() ? std::format("Steam user {}", owner) : std::move(name) });
			}
		}
		REX::INFO("Steam: friends scanned: {} total, {} online, {} in Fallout 4, {} hosting", scan.friends, scan.online, scan.inGame, scan.hosting.size());
		return scan;
	}

	std::vector<JoinableFriend> JoinableFriends()
	{
		return ScanFriends().hosting;
	}

	std::size_t RefreshFriends()
	{
		std::size_t asked = 0;
		ForEachFriend([&](const FriendInfo& a_friend) {
			// Offline friends have nothing to tell; everyone else is asked (a host whose game details
			// are private shows as online with no game, so "in Fallout 4" would miss them).
			if (a_friend.state != 0) {
				api->RequestFriendRichPresence(api->friends, a_friend.id);
				++asked;
			}
			return true;
		});
		Note(std::format("asked {} friends for their rich presence", asked));
		return asked;
	}

	std::uint32_t PresenceUpdates()
	{
		return presenceUpdates;
	}

	std::string DescribeFriends()
	{
		if (!Available()) {
			return "steam=unavailable";
		}
		if (!api->GetFriendCount) {
			return "friends list: this Steam library lacks the friends functions";
		}
		// One line: the dev channel's reply is a line, so a second line never reaches the caller.
		// Every friend, raw, without the skips ForEachFriend makes, so a wrong count or flag shows.
		const int   count = api->GetFriendCount(api->friends, SteamApi::FRIEND_FLAG_IMMEDIATE);
		std::string out = std::format("presenceUpdates={} self={} count={} flags={} friends:", presenceUpdates, LocalId(), count, SteamApi::FRIEND_FLAG_IMMEDIATE);
		for (int i = 0; i < count; ++i) {
			const auto               id = api->GetFriendByIndex(api->friends, i, SteamApi::FRIEND_FLAG_IMMEDIATE);
			const char* const        name = api->GetFriendPersonaName(api->friends, id);
			const char* const        connect = api->GetFriendRichPresence(api->friends, id, "connect");
			const char* const        status = api->GetFriendRichPresence(api->friends, id, "status");
			SteamApi::FriendGameInfo game{};
			const bool               inGame = api->GetFriendGamePlayed(api->friends, id, &game);
			out += std::format(" | '{}' {} state={} inGame={} app={} gameLobby={} connect='{}' status='{}' lobby={}", name ? name : "", id,
				api->GetFriendPersonaState(api->friends, id), inGame, game.gameId, game.lobby, connect ? connect : "", status ? status : "",
				LobbyFromConnect(connect ? connect : "").value_or(game.gameId == FALLOUT4_APP_ID && IsLobbyId(game.lobby) ? game.lobby : 0));
		}
		return out;
	}

	std::string Describe()
	{
		if (!Available()) {
			return "steam=unavailable";
		}
		// Our own rich presence, read back the way friends read it: shows whether the lobby is still
		// advertised (LeaveLobby clears it).
		const char* const ownConnect = api->GetFriendRichPresence ? api->GetFriendRichPresence(api->friends, LocalId(), "connect") : nullptr;
		const char* const ownStatus = api->GetFriendRichPresence ? api->GetFriendRichPresence(api->friends, LocalId(), "status") : nullptr;
		return std::format("steam={} name='{}' relay={} hosting={} lobby={} members={} ownConnect='{}' ownStatus='{}' presenceUpdates={} callbacks={} last='{}'",
			LocalId(), PersonaName(), api->GetRelayNetworkStatus(api->utils, nullptr), hosting, lobby,
			lobby ? api->GetNumLobbyMembers(api->matchmaking, lobby) : 0, ownConnect ? ownConnect : "", ownStatus ? ownStatus : "", presenceUpdates,
			callbacksSeen.load(), lastEvent);
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
