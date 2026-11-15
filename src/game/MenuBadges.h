#pragma once

// The badge over the nearest friend's stand-in while they are in the Pip-Boy: the Pip-Boy's own world model, hung from
// the stand-in's 3D root over its head and turned to our camera each frame (StandInBadge says what and where). A node in
// the stand-in's loaded 3D, never a form or a reference, so nothing of it is saved; and it comes off before a save, a
// load, the session's end and the stand-in's deletion all the same. Off unless bMenuBadges is set: not seen on the rig
// yet. All on the main thread (Describe too: the dev channel's commands run there) but SetCopy and CopyWanted.
namespace MenuBadges
{
	// Each frame after RemotePlayers::Update. Nothing is done while a loading screen is up (no stand-in's 3D is touched
	// then); out of the world (no cell) it comes off.
	void Frame();

	// Before a stand-in is deleted (RemotePlayers' DeleteActor): off it, if it wears it.
	void Drop(std::uint32_t a_actorFormId);

	// Before a save or a load, and when the session ends: off whoever wears it.
	void DropAll();

	// When the session ends and after a load (on the main thread both): off whoever wears it (DropAll), then the model
	// and our node it hangs under let go, so nothing of ours outlives the world it was hung in. The next hang asks for
	// the model again and makes a new node. Never before a load: that work can run on the load's thread when the main
	// thread is late (main.cpp's RunBeforeSaveOrLoad), beside a frame that may still draw them; the session's DropAll
	// has taken the badge off by then, and holding the model and our node across the load is harmless.
	void Release();

	// At the game's shutdown (Session::Shutdown, on the main thread): the stand-in's root it hangs from, our node and the
	// model are let go of without being freed or taken off, and nothing of the badge runs again. The world is being torn
	// down then, and a static's destructor would free them at the DLL's unload, after the renderer and Havok are gone.
	void AtExit();

	// Dev (menubadges copy on|off, off at every start): a Pip-Boy model with collision hangs as our own copy with the
	// collision taken off it, instead of being refused (StandInBadge.h). Any thread; the next Frame takes the badge off
	// and asks for the model again.
	void SetCopy(bool a_on);
	[[nodiscard]] bool CopyWanted();

	// Dev (menubadges): the model, who wears it, and what was hung and taken off.
	[[nodiscard]] std::string Describe();
}
