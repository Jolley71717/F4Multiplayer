#pragma once

#include "Protocol.h"

// Proximity voice chat through Steam: while the push-to-talk key is held (or always, with an
// open mic), Steam records and compresses the microphone; the pieces go to the others, whose games
// decompress them and play them louder the closer the speaker's character is. Main thread only.
namespace Voice
{
	// Captures our voice and recycles finished playback buffers. Call every frame.
	// a_inSession: welcomed by a server.
	void Frame(bool a_inSession);

	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// A piece of another player's voice.
	void Apply(const Protocol::VoiceData& a_voice);

	void OnPlayerLeft(std::uint32_t a_id);

	// Stops recording and closes every playback device.
	void Reset();

	// Testing: record without the key, and play our own voice back to us.
	void SetForceTalk(bool a_on);
	void SetLoopback(bool a_on);

	[[nodiscard]] std::string Describe();
}
