#include "game/ExitMusic.h"

namespace ExitMusic
{
	namespace
	{
		// MainStatus::StageEvent, which CommonLibF4 does not have. The game sends 0 and 1 while it starts, 2 when
		// its data is loaded or reset (once from the thread that loads it, +0xC4189E), and 3 only from
		// Main::Shutdown (Fallout4+0xC3180C), on the main thread, before that deletes the region state
		// (+0xC319F2), shuts the audio down (+0xC31A20, +0xC31A2D) and deletes TES (+0xC31AAF). Every way out
		// of the game (qqq, the menus' Quit through Main::RequestQuitGame, Debug.QuitGame, closing the window)
		// sets the flag that ends Main::Run, and WinMain then runs Main::Shutdown.
		struct StageEvent
		{
			std::uint32_t stage;  // 00
		};

		constexpr std::uint32_t STAGE_SHUTDOWN = 3;

		// void(BSTEventSink<MainStatus::StageEvent>*): adds the sink to the stage event source, a game
		// singleton that is never deleted. Nothing takes the sink off again, so it is a static.
		constexpr REL::ID REGISTER_FOR_STAGE_EVENTS{ 2228981 };

		// The BGSPlayerMusicChanger, a function static in the game: all zero until the game first uses it.
		constexpr REL::ID MUSIC_CHANGER{ 4798157 };

		// void(BGSPlayerMusicChanger*): stops the music the changer holds at once (BSMusicEvent 2, to the
		// music manager, which is still there at stage 3) and forgets it. It touches nothing else of the
		// changer. The game's own TESAudio reset calls it (+0xBA50E5).
		constexpr REL::ID CLEAR_MUSIC{ 2232684 };

		struct MusicChanger
		{
			std::uintptr_t       vtable;      // 00
			std::uintptr_t       secondBase;  // 08
			RE::PlayerCharacter* player;      // 10  set when the player is made (the game's cell events compare it); not read here
			RE::TESForm*         music;       // 18  BGSMusicType: ~TESObjectCELL reads the region state only while it is set
		};
		static_assert(offsetof(MusicChanger, music) == 0x18);

		// Resolved at install, so nothing is looked up while the game shuts down.
		MusicChanger*  changer = nullptr;
		std::uintptr_t changerVtable = 0;
		void (*clearMusic)(MusicChanger*) = nullptr;
		bool installed = false;

		void StopMusic()
		{
			if (changer->vtable == 0) {
				REX::INFO("ExitMusic: shutting down; the music changer was never made");
				return;
			}
			if (changer->vtable != changerVtable) {
				REX::WARN("ExitMusic: ID {} is not the player's music changer in this game version; left alone", MUSIC_CHANGER.id());
				return;
			}
			const auto music = changer->music;
			if (!music) {
				REX::INFO("ExitMusic: shutting down; no music held");
				return;
			}
			const auto formID = music->GetFormID();
			clearMusic(changer);
			REX::INFO("ExitMusic: shutting down; stopped music {:08X} before the cells are deleted", formID);
		}

		class ShutdownWatcher :
			public RE::BSTEventSink<StageEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const StageEvent& a_event, RE::BSTEventSource<StageEvent>*) override
			{
				if (a_event.stage == STAGE_SHUTDOWN) {
					StopMusic();
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		ShutdownWatcher watcher;
	}

	void Install()
	{
		if (installed) {
			return;
		}
		installed = true;

		changer = REL::Relocation<MusicChanger*>{ MUSIC_CHANGER }.get();
		changerVtable = RE::VTABLE::BGSPlayerMusicChanger[0].address();
		clearMusic = REL::Relocation<void (*)(MusicChanger*)>{ CLEAR_MUSIC }.get();

		REL::Relocation<void (*)(RE::BSTEventSink<StageEvent>*)> registerForStageEvents{ REGISTER_FOR_STAGE_EVENTS };
		registerForStageEvents(&watcher);
		REX::INFO("ExitMusic: installed");
	}
}
