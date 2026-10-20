#pragma once

// An icon over a friend's stand-in while they are in the Pip-Boy or another menu (their status's menu): the Pip-Boy,
// two bars while paused, a bubble with three dots for the rest (MenuIconPlace). Drawn on the HUD from inside its own
// update, the way the compass markers are (Compass hooks it), and only while the HUD may be written to (Hud::IsSettled);
// while a menu of ours is up they are hidden, once, and nothing is written until it closes. Off unless bMenuIcons is
// set: on the rig the HUD showed them only in part (docs/DESIGN.md, Menus).
namespace MenuIcons
{
	// Main thread, each frame after RemotePlayers::Update: where each icon goes now.
	void Frame();

	// Inside the HUD's own update (Compass's hook): writes them.
	void OnHudAdvance(RE::IMenu* a_hud);

	// Dev (menuicons): each friend's menu, the spot over their head, its projection and where the icon went.
	[[nodiscard]] std::string Probe();

	// Dev (menuicons layer <path> <class>): makes the layer again under another object of the HUD's movie, as another
	// class (flash.display.Sprite or MovieClip).
	[[nodiscard]] std::string Layer(const std::string& a_parent, const std::string& a_class);

	// Dev (menuicons opt scale|readd|remake 0|1): ways of writing the icons, to try on the rig.
	[[nodiscard]] std::string Opt(const std::string& a_name, bool a_on);

	[[nodiscard]] std::string Describe();
}
