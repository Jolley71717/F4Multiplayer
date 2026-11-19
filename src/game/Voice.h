#pragma once

#include "Protocol.h"
#include "game/VoiceLevel.h"

// Proximity voice chat through Steam: while the push-to-talk key is held (or always, with an
// open mic), Steam records and compresses the microphone; the pieces go to the others, whose games
// decompress them and play them louder the closer the speaker's character is. Main thread only.
namespace Voice
{
	// Captures our voice and recycles finished playback buffers. Call every frame: it returns at once
	// while nobody could hear us and no friend's voice is playing (VoiceGate.h).
	// a_inSession: welcomed by a server.
	void Frame(bool a_inSession);

	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// A piece of another player's voice.
	void Apply(const Protocol::VoiceData& a_voice);

	void OnPlayerLeft(std::uint32_t a_id);

	// Stops recording and closes every playback device.
	void Reset();

	// What the push-to-talk HUD shows.
	struct Talk
	{
		bool             mayTalk = false;  // voice can be used now: the HUD is kept ready
		bool             talking = false;  // transmitting
		VoiceLevel::Look look;
	};
	[[nodiscard]] Talk TalkState();

	// Hosting with nobody here yet: the talk key still records and the HUD meters it, with nothing sent. Open mic stays off.
	void SetMeterAlone(bool a_on);

	// The menu's mic test: record without the key and hear ourselves, with nothing sent to anyone.
	void SetMicTest(bool a_on);
	[[nodiscard]] bool MicTesting();
	// The mic's level just now, in dB below full scale.
	[[nodiscard]] float MicLevel();

	// The audio output devices, by name.
	[[nodiscard]] std::vector<std::string> Outputs();
	// The output or volume changed: voices play on the new one from their next piece.
	void CloseOutputs();

	// Testing: record without the key, and play our own voice back to us.
	void SetForceTalk(bool a_on);
	void SetLoopback(bool a_on);

	[[nodiscard]] std::string Describe();
}
