#pragma once

#include "Protocol.h"

#include <span>

// LooksMenu's body sliders (F4EE body morphs) and overlays (tattoos, face and body paint): read from our player and set on
// friends' stand-ins through LooksMenu's own Papyrus scripts, BodyGen and Overlays (see game/BodyMorphs.h and
// game/LooksOverlays.h). Without LooksMenu installed nothing here calls anything: our face goes without them, and a
// friend's are left be. Its skin overrides need nothing here: LooksMenu puts the skin on the base NPC, and Face carries it.
// Main thread only.
namespace LooksMenu
{
	// Whether LooksMenu's plugin (f4ee.dll) is loaded.
	[[nodiscard]] bool Installed();

	// Reads our player's sliders again when due, and rebuilds the body of a stand-in whose sliders are all set. Call once
	// per frame while in the game.
	void Frame();

	// Whether our face may go out yet: always without LooksMenu, else once the first read of our sliders is done or
	// given up (see BodyMorphs::Reading::Settled).
	[[nodiscard]] bool Settled();

	// Our player's sliders as last read (none without LooksMenu).
	[[nodiscard]] std::vector<Protocol::BodyMorph> PlayerMorphs();

	// Our player's overlays as last read (none without LooksMenu).
	[[nodiscard]] std::vector<Protocol::Overlay> PlayerOverlays();

	// A friend's sliders and overlays onto their new stand-in.
	void Apply(RE::Actor* a_standIn, bool a_female, const std::vector<Protocol::BodyMorph>& a_morphs, const std::vector<Protocol::Overlay>& a_overlays);

	// The stand-in goes: the sliders and overlays we set on it leave LooksMenu's store with it.
	void Remove(RE::Actor* a_standIn);

	// A save was loaded.
	void OnGameLoaded();

	// The co-save (on the save's or load's thread, frames held out): the form IDs LooksMenu may still keep a friend's looks
	// under as the save is written (a stand-in's removal still queued then; BodyMorphs::StandIns::Saved), read back with
	// the save and cleared off whatever reference has one after the load. Empty: none to write (always without LooksMenu).
	std::vector<std::uint8_t> SaveRecord();
	void                      LoadRecord(std::span<const std::uint8_t> a_bytes);
	void                      RevertRecord();

	// Dev: what was set on a stand-in and the skin it wears; what LooksMenu holds for it is asked for and goes to the log.
	std::string Look(RE::Actor* a_standIn);

	std::string Describe();
}
