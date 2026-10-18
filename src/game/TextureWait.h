#pragma once

#include <string>
#include <string_view>

// The engine's texture wait at the end of every move of the player, and what the plugin does about the hang it caused
// (TextureFlush.h says why; docs/HANG-20261017.md has the dump). The decision is TextureFlush.h; this reads the engine
// (1.11.240, by Address Library ID) and calls it.
namespace TextureWait
{
	// At plugin load: puts the hand-over in front of Main::OnIdle's call to PositionPlayerJob::Run, once its five bytes
	// are checked to be that call (TextureFlush::CallTarget). From then on, right before every move of the player
	// starts, when the texture walk owes the streamer requests it has counted with `pending` set, the main thread hands
	// them over (the engine's SubmitBatch, under the texture update's lock) and logs it. False (logged) when the call
	// isn't there: the game is then left as it is.
	bool Install();

	// For the frame watch's report, from its thread (the main thread is stopped for a look): empty unless the main
	// thread is inside the engine's pump, moving the player; then the texture wait's counters, which thread holds the
	// texture update's lock and whether it is in the wait, and how many requests the batch it set aside never handed
	// over. Reads only.
	[[nodiscard]] std::string DescribePump(void* a_window);

	// The dev channel's `texwait` (docs/DEV-CHANNEL.md): status, sample, flush on|off and unstick run on the channel's
	// thread and touch nothing the main thread needs (they work while it is stuck); strand runs on the main thread.
	[[nodiscard]] std::string Command(std::string_view a_args);
	[[nodiscard]] bool        OnMainThread(std::string_view a_args);
}
