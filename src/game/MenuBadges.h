#pragma once

// The badge over the nearest friend's stand-in while they are in the Pip-Boy: the Pip-Boy's own world model, hung from
// the stand-in's 3D root over its head and turned to our camera each frame (StandInBadge says what and where). A node in
// the stand-in's loaded 3D, never a form or a reference, so nothing of it is saved; and it comes off before a save, a
// load, the session's end and the stand-in's deletion all the same. Off unless bMenuBadges is set: not seen on the rig
// yet. All on the main thread.
namespace MenuBadges
{
	// Each frame after RemotePlayers::Update.
	void Frame();

	// Before a stand-in is deleted (RemotePlayers' DeleteActor): off it, if it wears it.
	void Drop(std::uint32_t a_actorFormId);

	// Before a save or a load, and when the session ends: off whoever wears it.
	void DropAll();

	// Dev (menubadges): the model, who wears it, and what was hung and taken off.
	[[nodiscard]] std::string Describe();
}
