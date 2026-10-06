#include "steam/SteamApi.h"

namespace SteamApi
{
	namespace
	{
		Api                     api;
		std::atomic<const Api*> loaded{ nullptr };
		bool                    reportedMissing = false;

		template <class T>
		bool Resolve(REX::W32::HMODULE a_module, const char* a_name, T& a_out)
		{
			a_out = reinterpret_cast<T>(REX::W32::GetProcAddress(a_module, a_name));
			if (!a_out && !reportedMissing) {
				REX::WARN("Steam: steam_api64.dll has no {}", a_name);
			}
			return a_out != nullptr;
		}
	}

	const Api* Get()
	{
		if (const auto table = loaded.load()) {
			return table;
		}

		const auto module = REX::W32::GetModuleHandleW(L"steam_api64.dll");
		if (!module) {
			return nullptr;
		}

		using Accessor = void* (*)();
		Accessor user{}, friends{}, matchmaking{}, messages{}, utils{};

		bool ok = true;
		ok &= Resolve(module, "SteamAPI_SteamUser_v021", user);
		ok &= Resolve(module, "SteamAPI_SteamFriends_v017", friends);
		ok &= Resolve(module, "SteamAPI_SteamMatchmaking_v009", matchmaking);
		ok &= Resolve(module, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002", messages);
		ok &= Resolve(module, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004", utils);

		ok &= Resolve(module, "SteamAPI_ISteamUser_GetSteamID", api.GetSteamID);
		ok &= Resolve(module, "SteamAPI_ISteamFriends_GetPersonaName", api.GetPersonaName);
		ok &= Resolve(module, "SteamAPI_ISteamFriends_GetFriendPersonaName", api.GetFriendPersonaName);
		ok &= Resolve(module, "SteamAPI_ISteamFriends_GetFriendRelationship", api.GetFriendRelationship);
		ok &= Resolve(module, "SteamAPI_ISteamFriends_SetRichPresence", api.SetRichPresence);
		ok &= Resolve(module, "SteamAPI_ISteamFriends_ClearRichPresence", api.ClearRichPresence);
		ok &= Resolve(module, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog", api.ActivateGameOverlayInviteDialog);

		ok &= Resolve(module, "SteamAPI_ISteamMatchmaking_CreateLobby", api.CreateLobby);
		ok &= Resolve(module, "SteamAPI_ISteamMatchmaking_JoinLobby", api.JoinLobby);
		ok &= Resolve(module, "SteamAPI_ISteamMatchmaking_LeaveLobby", api.LeaveLobby);
		ok &= Resolve(module, "SteamAPI_ISteamMatchmaking_SetLobbyData", api.SetLobbyData);
		ok &= Resolve(module, "SteamAPI_ISteamMatchmaking_GetLobbyData", api.GetLobbyData);
		ok &= Resolve(module, "SteamAPI_ISteamMatchmaking_GetLobbyOwner", api.GetLobbyOwner);
		ok &= Resolve(module, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers", api.GetNumLobbyMembers);
		ok &= Resolve(module, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex", api.GetLobbyMemberByIndex);

		ok &= Resolve(module, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser", api.SendMessageToUser);
		ok &= Resolve(module, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel", api.ReceiveMessagesOnChannel);
		ok &= Resolve(module, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser", api.AcceptSessionWithUser);
		ok &= Resolve(module, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser", api.CloseSessionWithUser);
		ok &= Resolve(module, "SteamAPI_SteamNetworkingMessage_t_Release", api.ReleaseMessage);

		ok &= Resolve(module, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess", api.InitRelayNetworkAccess);
		ok &= Resolve(module, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus", api.GetRelayNetworkStatus);

		ok &= Resolve(module, "SteamAPI_RegisterCallback", api.RegisterCallback);
		ok &= Resolve(module, "SteamAPI_UnregisterCallback", api.UnregisterCallback);

		const bool voice = Resolve(module, "SteamAPI_ISteamUser_StartVoiceRecording", api.StartVoiceRecording) &&
		                   Resolve(module, "SteamAPI_ISteamUser_StopVoiceRecording", api.StopVoiceRecording) &&
		                   Resolve(module, "SteamAPI_ISteamUser_GetAvailableVoice", api.GetAvailableVoice) &&
		                   Resolve(module, "SteamAPI_ISteamUser_GetVoice", api.GetVoice) &&
		                   Resolve(module, "SteamAPI_ISteamUser_DecompressVoice", api.DecompressVoice) &&
		                   Resolve(module, "SteamAPI_ISteamFriends_SetInGameVoiceSpeaking", api.SetInGameVoiceSpeaking);
		if (!voice) {
			api.StartVoiceRecording = nullptr;  // voice chat checks this one
		}

		reportedMissing = true;
		if (!ok) {
			return nullptr;
		}

		// These return nullptr until the game has initialized Steam.
		api.user = user();
		api.friends = friends();
		api.matchmaking = matchmaking();
		api.messages = messages();
		api.utils = utils();
		if (!api.user || !api.friends || !api.matchmaking || !api.messages || !api.utils) {
			return nullptr;
		}

		loaded = &api;
		return &api;
	}

	const Api* Loaded()
	{
		return loaded.load();
	}
}
