#pragma once

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
		std::uint16_t moveMode = 0;      // ActorState::moveMode bits of the remote player
		std::uint8_t  flags = 0;         // Protocol::StateFlags
	};

	enum class Kind
	{
		kPlayer,  // a remote player's stand-in: can't be hurt, activated or killed
		kNpc,     // a world NPC mirrored from its owner: can be hurt and killed normally
	};

	// Marks an actor as a puppet. Installs the hooks on first use.
	void Register(RE::Actor* a_actor, Kind a_kind = Kind::kPlayer);
	void Unregister(RE::Actor* a_actor);

	// True for remote players' stand-ins (not mirrored NPCs).
	[[nodiscard]] bool IsPuppet(const RE::Actor* a_actor);

	// Unregisters a mirrored NPC and gives it back to its own AI.
	void ReleaseNpc(RE::Actor* a_actor);

	// The puppet's equipment changed: draw the weapon again once it is in place. (Drawing before
	// the weapon arrives leaves the actor flagged as drawn but empty-handed, and then it can't
	// play attacks.)
	void RedrawWeapon(RE::Actor* a_actor);

	// Sets the puppet's target motion; applied on the actor's next update.
	void SetTarget(RE::Actor* a_actor, const Motion& a_motion);

	// Lets the engine run the actor's own update (AI and all) for a while, for a furniture interaction
	// such as getting into power armor; the stand-in is not warped meanwhile.
	void AllowAI(RE::Actor* a_actor, std::chrono::milliseconds a_for);

	// Periodic maintenance; call once per frame from the main thread.
	void Tick();
}
