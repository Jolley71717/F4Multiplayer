#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace RE
{
	class Actor;
}

// A friend's NPC in power armor (their companion in a suit), shown in one here. Our copy of the NPC (a mirror, NpcSync) is
// put into a frame copy this game places for it (RemotePlayers::PlaceFrameCopy, kept out of saves) as its owner's copy is
// in a frame, with the owner's pieces in it, and taken out of it again as theirs gets out, as we stop mirroring it, before
// every save and load, and as the session ends: nothing of it is ever saved, ours or the NPC's.
//
// Getting out: the engine's own exit (PowerArmor's, 1.11.240) is run to its end at once. The game's SwitchToPowerArmor with
// None only begins it (2219450, state 3) and leaves the rest to the actor's update (2219457), which a mirror never runs
// (Puppets swaps its update for one without AI): she stayed in the frame, and a save kept her in it (the report this is for).
// Here the steps that update would run are called one after another, as the engine runs them for an actor whose process is
// not high: the item transfer back into the frame (2219466), the race given back, the finish (2219449), with no exit
// animation (Equipment::SwitchOutOfPowerArmor). Only ever on a mirror, and only out of the copy placed for it.
//
// Never touched: an NPC our game runs, a frame that is not the copy (one of its own here, or the world's), and a mirror that is
// in a frame of its own here.
namespace MirrorFrames
{
	using Clock = std::chrono::steady_clock;
	using Duration = Clock::duration;

	// The engine's power armor states (ExtraPowerArmor).
	constexpr std::uint8_t STATE_IN = 2;     // in it: getting in is over
	constexpr std::uint8_t STATE_LAST_EXIT = 5;  // 3 to 5: getting out, begun and not over (6 is the finish's own, never seen between frames)
	constexpr std::uint8_t STATE_NONE = 7;   // no ExtraPowerArmor (what the engine's getter answers)

	// How long a copy just placed may take to load before the switch-in is given up for this wear, and how many switch-ins
	// that did not take are tried.
	constexpr auto COPY_LIMIT = std::chrono::seconds(10);
	constexpr int  MAX_TRIES = 3;
	constexpr auto RETRY = std::chrono::seconds(2);

	enum class Do
	{
		kNothing,
		kPlaceCopy,    // a copy of the owner's frame is placed at the mirror
		kGetIn,        // the mirror is switched into its copy
		kGetOut,       // out of its copy (the owner's copy got out, or went into another suit)
		kDropCopy,     // the copy goes (nobody is in it)
		kGiveUp,       // the switch-in is given up for this wear: the copy goes, the mirror stays out
	};

	struct Facts
	{
		std::uint32_t ownerFrame = 0;  // the frame base its owner's copy is in (0: out of one, or one our game lacks)
		bool          ready = false;   // the mirror is mirrored, has its 3D, is alive and its process high or middle-high
		bool          inFrame = false;     // the engine has the mirror in a frame here (any state)
		bool          inCopy = false;      // ...in the copy placed for it
		std::uint8_t  state = STATE_NONE;  // its power armor state here
		bool          copyPlaced = false;  // a copy is placed for it (and still there)
		bool          copyLoaded = false;  // that copy has its 3D
		std::uint32_t copyBase = 0;        // that copy's base
		Duration      sincePlaced{};       // since the copy was placed
		int           tries = 0;           // switch-ins tried for this wear that did not take
		Duration      sinceTried = Duration::max();
		bool          gaveUp = false;      // given up for this wear (owner's frame base)
		bool          down = false;        // dead, dying or knocked down (ragdolled): Down
	};

	// The engine's life states (ActorState::lifeState) a body is past an exit in.
	constexpr std::int32_t LIFE_DYING = 1;
	constexpr std::int32_t LIFE_DEAD = 2;

