#include "game/SyncAnimMark.h"

#include "game/Downed.h"
#include "game/Hud.h"
#include "game/StuckSyncAnim.h"

namespace SyncAnimMark
{
	namespace
	{
		StuckSyncAnim::Guard guard;

		// ActorState's second flags word (ActorState + 0x0C) and the mark's bit in it.
		constexpr std::size_t   FLAGS2 = 0x0C;
		constexpr std::uint32_t IN_SYNC_ANIM = 1u << 21;

		std::uint32_t& Flags2(RE::PlayerCharacter* a_player)
		{
			return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(&static_cast<RE::ActorState&>(*a_player)) + FLAGS2);
		}

		// Whether the synchronized-animation manager has any animation at all (anyone's: one still being set up holds
		// no active entry for its actors yet, and may already have set their marks). Without the manager, yes: nothing
		// is cleared then.
		bool SyncAnimsRunning()
		{
			const auto manager = RE::BGSSynchronizedAnimationManager::GetSingleton();
			if (!manager) {
				return true;
			}
			const RE::BSAutoReadLock l{ manager->synchAnimsLock };
			return !manager->currentSynchAnims.empty();
		}

		StuckSyncAnim::Facts Read(RE::PlayerCharacter* a_player)
		{
			StuckSyncAnim::Facts facts;
			facts.inWorld = a_player->GetParentCell() && a_player->Get3D() && !Hud::LoadingScreenUp();
			if (!facts.inWorld) {
				return facts;
			}
			const auto& state = static_cast<const RE::ActorState&>(*a_player);
			facts.marked = state.inSyncAnim;
			if (!facts.marked) {
				return facts;  // nearly always: nothing more is looked at
			}
			facts.syncAnimsRunning = SyncAnimsRunning();
			facts.inScene = a_player->GetCurrentScene() != nullptr;
			facts.inFurniture = static_cast<int>(a_player->DoGetSitSleepState()) != 0 || static_cast<int>(state.interactingState) != 0;
			facts.inDialogue = RE::PlayerCamera::GetSingleton() && RE::IsPlayerInDialogue();
			facts.deadOrDown = state.lifeState != 0 || a_player->IsDead(false) || Downed::IsDown();
			return facts;
		}
	}

	void OnGameStart()
	{
		guard.Arm();
	}

	void Frame()
	{
		if (!guard.Armed()) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		if (guard.Frame(Read(player), StuckSyncAnim::Clock::now()) != StuckSyncAnim::Step::kClear) {
			return;
		}
		// The engine's job threads write this word too (the weapon and gun states): only the mark's bit is changed.
		std::atomic_ref<std::uint32_t>{ Flags2(player) }.fetch_and(~IN_SYNC_ANIM);
		REX::INFO("Input: the player was marked as in a synced animation with none running; cleared");
	}

	std::string SetForTest()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) {
			return "error: not in a game";
		}
		std::atomic_ref<std::uint32_t>{ Flags2(player) }.fetch_or(IN_SYNC_ANIM);
		REX::INFO("Input: test: the player is marked as in a synced animation with none running");
		return Describe();
	}

	std::string Describe()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const bool marked = player && static_cast<const RE::ActorState&>(*player).inSyncAnim;
		return std::format("sync: inSyncAnim={} running={} guard={} cleared={}", marked, SyncAnimsRunning(), guard.Armed() ? "armed" : "settled", guard.Cleared());
	}
}
