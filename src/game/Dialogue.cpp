#include "game/Dialogue.h"
#include "game/StrandedDialogue.h"

#include "game/Puppets.h"

namespace Dialogue
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// The engine's MenuTopicManager, the one IsPlayerInDialogue reads (next to PlayerCamera's 4796065).
		// CommonLibF4's RE::MenuTopicManager::GetSingleton() reads ID 4796375, which is MenuControls'
		// singleton (the same as ID::MenuControls::Singleton): read through it, menuOpen was always set
		// and the speaker always none, whatever the player was doing.
		constexpr REL::ID TOPIC_MANAGER{ 4796064 };

		// MenuTopicManager::SpeakerShutMenu(TESObjectREFR*, BGSScene*, bool): how the engine ends a
		// conversation (the player moving away, the speaker dying or starting a fight, its scene ending).
		// It finds the speaker through its handle first and does nothing when that fails.
		constexpr REL::ID SPEAKER_SHUT_MENU{ 2227155 };

		// The reset every conversation ends with (SpeakerShutMenu's last step; starting one runs it too):
		// it lets go of the conversation's input layer and clears menuOpen. It needs no speaker.
		constexpr REL::ID RESET_MENU{ 2227138 };

		// Being in dialogue without the dialogue menu this long is logged, with what holds the player.
		constexpr auto HELD_REPORT = std::chrono::seconds(10);

		StrandedDialogue::Timer stranded;
		std::uint32_t           ended = 0;
		Clock::time_point       heldSince{};
		bool                    heldReported = false;
		bool                    wrongManagerReported = false;

		void SpeakerShutMenu(RE::MenuTopicManager* a_manager, RE::TESObjectREFR* a_speaker, RE::BGSScene* a_scene, bool a_closeMenu)
		{
			using func_t = void (*)(RE::MenuTopicManager*, RE::TESObjectREFR*, RE::BGSScene*, bool);
			static REL::Relocation<func_t> func{ SPEAKER_SHUT_MENU };
			func(a_manager, a_speaker, a_scene, a_closeMenu);
		}

		void ResetMenu(RE::MenuTopicManager* a_manager)
		{
			using func_t = void (*)(RE::MenuTopicManager*);
			static REL::Relocation<func_t> func{ RESET_MENU };
			func(a_manager);
		}

		RE::BGSScene* PlayerScene()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player ? player->GetCurrentScene() : nullptr;
		}

		// The player kept in dialogue (no weapons, Pip-Boy or workshop) with no dialogue menu to talk in
		// is logged once, with what keeps them there, whatever the cause.
		void ReportHeld(RE::MenuTopicManager* a_topics, RE::TESObjectREFR* a_speaker, bool a_menu, Clock::time_point a_now)
		{
			if (a_menu || !RE::IsPlayerInDialogue()) {
				heldSince = {};
				heldReported = false;
				return;
			}
			if (heldSince == Clock::time_point{}) {
				heldSince = a_now;
			}
			if (heldReported || a_now - heldSince < HELD_REPORT) {
				return;
			}
			heldReported = true;
			const auto actor = a_speaker ? a_speaker->As<RE::Actor>() : nullptr;
			const auto scene = PlayerScene();
			REX::WARN("Dialogue: the player has been in dialogue without the dialogue menu for {} s: menuOpen={} speaker={:08X} disabled={} deleted={} standIn={} scene={:08X} quest={:08X}",
				std::chrono::duration_cast<std::chrono::seconds>(HELD_REPORT).count(), a_topics->menuOpen, a_speaker ? a_speaker->GetFormID() : 0,
				a_speaker && a_speaker->IsDisabled(), a_speaker && a_speaker->IsDeleted(), actor && Puppets::IsPuppet(actor), scene ? scene->GetFormID() : 0,
				scene && scene->parentQuest ? scene->parentQuest->GetFormID() : 0);
		}
	}

	RE::MenuTopicManager* TopicManager()
	{
		static REL::Relocation<RE::MenuTopicManager**> singleton{ TOPIC_MANAGER };
		const auto manager = *singleton;
		if (!manager) {
			return nullptr;
		}
		// It must be what it is taken for: an ID naming another object is how the conversation was
		// misread before.
		static const auto vtable = RE::VTABLE::MenuTopicManager[0].address();
		if (*reinterpret_cast<const std::uintptr_t*>(manager) != vtable) {
			if (!wrongManagerReported) {
				wrongManagerReported = true;
				REX::ERROR("Dialogue: ID {} is not the MenuTopicManager in this game version; conversations are left alone", TOPIC_MANAGER.id());
			}
			return nullptr;
		}
		return manager;
	}

	bool Holds(RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return false;
		}
		if (const auto topics = TopicManager(); topics && topics->menuOpen && topics->speaker.get().get() == a_ref) {
			return true;
		}
		const auto scene = PlayerScene();
		return scene && a_ref->GetCurrentScene() == scene;
	}

	void Frame()
	{
		const auto topics = TopicManager();
		const auto ui = RE::UI::GetSingleton();
		if (!topics || !ui || ui->GetMenuOpen("LoadingMenu"sv)) {
			return;
		}
		const auto now = Clock::now();
		const auto speaker = topics->speaker.get();
		const bool menu = ui->GetMenuOpen("DialogueMenu"sv);
		ReportHeld(topics, speaker.get(), menu, now);

		const bool gone = !speaker || speaker->IsDeleted() || speaker->IsDisabled();
		if (!stranded.Due(topics->menuOpen && gone && !menu, now)) {
			return;
		}
		const auto id = speaker ? speaker->GetFormID() : 0;
		if (speaker) {
			// Disabled or deleted, but still there: the engine's own ending, as when the player moves away
			// (their part in a scene is walked away from). Not closing the dialogue menu: it isn't open.
			SpeakerShutMenu(topics, nullptr, PlayerScene(), false);
		}
		if (topics->menuOpen) {
			ResetMenu(topics);  // nothing to end it through: the reset it would have ended with
		}
		++ended;
		if (stranded.Ends() == 1) {
			REX::INFO("Dialogue: ended a conversation left open without its speaker ({:08X}{}); the player had no weapons, Pip-Boy or workshop", id,
				speaker ? (speaker->IsDeleted() ? ", deleted" : ", disabled") : ", gone");
		} else if (stranded.Ends() == 2) {
			REX::WARN("Dialogue: the conversation without a speaker is still open after ending it (menuOpen={} inDialogue={}); trying again every {} s",
				topics->menuOpen, RE::IsPlayerInDialogue(), std::chrono::duration_cast<std::chrono::seconds>(StrandedDialogue::GRACE).count());
		}
	}

	std::string Describe()
	{
		const auto topics = TopicManager();
		return std::format("dialogue: open={} ended={}", topics && topics->menuOpen, ended);
	}
}
