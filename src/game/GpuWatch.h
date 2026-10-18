#pragma once

#include <string>

// Asks the game's Direct3D device once a second whether the graphics driver has taken it away, and says so in the
// log, once, when it has (GpuDevice.h has why: the crash that follows has no plugin frame on its stack).
namespace GpuWatch
{
	// Once a second, from the frame watch's thread (main.cpp).
	void Tick();

	// For the developer's `frames` command: gpu=ok|lost(reason)|none and how many checks ran. Any thread.
	[[nodiscard]] std::string Describe();
}
