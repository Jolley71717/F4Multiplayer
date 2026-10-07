#pragma once

// The parts of the Steamworks flat API we use, resolved at runtime from the game's own
// steam_api64.dll (we ship no Steam files). Types are declared here to match the Steamworks ABI
// (8-byte packing on Windows).
namespace SteamApi
{
	using SteamId = std::uint64_t;

#pragma pack(push, 8)
	struct NetworkingIdentity
	{
		std::int32_t type = 0;
		std::int32_t size = 0;
		union
		{
			std::uint64_t steamId64;
			char          raw[128];
		};
	};
	static_assert(sizeof(NetworkingIdentity) == 136);

	struct NetworkingMessage
	{
		void*              data;
		std::int32_t       size;
		std::uint32_t      connection;
		NetworkingIdentity peer;
		std::int64_t       connectionUserData;
		std::int64_t       timeReceived;
		std::int64_t       messageNumber;
		void*              freeData;
		void*              release;
		std::int32_t       channel;
		std::int32_t       flags;
		std::int64_t       userData;
		std::uint16_t      lane;
	};
	static_assert(offsetof(NetworkingMessage, channel) == 192);

	// Callback payloads.
	struct LobbyEnter  // 504
	{
		std::uint64_t lobby;
		std::uint32_t chatPermissions;
		bool          locked;
		std::uint32_t response;  // 1 = success
	};
	static_assert(sizeof(LobbyEnter) == 24);

	struct LobbyChatUpdate  // 506
	{
		std::uint64_t lobby;
		std::uint64_t userChanged;
		std::uint64_t makingChange;
		std::uint32_t stateChange;
	};

	struct GameLobbyJoinRequested  // 333
	{
		std::uint64_t lobby;
		std::uint64_t friendId;
	};

	struct GameRichPresenceJoinRequested  // 337
	{
		std::uint64_t friendId;
		char          connect[256];
	};

	struct SessionRequest  // 1251
	{
		NetworkingIdentity remote;
	};
#pragma pack(pop)

	// Matches CCallbackBase. Both Run overloads do the same thing, so their vtable order doesn't matter.
	class CallbackBase
	{
	public:
		virtual void Run(void* a_param) = 0;
		virtual void Run(void* a_param, bool a_ioFailure, std::uint64_t a_call) = 0;
		virtual int  GetCallbackSizeBytes() = 0;

	protected:
		std::uint8_t flags = 0;
		int          callbackId = 0;
	};

	enum Callback : int
	{
		kGameLobbyJoinRequested = 333,
		kGameRichPresenceJoinRequested = 337,
		kLobbyEnter = 504,
		kLobbyChatUpdate = 506,
		kSessionRequest = 1251,
	};

	// Send flags.
	constexpr int SEND_UNRELIABLE = 0;
	constexpr int SEND_NO_NAGLE = 1;
	constexpr int SEND_RELIABLE = 8;
	constexpr int SEND_AUTO_RESTART_BROKEN_SESSION = 32;

	constexpr int LOBBY_FRIENDS_ONLY = 1;
	constexpr int FRIEND_RELATIONSHIP_FRIEND = 3;
	constexpr int RELAY_AVAILABLE = 100;

	struct Api
	{
		void* user = nullptr;
		void* friends = nullptr;
		void* matchmaking = nullptr;
		void* messages = nullptr;
		void* utils = nullptr;

		SteamId (*GetSteamID)(void*) = nullptr;
		const char* (*GetPersonaName)(void*) = nullptr;
		const char* (*GetFriendPersonaName)(void*, SteamId) = nullptr;
		int (*GetFriendRelationship)(void*, SteamId) = nullptr;
		bool (*SetRichPresence)(void*, const char*, const char*) = nullptr;
		void (*ClearRichPresence)(void*) = nullptr;
		void (*ActivateGameOverlayInviteDialog)(void*, SteamId) = nullptr;

		std::uint64_t (*CreateLobby)(void*, int, int) = nullptr;
		std::uint64_t (*JoinLobby)(void*, SteamId) = nullptr;
		void (*LeaveLobby)(void*, SteamId) = nullptr;
		bool (*SetLobbyData)(void*, SteamId, const char*, const char*) = nullptr;
		const char* (*GetLobbyData)(void*, SteamId, const char*) = nullptr;
		SteamId (*GetLobbyOwner)(void*, SteamId) = nullptr;
		int (*GetNumLobbyMembers)(void*, SteamId) = nullptr;
		SteamId (*GetLobbyMemberByIndex)(void*, SteamId, int) = nullptr;

		int (*SendMessageToUser)(void*, const NetworkingIdentity*, const void*, std::uint32_t, int, int) = nullptr;
		int (*ReceiveMessagesOnChannel)(void*, int, NetworkingMessage**, int) = nullptr;
		bool (*AcceptSessionWithUser)(void*, const NetworkingIdentity*) = nullptr;
		bool (*CloseSessionWithUser)(void*, const NetworkingIdentity*) = nullptr;
		void (*ReleaseMessage)(NetworkingMessage*) = nullptr;

		void (*InitRelayNetworkAccess)(void*) = nullptr;
		int (*GetRelayNetworkStatus)(void*, void*) = nullptr;

		void (*RegisterCallback)(CallbackBase*, int) = nullptr;
		void (*UnregisterCallback)(CallbackBase*) = nullptr;

		// Voice (optional: nullptr if this Steam library lacks them). Results are EVoiceResult.
		void (*StartVoiceRecording)(void*) = nullptr;
		void (*StopVoiceRecording)(void*) = nullptr;
		int (*GetAvailableVoice)(void*, std::uint32_t*, std::uint32_t*, std::uint32_t) = nullptr;
		int (*GetVoice)(void*, bool, void*, std::uint32_t, std::uint32_t*, bool, void*, std::uint32_t, std::uint32_t*, std::uint32_t) = nullptr;
		int (*DecompressVoice)(void*, const void*, std::uint32_t, void*, std::uint32_t, std::uint32_t*, std::uint32_t) = nullptr;
		void (*SetInGameVoiceSpeaking)(void*, SteamId, bool) = nullptr;
	};

	constexpr int VOICE_OK = 0;
	constexpr int VOICE_NO_DATA = 3;

	// Resolves everything once Steam is running in the game. Returns nullptr until then (or if this
	// game's Steam library lacks something we need). Call from the main thread; the returned table
	// never changes afterwards, and the networking functions may then be used from any thread.
	const Api* Get();

	// The resolved table, or nullptr if Get() hasn't succeeded yet. Safe from any thread.
	const Api* Loaded();

	[[nodiscard]] inline NetworkingIdentity IdentityOf(SteamId a_id)
	{
		NetworkingIdentity identity;
		identity.type = 16;  // k_ESteamNetworkingIdentityType_SteamID
		identity.size = sizeof(std::uint64_t);
		identity.steamId64 = a_id;
		return identity;
	}

	[[nodiscard]] inline SteamId SteamIdOf(const NetworkingIdentity& a_identity)
	{
		return a_identity.type == 16 ? a_identity.steamId64 : 0;
	}
}
