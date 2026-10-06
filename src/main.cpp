#include "Config.h"
#include "DevChannel.h"

namespace
{
	// Logs the player's position so we can confirm the plugin can read game state.
	void LogPlayerPosition(std::string_view a_reason)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			REX::WARN("{}: player not available", a_reason);
			return;
		}

		const auto pos = player->GetPosition();
		REX::INFO("{}: player at ({:.1f}, {:.1f}, {:.1f})", a_reason, pos.x, pos.y, pos.z);
	}

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case F4SE::MessagingInterface::kGameDataReady:
			REX::INFO("Game data ready");
			break;
		case F4SE::MessagingInterface::kPostLoadGame:
			LogPlayerPosition("Save loaded");
			break;
		case F4SE::MessagingInterface::kNewGame:
			LogPlayerPosition("New game");
			break;
		default:
			break;
		}
	}
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);

	REX::INFO("F4Multiplayer loaded");

	Config::Load();

	if (!F4SE::GetMessagingInterface()->RegisterListener(OnF4SEMessage)) {
		REX::ERROR("Failed to register F4SE message listener");
		return false;
	}

	DevChannel::Start();

	return true;
}
