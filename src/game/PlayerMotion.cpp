#include "game/PlayerMotion.h"

#include "game/EngineClock.h"

#include <atomic>
#include <mutex>

namespace PlayerMotion
{
	namespace
	{
		// Actor::Update vtable slot (see RE/A/Actor.h; Puppets hooks the same slot of the stand-ins' class).
		constexpr std::size_t UPDATE_INDEX = 0xCF;

		using UpdateFn = void (*)(RE::Actor*, float);
		UpdateFn originalUpdate = nullptr;
		bool     installed = false;

		std::mutex             lock;
		EngineClock::Timeline  timeline;
		std::optional<Sample>  last;
		std::atomic<bool>      engineTime{ true };

		void UpdateHook(RE::Actor* a_this, float a_delta)
		{
			originalUpdate(a_this, a_delta);
			if (a_this != RE::PlayerCharacter::GetSingleton()) {
				return;
			}
			const auto wall = std::chrono::steady_clock::now();
			std::scoped_lock l{ lock };
			if (!last) {
				REX::INFO("PlayerMotion: the player's update runs (frame time {:.1f} ms)", a_delta * 1000.0f);
			}
			const auto at = timeline.Advance(wall, a_delta);
			last = Sample{ at, wall, a_this->data.location, a_this->data.angle.z, a_this->GetParentCell() };
		}
	}

	void Install()
	{
		if (installed) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		installed = true;
		const auto vtable = *reinterpret_cast<std::uintptr_t*>(player);
		const auto slot = vtable + sizeof(void*) * UPDATE_INDEX;
		originalUpdate = *reinterpret_cast<UpdateFn*>(slot);
		REL::WriteSafeData(slot, reinterpret_cast<std::uintptr_t>(&UpdateHook));
		REX::INFO("PlayerMotion: hooked the player's update (vtable {:X})", vtable);
	}

	std::optional<Sample> Last(std::chrono::steady_clock::time_point a_now, const RE::TESObjectCELL* a_cell, std::chrono::steady_clock::duration a_fresh)
	{
		std::scoped_lock l{ lock };
		if (!last || last->cell != a_cell || a_now - last->wall > a_fresh) {
			return std::nullopt;
		}
		return last;
	}

	void SetEngineTime(bool a_on) { engineTime.store(a_on, std::memory_order_relaxed); }

	bool EngineTime() { return engineTime.load(std::memory_order_relaxed); }
}
