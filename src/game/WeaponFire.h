#pragma once

#include "Protocol.h"

// Weapon shots: the local player's and those of NPCs we run are reported, so other players see
// the shooter's firing animation. Only the animation is replayed (no projectile, no damage):
// damage already reaches everyone through health sync.
namespace WeaponFire
{
	// A grenade a stand-in throws of its own (a replayed throw's: WeaponFire.cpp) is ended by setting this bit in
	// Projectile::flags: it never goes off, and the engine frees it and drops it from the HUD's grenade marker. Disabled
	// and deleted instead, three of a stand-in's kept a marker on the friend's screen for good (2026-10-15). In the test a
	// live grenade's 0x18104 became 0x2018104, and it was gone with no explosion within 5 s, three tries out of three.
	inline constexpr std::uint64_t PROJECTILE_ENDED = 1ull << 25;

	// Hooks the player's animation events. Call every frame; does nothing once hooked.
	void Install();

	// Hooks NPCs' animation events (all NPCs share the hook), using any loaded NPC.
	void InstallForNpcs(RE::Actor* a_npc);

	// Shots by the local player since the last call.
	[[nodiscard]] std::uint32_t TakeShots();

	// NPCs (form IDs) that fired since the last call, one entry per shot. Includes NPCs we don't
	// run; the caller filters.
	[[nodiscard]] std::vector<std::uint32_t> TakeNpcShots();

	// The sound the game picked for the actor's gun when it was equipped (a keyword sound mapping's
	// form ID: the gun's mods decide it, an automatic receiver or a suppressor), or 0. Sent with
	// every shot as Shot::sound.
	[[nodiscard]] std::uint32_t ShotSound(RE::Actor* a_actor);

	// Plays a firing animation on a stand-in or mirrored NPC (if its weapon is out), with the
	// shooter's gun sound (their ShotSound; 0 = the one this game picked for the stand-in's gun).
	// A gun that is put away is still heard, without the animation; a swing waits for the weapon to
	// be out (AttackKind::ReplayShot).
	void PlayShot(RE::Actor* a_actor, std::uint32_t a_sound);

	// Replays a reload or aiming (a ShotAction other than a shot) on a stand-in or mirrored NPC.
	void PlayAction(RE::Actor* a_actor, std::uint8_t a_action);

	// Reloads and aiming by the local player, and by NPCs (form ID, action), since the last call.
	[[nodiscard]] std::vector<std::uint8_t> TakeActions();

	// Something the local player did by hotkey (an emote) that others should see.
	void QueueAction(std::uint8_t a_action);

	// The idle an emote action plays (editor ID), or nullptr.
	const char* EmoteIdleName(std::uint8_t a_action);
	[[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint8_t>> TakeNpcActions();

	// Explosions seen near the player in the seconds after one of our throws (a grenade landing), for the
	// others to set off too. Frame() scans while a watch is armed; ArmExplosionWatch() arms it.
	void ArmExplosionWatch();

	// Ends any replayed sound still following this actor: called before the actor is deleted, since
	// the sound follows its 3D root.
	void ReleaseSoundsOf(std::uint32_t a_actorFormId);
	[[nodiscard]] std::vector<Protocol::Explosion> TakeExplosions();
	// Sets off an explosion another player reported, if it is in our space and near enough to matter. Our copies
	// of NPCs another game runs are made ghosts while it goes off (BlastShield): their owner's game deals its damage.
	// The copies go by the session's ids (NpcSync::SessionId): our copy of a friend's spawn by its spawn id.
	void ApplyExplosion(const Protocol::Explosion& a_explosion);

	// Our copy of this NPC (the session's id) stops being mirrored: a ghost of ours against a friend's explosion
	// comes off now.
	void EndShield(std::uint32_t a_refId);

	// The same for every copy, before a save or a load and when the session ends (a ghost is never saved).
	void EndAllShields();

	// Dev (handthrowable on|off, off at every start): the grenade's model is hung in the hand of a stand-in replaying a
	// friend's throw, from its wind-up to its release (HandThrowable.h). Main thread, but for HandThrowables(), which
	// the animation hook reads.
	void               SetHandThrowables(bool a_on);
	[[nodiscard]] bool HandThrowables();

	// Takes the model out of this actor's hand: called before the actor is deleted.
	void DropHandThrowable(std::uint32_t a_actorFormId);

	// ... out of every hand: before a save or a load, and when the session ends.
	void DropHandThrowables();

	// An NPC died in our world (its form id: a plugin's, or a runtime NPC's that may be our copy of a friend's
	// spawn): a copy that was a ghost of ours then is counted and logged (the explosion reached it).
	void NoteDeath(std::uint32_t a_refId);

	// The NPCs this game runs (their reloads and aiming are queued; other NPCs' are not ours to report).
	void SetOwnedNpcs(std::vector<std::uint32_t> a_ids);

	// Ends the gunshot sounds that would otherwise loop (automatic weapons' fire sounds loop until
	// stopped). Call every frame.
	void Frame();

	[[nodiscard]] std::string Describe();
}

namespace WeaponFire
{
	// Test: starts (or stops) recording the player's animation events; returns what was recorded.
	std::string LogAnimationEvents(bool a_start);

	// Test: also records this NPC's events (hooks NPCs first).
	void LogActor(RE::Actor* a_actor);
}
