#pragma once

#include <string>
#include <string_view>

// What the plugin does about a physics body gone NaN, which froze the host on 2026-10-11 (HavokNan.h says why;
// docs/HANG-20261011.md has the dump). The decisions are HavokNan.h; this reads and patches the engine (1.11.240, by
// Address Library ID) and calls them.
namespace HavokGuard
{
	// At plugin load: the exit test of Havok's angle loop, at both its copies, goes through a detour of ours that ends
	// the loop on a NaN and counts it, once each copy's bytes are checked to be 1.11.240's (HavokNan::SiteMatches). A copy
	// whose bytes differ (another version, another plugin's patch) is left as it is, logged. False when neither went in.
	bool Install();

	// Main thread, once a frame in game: when a NaN angle was counted, the loaded references whose 3D or place is not
	// finite are looked for and put right (HavokNan::Cleanup), and the enables that are due are done.
	void Frame();

	// The dev channel's `havoknan` (docs/DEV-CHANNEL.md): status runs on the channel's thread (it works while the main
	// thread is stuck); scan and poison run on the main thread.
	[[nodiscard]] std::string Command(std::string_view a_args);
	[[nodiscard]] bool        OnMainThread(std::string_view a_args);
}
