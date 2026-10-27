#pragma once

#include "game/MirrorGait.h"
#include "game/MotionPath.h"

// Puppets are actors driven entirely by us: remote players' stand-ins, and NPCs whose AI runs in
// another player's game. Their AI update is replaced with the engine's no-AI update, and their
// transform and animation state are forced to a target every frame.
namespace Puppets
{
	// How a puppet should be positioned and animated.
	struct Motion
	{
		RE::NiPoint3  position;
		float         heading = 0.0f;    // radians
		float         speed = 0.0f;      // units/second
		float         direction = 0.0f;  // travel relative to heading, fraction of a turn (0 fwd, 0.25 right, 0.5 back)
		float         pitch = 0.0f;      // aim up (+) or down (-), degrees: the owner's angle x (AimPoint.h)
		std::uint16_t moveMode = 0;      // ActorState::moveMode bits of the remote player
		std::uint8_t  flags = 0;         // Protocol::StateFlags
		std::uint8_t  gait = 0;          // with Protocol::kOwnGait: a mirrored NPC's gait and its clip's rate in its owner's game
		std::uint8_t  gaitRate = 0;      // (Protocol::ActorState::gait, gaitRate; MirrorGait.h, CreatureGait.h)
		// A mirrored NPC: the pitch its owner's copy aims its gun at (Protocol::ActorState::aimPitch), none while that
		// copy aims at nothing (AimPoint::ForMirror).
		std::optional<float> ownerAim;
		// Where it goes from the moment this motion was sampled (MotionPath.h): the engine's update puts it where the path is
		// at the moment that update runs. Empty: at position and heading.
		MotionPath::Path path;
	};

	enum class Kind
	{
		kPlayer,  // a remote player's stand-in: can't be hurt, activated or killed
		kNpc,     // a world NPC mirrored from its owner: can be hurt and killed normally
	};

	// Marks an actor as a puppet. Installs the hooks on first use.
	void Register(RE::Actor* a_actor, Kind a_kind = Kind::kPlayer);
	// Forgets an actor by address and form ID; the actor itself is never read (it may be gone already).
	void Unregister(const RE::Actor* a_actor, std::uint32_t a_formId);

	// True for remote players' stand-ins (not mirrored NPCs).
	[[nodiscard]] bool IsPuppet(const RE::Actor* a_actor);

	// True for any puppet: a remote player's stand-in or a mirrored NPC.
	[[nodiscard]] bool IsRegistered(const RE::Actor* a_actor);

	// Whether the actor uses furniture (sits or lies on it, or still holds it getting off).
	[[nodiscard]] bool InFurniture(const RE::Actor* a_actor);

	// Where a puppet's owner aims: the heading (radians) and the pitch (degrees up) its owner's game sent, or for a
	// mirrored NPC the pitch its owner's copy aims at (none while it aims at nothing: level). None for no puppet. Any
	// thread (the puppet's last state, under its lock): the launch hook aims a stand-in's inert shot by it
	// (InertShots), as the engine otherwise fires an NPC's shot at whoever it looks at.
	struct OwnerAim
	{
		float heading = 0.0f;  // radians, 0 along +y, a quarter turn along +x
		float pitch = 0.0f;    // degrees, up positive
	};
	[[nodiscard]] std::optional<OwnerAim> AimOf(const RE::Actor* a_actor);

	// Unregisters a mirrored NPC and gives it back to its own AI.
	void ReleaseNpc(RE::Actor* a_actor);

	// The puppet's equipment changed: draw the weapon again once it is in place. (Drawing before
	// the weapon arrives leaves the actor flagged as drawn but empty-handed, and then it can't
	// play attacks.)
	void RedrawWeapon(RE::Actor* a_actor);

	// No draw or holster is asked of the puppet for this long (a replayed throw plays first: DrawMatch::THROW_HOLD).
	void HoldDraw(RE::Actor* a_actor, std::chrono::steady_clock::duration a_for);

	// Sets the puppet's target motion; applied on the actor's next update.
	void SetTarget(RE::Actor* a_actor, const Motion& a_motion);

	// Dev (trace): when the engine's update last put the puppet at its target, and where. That is what is drawn, at that
	// moment of the frame; the actor's place read at another moment says nothing of how evenly it went.
	struct Put
	{
		std::chrono::steady_clock::time_point at;
		RE::NiPoint3                          position;
		std::chrono::steady_clock::time_point placedAt;  // the time on its path it was put at (FrameClock.h)
		std::chrono::steady_clock::time_point frameAt;   // the time of the frame the engine's timer had marked then (FrameClock.h)
		std::chrono::steady_clock::time_point previousAt;  // and the frame before's
	};
	[[nodiscard]] std::optional<Put> LastPut(const RE::Actor* a_actor);

	// Which locomotion graph any actor has (MirrorGait::Identify), and for a creature's the variable its state is in.
	// Three variable reads: its users keep the answer per actor (a puppet's entry, NpcSync's owned entry), asking again
	// only while it was kNone, since a graph loads after its actor.
	struct GaitGraph
	{
		MirrorGait::Family family = MirrorGait::Family::kNone;
		const char*        stateVariable = nullptr;  // MirrorGait::Family::kCreature only
	};
	[[nodiscard]] GaitGraph GaitGraphOf(const RE::Actor* a_actor);

