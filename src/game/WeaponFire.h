#pragma once

#include "Protocol.h"
#include "game/RemoteEdits.h"

#include <optional>

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

	// Shots (and swings or punches) by the local player since the last call. A gun's bash is left out
	// (AttackKind::ReportsAsShot): friends would replay it as a gunshot. Main thread.
	[[nodiscard]] std::uint32_t TakeShots();

	// NPCs (form IDs) that fired since the last call, one entry per shot, a gun's bash left out as for
	// TakeShots. Includes NPCs we don't run; the caller filters. Main thread.
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

	// The explosions our player and the NPCs our game runs set off, for the others to set off too, once (docs/DESIGN.md,
	// "Who sets off an explosion"). Frame() scans while a watch is armed: ArmExplosionWatch() after a throw (a_thrower: our
	// player as PLAYER_REF_ID, or the NPC; one with no owner named is taken for the throw's then), ArmShotWatch() after a
	// shot that may go off (a missile, a shell, an explosive bullet: one the shooter is named the owner of; Frame asks
	// each gun of ours that fired). A destructible object's own (its stage's) goes with its wreck instead.
	void ArmExplosionWatch(std::uint32_t a_thrower);
	void ArmShotWatch();

	// A shot of ours hit a destructible object (a mine, a car: Destructibles) that may set off an explosion other than its
	// stage's own: one within a_radius of it is reported for the next few seconds, as after a throw (no other one).
	void ArmBlastWatch(RE::TESObjectREFR* a_object, float a_radius);

	// The explosion that dealt a blow on a_target just now (the one the hit names, a_source: its HitData's source
	// reference; else the nearest that reaches it), and what became of it: reported, each game's own, or ours alone
	// (RemoteEdits::Fate, RemoteEdits::BlastLog::At). One not looked at yet is decided now, as the scan would have.
	// Main thread.
	[[nodiscard]] std::optional<RemoteEdits::Seen> BlastAt(RE::TESObjectREFR* a_target, std::uint32_t a_source = 0);

	// Whether our game reports the explosions around our player now (a throw's watch is on, and each of its explosions so
	// far was reported), so each goes off in the friends' games too; and whether it did within THREW_LATELY, while the
	// fire a Molotov left may still burn there (CopyHealth::OwnersToDeal). Any thread.
	inline constexpr auto THREW_LATELY = std::chrono::seconds{ 20 };
	// Each thrower's own (a_thrower: the hit's cause, our player as PLAYER_REF_ID; 0, unknown: the last to throw), so
	// that an NPC's throw never marks our player's kept Molotov as shared.
	[[nodiscard]] bool ExplosionsShared(std::uint32_t a_thrower);
	[[nodiscard]] bool ThrewLately(std::uint32_t a_thrower);

	// Ends any replayed sound still following this actor: called before the actor is deleted, since
	// the sound follows its 3D root.
	void ReleaseSoundsOf(std::uint32_t a_actorFormId);
	[[nodiscard]] std::vector<Protocol::Explosion> TakeExplosions();
	// Sets off an explosion another player reported, if it is in our space and near enough to matter. Our copies
	// of NPCs another game runs are made ghosts while it goes off (BlastShield): their owner's game deals its damage.
	// The copies go by the session's ids (NpcSync::SessionId): our copy of a friend's spawn by its spawn id.
	void ApplyExplosion(const Protocol::Explosion& a_explosion);

	// Whether this explosion is one our game set off for a friend's throw (ApplyExplosion) or trap (SetOffTrapExplosion):
	// theirs, never ours to report what it throws about (MovedObjects).
	[[nodiscard]] bool SetOffForFriend(RE::TESObjectREFR* a_explosion);

	// A world mine's blast, set off here because the mine went off in a_from's game (Traps): placed as their explosion
	// would be, our copies of NPCs other games run made ghosts while it goes off, and never reported back as ours.
	void SetOffTrapExplosion(RE::BGSExplosion* a_explosion, const RE::NiPoint3& a_at, std::uint32_t a_from);

	// Our copies of NPCs other games run near a_at are made ghosts a moment (BlastShield; called again to keep them so):
	// a trap set off here for a friend's (its grenades, its shots) is their owners' to deal.
	void ShieldCopiesNear(const RE::NiPoint3& a_at, float a_radius, std::uint32_t a_from);

	// Our copy of this NPC (the session's id) stops being mirrored: a ghost of ours against a friend's explosion
	// comes off now.
	void EndShield(std::uint32_t a_refId);

	// The same for every copy, before a save or a load and when the session ends (a ghost is never saved).
	void EndAllShields();

	// Before a save, after EndAllShields: the console's setghost 0 lands a frame after it is sent, so the ghosts it is
	// still to take off have their flag turned off for the save itself (a ghost saved stayed one for good in the game
	// that loaded it). AfterSave, once the save is written, turns it back on and sends setghost 0 again, which the
	// console leaves alone on a flag that reads off already. Main thread.
	void HideGhostsFromSave();
	void AfterSave();

	// The grenade is shown in the hand of a stand-in replaying a friend's throw, from its wind-up to its release
	// (HandThrowable.h): set from bGrenadeInHand at load, and by the dev's handthrowable on|off|model (a_modelFallback:
	// the model database's own node too, on a hand the engine gave no WeaponLeft). Main thread, but for
	// HandThrowables(), which the animation hook reads.
	void               SetHandThrowables(bool a_on, bool a_modelFallback = false);
	[[nodiscard]] bool HandThrowables();
	[[nodiscard]] bool HandModelFallback();

	// Takes the model out of this actor's hand: called before the actor is deleted.
	void DropHandThrowable(std::uint32_t a_actorFormId);

	// ... out of every hand: before a save or a load, and when the session ends.
	void DropHandThrowables();

	// An NPC died in our world (its form id: a plugin's, or a runtime NPC's that may be our copy of a friend's
	// spawn): a copy that was a ghost of ours then is counted and logged (the explosion reached it).
	void NoteDeath(std::uint32_t a_refId);

	// Any actor is dying or died in our world (a stand-in aside), whatever its form id: an explosion with no owner beside it in the next
	// moments is its death blast (RemoteEdits::DiedBeside). Any thread.
	void NoteDying(std::uint32_t a_refId, bool a_dead);

	// The NPCs this game runs (their reloads and aiming are queued; other NPCs' are not ours to report).
	void SetOwnedNpcs(std::vector<std::uint32_t> a_ids);
	// Our companions no other game runs (NpcSync's scan; one made at runtime too): their throws and shots arm the watches,
	// what they set off is ours to report.
	void SetOurCompanions(std::vector<std::uint32_t> a_ids);

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
