#include "game/Dialogue.h"
#include "game/StrandedDialogue.h"
#include "game/Hud.h"
#include "game/OpenMenus.h"

#include "game/Puppets.h"
#include "game/QuestSync.h"

namespace Dialogue
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// The engine's MenuTopicManager, the one IsPlayerInDialogue reads (next to PlayerCamera's 4796065).
		// CommonLibF4's RE::MenuTopicManager::GetSingleton() reads ID 4796375, which is MenuControls'
		// singleton (the same as ID::MenuControls::Singleton): read through it, menuOpen was always set
		// and the speaker always none, whatever the player was doing. The wrong ID is the library's
		// (ID::MenuTopicManager::Singleton in RE/IDs.h); lib/commonlibf4 is a submodule kept as published,
		// so the right one is used here instead.
		constexpr REL::ID TOPIC_MANAGER{ 4796064 };

		// The reset every conversation ends with (SpeakerShutMenu's last step; starting one runs it too):
		// it lets go of the conversation's input layer, which is what takes the player's weapons, Pip-Boy
		// and workshop, and clears menuOpen. It needs no speaker and leaves scenes alone.
		// MenuTopicManager::SpeakerShutMenu (2227155) is not used: before the reset it walks the player
		// out of the dialogue scene they are in (BGSScene::SetPlayerWalkAway and StartWalkAway on the
		// player's current scene, whichever scene it is passed), which can end a quest's scene.
		constexpr REL::ID RESET_MENU{ 2227138 };

		// Being in dialogue without the dialogue menu this long is logged, with what holds the player.
		constexpr auto HELD_REPORT = std::chrono::seconds(10);

		StrandedDialogue::Watch   watch;
		std::uint32_t             ended = 0;
		Clock::time_point         heldSince{};
		bool                      heldReported = false;
		bool                      wrongManagerReported = false;

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

		// The engine's own test: menuOpen, or the camera in its dialogue state. It reads the MenuTopicManager
		// and the PlayerCamera without checking that they are there.
		bool InDialogue()
		{
			return RE::PlayerCamera::GetSingleton() && RE::IsPlayerInDialogue();
		}

		// Out of dialogue: nothing known of a conversation, nothing waited for.
		void Forget()
		{
			watch.Reset();
			heldSince = {};
			heldReported = false;
		}

		// The player kept in dialogue (no weapons, Pip-Boy or workshop) with no dialogue menu to talk in
		// is logged once, with what keeps them there, whatever the cause. Called while in dialogue.
		void ReportHeld(RE::MenuTopicManager* a_topics, RE::TESObjectREFR* a_speaker, bool a_menu, bool a_ours, Clock::time_point a_now)
		{
			if (a_menu) {
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
			REX::WARN("Dialogue: the player has been in dialogue without the dialogue menu for {} s: menuOpen={} speaker={:08X} disabled={} deleted={} standIn={} ours={} scene={:08X} quest={:08X}",
				std::chrono::duration_cast<std::chrono::seconds>(HELD_REPORT).count(), a_topics->menuOpen, a_speaker ? a_speaker->GetFormID() : 0,
				a_speaker && a_speaker->IsDisabled(), a_speaker && a_speaker->IsDeleted(), actor && Puppets::IsPuppet(actor), a_ours, scene ? scene->GetFormID() : 0,
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
		// Not a scene a friend's stage started here (Codsworth's search, from his talk's stage): the friend is
		// playing it, and its people are theirs to run; claimed for it, Codsworth would be taken from the friend
		// mid-search. The friend's later stages end it here.
		const auto scene = PlayerScene();
		return scene && scene->GetFormID() != QuestSync::SessionScene() && a_ref->GetCurrentScene() == scene;
	}

	void Removed(std::uint32_t a_id, RE::TESObjectREFR* a_ref)
	{
		const auto scene = a_ref ? a_ref->GetCurrentScene() : nullptr;
		watch.Removed(a_id, scene ? scene->GetFormID() : 0, Clock::now());
	}

	void Frame()
	{
		// Out of dialogue, as the player nearly always is, nothing more is looked at.
		const auto topics = TopicManager();
		if (!topics || !InDialogue()) {
			Forget();
			return;
		}
		const auto now = Clock::now();
		const auto speaker = topics->speaker.get();
		const auto scene = PlayerScene();
		const bool menu = OpenMenus::Open(OpenMenus::Menu::kDialogue);
		const bool loading = Hud::LoadingScreenUp();
		// Only a conversation the plugin itself left without its speaker is ended (StrandedDialogue): one
		// the game left open is the game's, and so is a scene of its own the player is in.
		const bool due = watch.Due({
										.open = topics->menuOpen,
										.speaker = speaker ? speaker->GetFormID() : 0,
										.speakerGone = !speaker || speaker->IsDeleted() || speaker->IsDisabled(),
										.talking = menu,
										.scene = scene ? scene->GetFormID() : 0,
										.loading = loading,
									},
			now);
		// A loading screen is no part of the wait (it can last longer): it starts over once the screen is gone.
		if (loading) {
			heldSince = {};
			return;
		}
		ReportHeld(topics, speaker.get(), menu, watch.Ours(), now);
		if (!due) {
			return;
		}
		const auto id = watch.Speaker();
		// The speaker's side first, as in the engine's own ending: it no longer talks with the player
		// (SpeakerShutMenu does this before its reset). A deleted one is left alone.
		if (speaker && !speaker->IsDeleted()) {
			speaker->SetDialoguewithPlayer(false, false, nullptr);
		}
		ResetMenu(topics);
		++ended;
		if (watch.Ends() == 1) {
			REX::INFO("Dialogue: ended a conversation left open without its speaker ({:08X}{}); the player had no weapons, Pip-Boy or workshop", id,
				speaker ? (speaker->IsDeleted() ? ", deleted" : ", disabled") : ", gone");
		} else if (watch.Ends() == 2) {
			REX::WARN("Dialogue: the conversation without a speaker is still open after ending it (menuOpen={} inDialogue={}); trying again every {} s",
				topics->menuOpen, RE::IsPlayerInDialogue(), std::chrono::duration_cast<std::chrono::seconds>(StrandedDialogue::GRACE).count());
		}
	}

	std::string ResetForTest(bool a_overScene)
	{
		const auto topics = TopicManager();
		if (!topics) {
			return "none";
		}
		const auto state = [&] {
			const auto speaker = topics->speaker.get();
			return std::format("menuOpen={} overSceneActor={} speaker={:08X} inDialogue={}", topics->menuOpen, topics->overSceneActor,
				speaker ? speaker->GetFormID() : 0, InDialogue());
		};
		const auto before = state();
		ResetMenu(topics);
		if (a_overScene) {
			topics->overSceneActor = false;
		}
		REX::INFO("Dialogue: reset for a test{}: before {}, after {}", a_overScene ? " (overSceneActor cleared too)" : "", before, state());
		return "before " + before + "; after " + state();
	}

	std::string Describe()
	{
		const auto topics = TopicManager();
		return std::format("dialogue: open={} ended={}", topics && topics->menuOpen, ended);
	}
}
