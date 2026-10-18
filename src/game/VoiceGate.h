#pragma once

#include <chrono>
#include <cstdint>

// When proximity voice chat (Voice.cpp) has work to do in a frame. Asking Steam whether it has recorded
// voice for us costs tens of microseconds a call, and it used to be asked every frame of the game: 94 us
// a frame on average for a host alone in a session (docs/perf/0.8.7-tracy-host-solo.csv), with nobody
// there to hear. Steam only has voice for us between StartVoiceRecording and the end of the tail it keeps
// recording after StopVoiceRecording, so it is asked only then, and at most every ASK_EVERY; and the
// microphone only starts when someone can hear it. No game types: tests/VoiceGateTests.cpp covers it.
namespace VoiceGate
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// Steam's EVoiceResult values read here.
	constexpr int RESULT_OK = 0;
	constexpr int RESULT_NOT_RECORDING = 2;
	constexpr int RESULT_NO_DATA = 3;

	// How often Steam is asked for recorded voice while it can have some. Each answer is everything
	// recorded since the last one, so this only sets how much speech goes in a piece: at 60 fps every
	// other frame (33 ms a piece), well inside the 80 ms a listener buffers before playing a speaker.
	constexpr auto ASK_EVERY = 20ms;

	// Steam records on for a moment after StopVoiceRecording (people let go of the key mid-word) and
	// hands that out until it answers NotRecording; past this long it is not asked again, should it
	// never say so.
	constexpr auto TAIL_LIMIT = 2s;

	enum class Mic : std::uint8_t
	{
		kOff,
		kOn,    // Steam is recording us
		kTail,  // stopped, and Steam may still hold the end of what was said
	};

	enum class Change : std::uint8_t
	{
		kNone,
		kStart,  // StartVoiceRecording
		kStop,   // StopVoiceRecording
	};

	// Whether talking (the key held, or an open mic) records: voice chat is on and someone can hear it,
	// another player in our session or, testing with loopback, ourselves.
	constexpr bool MayTalk(bool a_voiceChat, bool a_inSession, bool a_othersHere, bool a_loopback)
	{
		return a_voiceChat && ((a_inSession && a_othersHere) || a_loopback);
	}

	// Nothing to do this frame: nobody could hear us (so the talk key needn't even be read), Steam holds
	// nothing of ours, and no friend's voice has an output open (nothing to free or close).
	constexpr bool Idle(bool a_mayTalk, Mic a_mic, bool a_outputsOpen)
	{
		return !a_mayTalk && a_mic == Mic::kOff && !a_outputsOpen;
	}

	// What the microphone does when we are (a_talk) or are not talking. Talking again during the tail
	// starts a new recording at once.
	constexpr Change Next(Mic a_mic, bool a_talk)
	{
		if (a_talk && a_mic != Mic::kOn) {
			return Change::kStart;
		}
		if (!a_talk && a_mic == Mic::kOn) {
			return Change::kStop;
		}
		return Change::kNone;
	}

	// Whether to ask Steam for recorded voice now, a_sinceAsked after the last time.
	constexpr bool Ask(Mic a_mic, Clock::duration a_sinceAsked)
	{
		return a_mic != Mic::kOff && a_sinceAsked >= ASK_EVERY;
	}

	// After asking during the tail: whether Steam is done with it (it said NotRecording, or failed), or
	// it has gone on too long. What it answered this time is still taken.
	constexpr bool TailOver(int a_result, Clock::duration a_sinceStopped)
	{
		return (a_result != RESULT_OK && a_result != RESULT_NO_DATA) || a_sinceStopped >= TAIL_LIMIT;
	}
}
