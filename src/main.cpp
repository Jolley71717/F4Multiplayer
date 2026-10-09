#include "Config.h"
#include "DevChannel.h"
#include "DevCommands.h"
#include "game/FastLoad.h"
#include "game/WorldSync.h"
#include "game/FrameStats.h"
#include "net/Session.h"

namespace
{
	bool gameDataReady = false;

	// Co-save records: which of the session's changes each save contains.
	constexpr std::uint32_t SERIALIZATION_ID = 'F4MP';

	void OnGameSaved(const F4SE::SerializationInterface* a_intfc)
	{
		WorldSync::SaveResyncPoint(a_intfc);
	}

	void OnGameLoaded(const F4SE::SerializationInterface* a_intfc)
	{
		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;
		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type == 'CONT') {
				WorldSync::LoadResyncPoint(a_intfc, version, length);
			}
		}
	}

	void OnRevert(const F4SE::SerializationInterface*)
	{
		WorldSync::RevertResyncPoint();
	}

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
		case F4SE::MessagingInterface::kPostLoadGame:
		case F4SE::MessagingInterface::kNewGame:
			if (gameDataReady) {
				Session::OnGameLoaded();
			}
			break;
		default:
			break;
		}
	}

	// Runs on the game's main thread once per frame (F4SE permanent task).
	void OnFrame()
	{
		FrameStats::Global().Frame(std::chrono::steady_clock::now());
		if (gameDataReady) {
			Session::Frame();
			DevCommands::Frame();
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

	if (const auto serialization = F4SE::GetSerializationInterface()) {
		serialization->SetUniqueID(SERIALIZATION_ID);
		serialization->SetSaveCallback(OnGameSaved);
		serialization->SetLoadCallback(OnGameLoaded);
		serialization->SetRevertCallback(OnRevert);
	} else {
		REX::WARN("No serialization interface: loading a save mid-session may duplicate shared loot");
	}

	DevChannel::Start();

	return true;
}

namespace FrameStats
{
	Tracker& Global()
	{
		static Tracker tracker;
		return tracker;
	}
}
