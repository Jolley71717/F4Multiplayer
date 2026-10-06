#pragma once

// Puppets are actors driven entirely by us (remote players). Their AI update is replaced
// with the engine's no-AI update, and their transform is forced to a target every frame.
namespace Puppets
{
	// Marks an actor as a puppet. Installs the actor update hook on first use.
	void Register(RE::Actor* a_actor);
	void Unregister(RE::Actor* a_actor);
	void Clear();

	[[nodiscard]] bool IsPuppet(const RE::Actor* a_actor);

	// Sets where the puppet should be and how it is moving. Applied on the actor's next update.
	// a_speed is in units/second; a_direction is the movement direction relative to the
	// heading as a fraction of a turn (0 = forward, 0.25 = right, 0.5 = back, 0.75 = left).
	void SetTarget(RE::Actor* a_actor, const RE::NiPoint3& a_position, float a_heading, float a_speed = 0.0f, float a_direction = 0.0f);

	// Periodic maintenance; call once per frame from the main thread.
	void Tick();
}