	// Whether the mirror is a body the engine never takes out of a frame: dead, dying, or knocked down (ragdolled; a_knockState
	// not 0). The engine leaves a corpse in its suit, its pieces looted off it there, as the owner's game shows its copy; and the
	// exit's 3D rebuild under a ragdoll is nothing the engine ever runs.
	constexpr bool Down(bool a_dead, std::int32_t a_lifeState, std::uint32_t a_knockState)
	{
		return a_dead || a_lifeState == LIFE_DYING || a_lifeState == LIFE_DEAD || a_knockState != 0;
	}

	// As we stop mirroring it (Leave): what becomes of it and its copy. Every way mirroring stops (its owner's game no longer
	// runs it here, its owner's copy elsewhere, far or silent past HOLD, its owner gone, its death, its cell unloaded here, a
	// save, a load, the session's end) leaves it while it is still a mirror (NpcSync's StopMirroring before Puppets lets it
	// go, MirrorFrames::LeaveAll before NpcSync::ReleaseMirrors), so the exit, which only ever takes a mirror out, takes it.
	enum class Leaving
	{
		kDrop,     // not in its copy: the copy goes (RemotePlayers::Discard)
		kExit,     // in it, on its feet: the engine's exit, then the copy goes; still in it after (refused, failed, or the engine's
		           // preload still loading after the wait): kRelease
		kRelease,  // in it and down, or no longer a mirror (an NPC our game runs: never ours to take out): never taken out; the
		           // copy is left to the game with it in it (SaveGuard::Release), so a save has the frame it wears, as the game's
	};

	// a_mirror: still mirrored (Puppets), as every ending above leaves it; one that is not is a stop missed (Update's net).
	constexpr Leaving ChooseLeaving(bool a_inCopy, bool a_down, bool a_mirror = true)
	{
		if (!a_inCopy) {
			return Leaving::kDrop;
		}
		return a_down || !a_mirror ? Leaving::kRelease : Leaving::kExit;
	}

	// After the exit (Leaving::kExit): still in the copy, it is left to the game with it in it; out, the copy goes.
	constexpr Leaving AfterExit(bool a_stillInCopy)
	{
		return a_stillInCopy ? Leaving::kRelease : Leaving::kDrop;
	}

	// A mirror in its copy whose owner's copy went silent (NpcPlace::Lost::kSilent) is held there, still a mirror and still at
	// its owner's last state (NpcSync's UpdateMirrors), for up to HOLD of silence: an NPC killed in its owner's game goes silent
	// there first (a runner sends no state for one dying), and its death is said a moment later. Let go at once, the body was
	// out of its suit here and in it there (rig, 2026-11-21: out 44 ms before the death came, 3 s into the silence). Its death
	// within the hold leaves the body in its suit (Down: kRelease); its states back, it is mirrored on; anything else that
	// ends it (HOLD over, its owner gone, a save, the session's end) is a stop as any, the mirror taken out as it stops. Only
	// silence holds: an owner's copy that went through a door or ran far is alive there, and ours gets out at once.
	constexpr auto HOLD = std::chrono::seconds(10);

	// a_silentFor: since its owner's last state.
	constexpr bool HoldWhileSilent(bool a_inCopy, Duration a_silentFor)
	{
		return a_inCopy && a_silentFor <= HOLD;
	}

