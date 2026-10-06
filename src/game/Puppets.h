#pragma once

// Puppets are actors driven entirely by us (remote players). Their AI update is replaced
// with the engine's no-AI update, and their transform and animation state are forced to a
// target every frame.
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

	// Marks an actor as a puppet. Installs the hooks on first use.
	void Register(RE::Actor* a_actor);
	void Unregister(RE::Actor* a_actor);
	void Clear();

	[[nodiscard]] bool IsPuppet(const RE::Actor* a_actor);

	// Sets the puppet's target motion; applied on the actor's next update.
	void SetTarget(RE::Actor* a_actor, const Motion& a_motion);

	// Periodic maintenance; call once per frame from the main thread.
	void Tick();
}
