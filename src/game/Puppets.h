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

	// Sets where the puppet should be. Applied on the actor's next update.
	void SetTarget(RE::Actor* a_actor, const RE::NiPoint3& a_position, float a_heading);

	// Periodic maintenance; call once per frame from the main thread.
	void Tick();
}