	constexpr Do Choose(const Facts& a_facts)
	{
		if (a_facts.inFrame && !a_facts.inCopy) {
			return Do::kNothing;  // a frame of its own here: never ours to take it out of
		}
		if (a_facts.inCopy && a_facts.down) {
			return Do::kNothing;  // a body in its suit stays in it (Down); one knocked down gets out once it is up
		}
		if (a_facts.inCopy) {
			// An exit begun and not over (waiting on the engine's preload of its own 3D) is carried on, whatever the owner's copy
			// does: there is no way back in from there.
			if (a_facts.state > STATE_IN && a_facts.state <= STATE_LAST_EXIT) {
				return Do::kGetOut;
			}
			// Out once it is in all the way (an exit only begins from there), when the owner's copy is out or in another suit.
			const bool wanted = a_facts.ownerFrame != 0 && a_facts.ownerFrame == a_facts.copyBase;
			return !wanted && a_facts.state == STATE_IN ? Do::kGetOut : Do::kNothing;
		}
		const bool wanted = a_facts.ownerFrame != 0 && !a_facts.gaveUp;
		if (!wanted || (a_facts.copyPlaced && a_facts.copyBase != a_facts.ownerFrame)) {
			return a_facts.copyPlaced ? Do::kDropCopy : Do::kNothing;
		}
		if (!a_facts.ready) {
			return a_facts.copyPlaced ? Do::kDropCopy : Do::kNothing;  // placed again once it is (a copy left standing is a frame seen empty)
		}
		if (!a_facts.copyPlaced) {
			return Do::kPlaceCopy;
		}
		if (a_facts.tries >= MAX_TRIES || (!a_facts.copyLoaded && a_facts.sincePlaced > COPY_LIMIT)) {
			return Do::kGiveUp;
		}
		if (!a_facts.copyLoaded || (a_facts.tries > 0 && a_facts.sinceTried < RETRY)) {
			return Do::kNothing;
		}
		return Do::kGetIn;
	}

	// What the switch-in gave the mirror that it is to give back as it gets out: the armor it carries more of after the exit
	// than before the switch-in (a piece that did not go back into the frame, the frame item). a_before and a_after: (base,
	// count) of its armor, any order. Never more than it gained, never a base it had as many of before.
	struct Count
	{
		std::uint32_t item = 0;
		std::int32_t  count = 0;
		bool operator==(const Count&) const = default;
	};
	inline std::vector<Count> Gained(const std::vector<Count>& a_before, const std::vector<Count>& a_after)
	{
		const auto total = [](const std::vector<Count>& a_list, std::uint32_t a_item) {
			std::int64_t n = 0;
			for (const auto& c : a_list) {
				if (c.item == a_item) {
					n += c.count;
				}
			}
			return n;
		};
		std::vector<Count> gained;
		for (const auto& c : a_after) {
			if (std::ranges::any_of(gained, [&](const Count& a_g) { return a_g.item == c.item; })) {
				continue;
			}
			const auto more = total(a_after, c.item) - total(a_before, c.item);
			if (more > 0) {
				gained.push_back({ c.item, static_cast<std::int32_t>(more) });
			}
		}
		return gained;
	}

	// Game side (main thread).

	// Its owner's game says the NPC our copy (a_actor, mirrored) is of is in a frame of this base (0: out of one), wearing
	// a_items. Acted on by Update.
	void Want(RE::Actor* a_actor, std::uint32_t a_frameBase, const std::vector<std::uint32_t>& a_items);

	// We stop mirroring it (its owner's game no longer runs it here, it unloaded, a save or a load comes, the session ends),
	// called while it is still a mirror: out of its copy at once, the copy gone (ChooseLeaving). A body (Down), or one the exit
	// did not take out, stays in it, and the copy is left to the game with it, never orphaned. a_why names it in the log. Its
	// form ID: the actor may be unloaded.
	void Leave(std::uint32_t a_actorId, std::string_view a_why);

	// Every mirror in a copy taken out (Leave), and what was wanted forgotten: before a save or a load, and as the session
	// ends (NpcSync::Reset, before its mirrors are let go). The waits on the engine's preload share one budget.
	void LeaveAll(std::string_view a_why);

	// Whether the mirror is in the copy placed for it (HoldWhileSilent).
	bool InCopy(RE::Actor* a_actor);

	// NpcSync holds it in its copy this frame (HoldWhileSilent): said once a hold, and its end (Leave, or mirrored on: Update).
	void Held(RE::Actor* a_actor, Duration a_silentFor);

	// A load is done: what was noted names the old game's forms (LeaveAll ran before it); forgotten without touching anything.
	void Forget();

	// Once a frame.
	void Update();

	// The actor's power armor as the engine has it (for the dev channel): in, state, frame, whether that is our copy, skeleton.
	std::string DescribeOf(RE::Actor* a_actor);
	std::string Describe();
}
