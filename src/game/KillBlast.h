#pragma once

#include <cstddef>
#include <cstdint>

// The console's kill crashed the game on a flying ant swarm (2026-11-21, a player's report from Nuka-World: every swarm
// they met in a session crashed the game, never alone; then the rig's host, at `<swarm>.kill`). Read from disk in 1.11.240:
// - The console's `kill` (Address Library ID 2204420, +5C03D0) first kills the actor (2229715, +C61800: the actor, the
//   killer or none, no damage, with its events). Then it blows the body up where the creature is made to burst:
//   Actor::GetBodyPartData (2229571, +C59190), BGSBodyPartData::GetBodyPart(Torso) (2206215, +617B40), and when that
//   part's flags (its BPND byte, BGSBodyPart +0x114) hold both Explodable (0x08) and Explodable - Absolute Chance (0x40),
//   and a test of the actor's AI process (2231157, +CD5A30, the call at +0x160: a count in its middle-high data) says no,
//   it takes the part's limb value (the BPND's actor value, BGSBodyPart +0x100), asks the actor for it
//   (ActorValueOwner's second slot, +CC93D0), changes it by as much and hands the actor a hit that explodes the torso.
// - It never looks whether the part HAS a limb value: with none, +CC93D0's first instruction reads the value's flags
//   through null (mov eax,[rdx+1ACh]): an access violation, in a vanilla game too.
// - The game's own data has one such body: DLC04_SwarmBodyPartData (Nuka-World's flying ant swarm, DLC04_SwarmRace). Its
//   torso (Swrm_BN_C_Torso) is Explodable with an absolute chance and has no actor value; its other parts have one. The
//   five other bodies whose torso always explodes (the bloatfly's, the tripod and bubble turrets', Far Harbor's defender
//   turret's, Nuka-World's animatronic alien's) name a value, so the console's kill bursts them and no more. Every other
//   body's torso lacks one flag or the other. A mod's creature can be made like the swarm.
// - A player seldom types `kill`, but the session does it for them: a death in one game lands on the other games' copies
//   with the console's kill (WorldSync::KillNamingNoKiller: the engine's whole death path, as if it died there), a quest
//   stage's kill is that command too (StageRules), and so is a downed player's. So a swarm that died in one game crashed
//   every other game that had it loaded.
// - Papyrus's Actor.Kill and Actor.KillSilent (both through 2251955, +1108D30) are the same code: the kill, then the same
//   test of the torso, the same call of the AI-process test (at +0xE4) and the same unchecked read. A script that kills
//   a swarm crashes the game too. The engine's other readers of a part's limb value test it for none first, or (+C5AB04)
//   read it only for a part with another flag (0x20) that no valueless part in the game's data has.
// So both calls of the AI-process test go through KillBlasts' hook, which says "no blast" for a torso with no limb value
// (the kill itself is done by then; the swarm's own death script bursts it, as when it dies of a shot) and asks the
// engine's test for every other body. That covers whoever kills that way: the session, a quest's stage, a script, the
// player's own console. Where the console's hook could not go in (another plugin's patch at the call), the session's kill
// of such an actor is not run at all (Refused): a copy left standing is better than a crash.
// Kept free of the game so it can be tested; KillBlasts.cpp reads the facts and puts it to the engine.
namespace KillBlast
{
	// The console's kill (ID 2204420): its call of the AI-process test before the torso's blast (E8 rel32).
	inline constexpr std::size_t TEST_CALL = 0x160;
	// Papyrus's Actor.Kill and KillSilent (ID 2251955): the same call in their shared body.
	inline constexpr std::size_t SCRIPT_TEST_CALL = 0xE4;

	// BGSBodyPart, as the command reads it.
	inline constexpr std::size_t PART_VALUE = 0x100;  // ActorValueInfo*: the limb's value (the BPND's actor value), or none
	inline constexpr std::size_t PART_FLAGS = 0x114;  // the BPND's flags byte

	inline constexpr std::uint8_t EXPLODABLE = 0x08;       // Explodable
	inline constexpr std::uint8_t ABSOLUTE_CHANCE = 0x40;  // Explodable - Absolute Chance

	// The first few skipped blasts are logged (with the actor and its body part data); the rest are only counted.
	inline constexpr std::uint32_t LOGGED = 5;

	// The actor's torso, as its race's body part data has it.
	struct Torso
	{
		bool         present = true;  // the actor has body part data with a torso part
		std::uint8_t flags = 0;       // the part's flags
		bool         value = true;    // the part names a limb value
	};

	// Whether the console's kill goes on to blast this torso (the command's own test of the part).
	constexpr bool Blasts(const Torso& a_torso)
	{
		constexpr std::uint8_t BOTH = EXPLODABLE | ABSOLUTE_CHANCE;
		return a_torso.present && (a_torso.flags & BOTH) == BOTH;
	}

	// Whether that blast crashes the game: it reads the part's limb value, and there is none.
	constexpr bool Crashes(const Torso& a_torso)
	{
		return Blasts(a_torso) && !a_torso.value;
	}

	enum class Verdict
	{
		kEngine,  // the engine's test decides, as it would alone
		kSkip,    // no blast: the torso has no limb value to read
	};

	// The hook's answer where the command asks whether to leave the blast out.
	constexpr Verdict Judge(const Torso& a_torso)
	{
		return Crashes(a_torso) ? Verdict::kSkip : Verdict::kEngine;
	}

	// Whether the session's own console kill of this actor is left out: only without the hook, and only where the
	// command would crash.
	constexpr bool Refused(bool a_hooked, const Torso& a_torso)
	{
		return !a_hooked && Crashes(a_torso);
	}
}