	// The graph name for a variable or event, made once: building a BSFixedString looks the text up in the engine's string
	// table under its lock, which a mirror's every frame must not do. A fixed table of the names Gait, CreatureGait and
	// MirrorGait define (their events, rate variables and state variables), found by address or text with no lock of
	// ours; a name outside it is a programming error (logged once, an empty name answered).
	[[nodiscard]] const RE::BSFixedString& VariableName(const char* a_name);

	// Dev: whether the puppet's graph is moving, the idle stops sent to free it, how often its graph was
	// found in the other state and put back, its furniture state (sit state, the furniture it holds, how
	// often it was stood up out of furniture, whether it is held for furniture), the gait a dog's owner's game says it plays
	// (ownerGait=state@rate, CreatureGait.h), and its locomotion variables.
	[[nodiscard]] std::string Describe(const RE::Actor* a_actor);

	// Dev: where any actor aims (AimPoint.h): its angles in degrees, the point the engine gives its weapon aim
	// (and the pitch and heading that point is at from its feet, and the pitch from its shoulders that NpcSync sends),
	// the pitch a stand-in's owner sent or a mirrored NPC's owner's copy aims at (ownerAim), and the aim modifier's
	// graph variables.
	[[nodiscard]] std::string DescribeAim(const RE::Actor* a_actor);

	// The point the engine gives an actor's weapon aim (the one Actor::SetLookAtTarget left, which an NPC's combat
	// AI sets to what it shoots at), or none: what NpcSync sends of the NPCs it runs (AimPoint::OwnerPitch).
	[[nodiscard]] std::optional<RE::NiPoint3> AimPointOf(const RE::Actor* a_actor);

	// Dev: gives any actor an aim point a_pitch degrees up (or down) along its heading, or with none forgets its
	// point, and describes its aim. A stand-in or a mirrored NPC is given its owner's again on its next update.
	std::string AimAt(RE::Actor* a_actor, std::optional<float> a_pitch);

	// Lets the engine run the actor's own update (AI and all) for a while, for a furniture interaction
	// such as getting into power armor; the stand-in is not warped meanwhile, nor given its idle package
	// again. Each call sets the time left (zero ends it now).
	void AllowAI(RE::Actor* a_actor, std::chrono::milliseconds a_for);

	// The engine rebuilt the puppet's 3D with a fresh animation graph (a stand-in's power armor skeleton came): that graph
	// stands in its base state whatever the old one was told, so it is told at once whether it moves, its sneak again,
	// and a refused start counts as a new spell for the idle stops (ReleaseIdle).
	void GraphRebuilt(RE::Actor* a_actor);

	// A remedy for a graph that plays nothing (ArmorFrames::Remedy::kSetUp): on its next update the puppet's graph is set up
	// afresh (the actor's ModifyInitialAnimationStateImpl) whatever the engine says of its subgraphs, and told again whether
	// it moves. Normally that is done only for a rebuild the engine queued it for, once its subgraphs are loaded.
	void ForceGraphSetUp(RE::Actor* a_actor);
	// Dev: whether the engine has the actor's graph setup queued, whether its subgraphs read loaded, and how often we set it up.
	[[nodiscard]] std::string DescribeGraphSetUp(RE::Actor* a_actor);

	// A stand-in whose friend is down lies where they are (DownRules::StandInDown): down, its life state goes to
	// essential-down, which plays its graph's own downed pose (in a power armor frame too), and it is held there with no
	// recovery of its own; it is still put at its target each update, but told no locomotion, aim or draw. Up again, it is
	// alive again (the engine's get-up) and driven as before. Main thread.
	void SetDown(RE::Actor* a_actor, bool a_down);

	// Furniture (StandInSeat.h). A held puppet is neither placed nor animated by its target: furniture it is in places and
	// animates it, and one waiting to get in, or left beside it, stands where it is. Nor is its do-nothing package renewed
	// meanwhile (a new package can make the engine walk an actor out of its furniture). Its target is still kept.
	void Hold(RE::Actor* a_actor, bool a_hold);

	// The ways a puppet is sent into furniture, tried in this order (StandInSeat::MAX_TRIES).
	enum class Entry
	{
		kSnap,      // Papyrus Actor.SnapIntoInteraction: straight into it, as scenes put NPCs in their seats
		kActivate,  // the furniture activated by it (the console's activate, no OnActivate script), as an NPC uses one
	};
	// Asks the engine to put the puppet in the furniture, and holds it (Hold). It takes a frame or more: see FurnitureOf.
	// False if it could not be asked.
	bool Enter(RE::Actor* a_actor, RE::TESObjectREFR* a_furniture, Entry a_way);

	// Takes the puppet out of the furniture it is in at once, standing where it is (as a mirror is stood up when its owner's
	// copy walks off), and lets go of it (Hold). False if it was in none.
	bool StandUp(RE::Actor* a_actor);

	// The furniture an actor uses (sits or lies on, works at, or still holds getting off it), or null.
	[[nodiscard]] RE::NiPointer<RE::TESObjectREFR> FurnitureOf(const RE::Actor* a_actor);

	// Periodic maintenance; call once per frame from the main thread.
	void Tick();

	// For the dev status line: how often a stand-in's graph was found walking while it should stand, or
	// the other way round, and put back, and how many stand-ins' rebuilt graphs were set up once their subgraphs loaded (a
	// power armor skeleton's: see Puppets.cpp, SetUpRebuiltGraph). Empty until either happened.
	[[nodiscard]] std::string Describe();
}
