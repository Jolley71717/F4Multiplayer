#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "game/TextureFlush.h"

// A friend's stand-in's plain bullet or beam (InertShot::IsPlainTrail, launched from an inert copy of its record so
// friends see it fly) never hits anyone: it passes through bodies as a projectile passes through its own shooter. Its
// damage was only zeroed (InertShots: projectile->damage = 0), and a 0-damage hit is a hit all the same: a TESHitEvent,
// and an NPC that flees, turns hostile or becomes shared, and a player whose hit counters move (WorldSync's
// puppetHits=). Kept free of the game so it can be tested; game/InertShots.cpp patches the engine.
//
// How a projectile hits (1.11.240, read from Fallout4.exe with the game folder's Fallout4.pdb): every impact a
// projectile is to make is first put to one function, Projectile::IsValidImpact(TESObjectREFR* collidee, CFilter,
// Projectile*, hknpBodyId) (ID 2237061, +E4DE70). It says no for a trigger volume, a small transparent layer the record
// doesn't collide with, the projectile's own shooter and the actor it rides, and a few more. A no is the engine's own
// "nothing was hit here": the cast's hit loop goes on to its next hit (the wall behind), the class's AddImpact returns
// -1 and adds nothing, so no ImpactData is made and Projectile::ProcessImpacts has no hit to hand the actor (the hit
// event, the reaction and the damage all come from there). Seven calls reach it; the four a bullet or a beam goes
// through are patched (SITES), each a call whose target is checked to be IsValidImpact before any is patched:
//   - the cast-hit loop (+E48C50, ID 2237022, the call at +0xCB8), which a beam's update (+E1E600) runs on the
//     all-hits collector its cast filled (as do a flame's, a grenade's and one more of Projectile's); a no skips that
//     hit (+E4F6C0 then sees whether it was a trap's trigger, as for any refused hit);
//   - Projectile::HandleHits (ID 2237063, +0x76C), the same loop for a moving projectile: Projectile::Move (+E4C410),
//     which a bullet's update calls, casts its shape along the step and hands the sorted hits to it;
//   - BeamProjectile::AddImpact (ID 2236559, +0x1E) and MissileProjectile::AddImpact (ID 2236887, +0x115), the classes
//     a beam and a bullet are (motion type beam, or missile/none), which ask again before adding the impact.
// The other three (ConeProjectile::AddImpact, FlameProjectile::AddImpact, GrenadeProjectile::AddImpact) belong to no
// plain trail and are left alone.
//
// The ways around that were looked at: the copy's collision layer (BGSProjectileData::collisionLayer, read by
// Projectile::GetCollisionLayer) would take the beam's cast past the bodies too, but whether a layer exists that stops at
// walls and not at bodies depends on the game's collision matrix and mods; and dropping the hit event after the fact
// (WorldSync's HitWatcher) comes after the actor has already reacted. BeamProjectile's own "dealt damage" flag (+37E)
// clears an impact's collidee only for a continuous beam, after its first impact.
namespace InertImpact
{
	// 1.11.240: the function each call is in (Address Library ID) and the call's offset in it.
	struct Site
	{
		std::uint64_t  id = 0;
		std::uintptr_t offset = 0;
	};

	inline constexpr std::uint64_t IS_VALID_IMPACT = 2237061;  // +E4DE70
	inline constexpr std::size_t   CALL_STUB = 14;             // what a write_call<5> takes of the trampoline (the four calls share it)
	inline constexpr std::array<Site, 4> SITES{ {
		{ 2237022, 0xCB8 },  // the cast-hit loop (+E48C50): the call at +E49908
		{ 2237063, 0x76C },  // Projectile::HandleHits (+E4E5D0): +E4ED3C
		{ 2236559, 0x1E },   // BeamProjectile::AddImpact (+E1FB50): +E1FB6E
		{ 2236887, 0x115 },  // MissileProjectile::AddImpact (+E3CBB0): +E3CCC5
	} };

	// The call sites, checked before any is patched: how many call a_target where 1.11.240 does. All or none are
	// patched: a beam that passes through bodies on one path and hits them on another would be as bad as before and
	// harder to read.
	struct Sites
	{
		std::size_t calling = 0;
		std::size_t total = 0;

		[[nodiscard]] constexpr bool All() const { return total != 0 && calling == total; }
	};

	// a_bytes[i]: the five bytes at a_at[i].
	constexpr Sites Check(std::span<const std::array<std::uint8_t, 5>> a_bytes, std::span<const std::uintptr_t> a_at, std::uintptr_t a_target)
	{
		Sites sites;
		sites.total = a_at.size();
		if (a_bytes.size() != a_at.size()) {
			sites.total = 0;  // a caller's mistake: nothing is patched
			return sites;
		}
		for (std::size_t i = 0; i < a_at.size(); ++i) {
			if (TextureFlush::CallTarget(std::span<const std::uint8_t, 5>(a_bytes[i]), a_at[i]) == a_target) {
				++sites.calling;
			}
		}
		return sites;
	}

	// Whether an impact the engine found valid (a_valid) is refused: a stand-in's trail (a_trail: its projectile's base
	// is an inert copy of a plain record, Trails) reaching an actor (a_actor: the collidee is one, alive or dead, ours, a
	// friend's stand-in or a mirrored NPC). It flies on through the body, as through its own shooter. Anything else is
	// the engine's as before, a trail's impact on a wall or a crate among them (no force, no damage: its look alone).
	constexpr bool Refused(bool a_valid, bool a_trail, bool a_actor)
	{
		return a_valid && a_trail && a_actor;
	}

	// The inert copies of plain records (a trail's base), asked of on every impact on an actor, on whatever thread the
	// engine runs the projectile's update: a fixed list that is only added to, read with no lock. The copies are never
	// freed (InertShots::CopyOf), so a pointer, once here, stays the copy's. Add is called under the copies' own lock
	// (one writer at a time); Contains from anywhere.
	template <std::size_t N>
	class Trails
	{
	public:
		// Whether a_copy was added (false: the list is full, or it is null). Once added, a second Add of it is a no.
		bool Add(const void* a_copy)
		{
			if (!a_copy || Contains(a_copy)) {
				return false;
			}
			const auto n = count_.load(std::memory_order_relaxed);
			if (n >= N) {
				return false;
			}
			slots_[n].store(a_copy, std::memory_order_relaxed);
			count_.store(n + 1, std::memory_order_release);  // the slot is seen before the count that covers it
			return true;
		}

		[[nodiscard]] bool Contains(const void* a_base) const
		{
			if (!a_base) {
				return false;
			}
			const auto n = count_.load(std::memory_order_acquire);
			for (std::size_t i = 0; i < n; ++i) {
				if (slots_[i].load(std::memory_order_relaxed) == a_base) {
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] std::size_t Size() const { return count_.load(std::memory_order_acquire); }

	private:
		std::array<std::atomic<const void*>, N> slots_{};
		std::atomic<std::size_t>                count_{ 0 };
	};
}
