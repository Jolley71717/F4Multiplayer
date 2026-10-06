#include "Config.h"
#include "DevChannel.h"
#include "game/FastLoad.h"
#include "net/Session.h"

namespace
{
	bool gameDataReady = false;

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case F4SE::MessagingInterface::kGameDataReady:
			if (Config::Get().fastLoading) {
				FastLoad::Install();
			}
			Session::Initialize();
			gameDataReady = true;
			REX::INFO("Game data ready");
			break;
		case F4SE::MessagingInterface::kPreSaveGame:
		case F4SE::MessagingInterface::kPreLoadGame:
			// Remote players' actors must never be written into a save.
			Session::OnBeforeSaveOrLoad();
			break;
		default:
			break;
		}
	}

	// Runs on the game's main thread once per frame (F4SE permanent task).
	void OnFrame()
	{
		if (gameDataReady) {
			Session::Frame();
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

	F4SE::GetTaskInterface()->AddTaskPermanent(OnFrame);

	DevChannel::Start();

	return true;
}
