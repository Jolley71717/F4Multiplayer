#include "Test.h"

#include "game/VoiceGate.h"

using namespace std::chrono_literals;
using VoiceGate::Change;
using VoiceGate::Mic;

TEST("voice gate: a host alone, or a game with no session, does nothing for voice")
{
	// Alone in our own session, or not in one: the talk key isn't even read.
	CHECK(!VoiceGate::MayTalk(true, true, false, false));
	CHECK(!VoiceGate::MayTalk(true, false, false, false));
	CHECK(VoiceGate::Idle(VoiceGate::MayTalk(true, true, false, false), Mic::kOff, false));
	CHECK(VoiceGate::Idle(VoiceGate::MayTalk(true, false, true, false), Mic::kOff, false));
	// Voice chat off in the ini: nothing either, friends or not.
	CHECK(!VoiceGate::MayTalk(false, true, true, false));
	CHECK(!VoiceGate::MayTalk(false, true, true, true));
}

TEST("voice gate: with a friend in the session, or loopback, the talk key is read")
{
	CHECK(VoiceGate::MayTalk(true, true, true, false));
	CHECK(VoiceGate::MayTalk(true, false, false, true));
	CHECK(VoiceGate::MayTalk(true, true, false, true));
	CHECK(!VoiceGate::Idle(true, Mic::kOff, false));
}

TEST("voice gate: a friend's voice still playing, or our own tail, keeps the frame working after the session")
{
	CHECK(!VoiceGate::Idle(false, Mic::kOff, true));
	CHECK(!VoiceGate::Idle(false, Mic::kTail, false));
	CHECK(!VoiceGate::Idle(false, Mic::kOn, false));
}

TEST("voice gate: the microphone starts on talking, stops into a tail, and restarts during it")
{
	CHECK(VoiceGate::Next(Mic::kOff, true) == Change::kStart);
	CHECK(VoiceGate::Next(Mic::kOn, true) == Change::kNone);
	CHECK(VoiceGate::Next(Mic::kOn, false) == Change::kStop);
	CHECK(VoiceGate::Next(Mic::kTail, false) == Change::kNone);
	CHECK(VoiceGate::Next(Mic::kTail, true) == Change::kStart);
	CHECK(VoiceGate::Next(Mic::kOff, false) == Change::kNone);
}

TEST("voice gate: Steam is asked only while it can hold our voice, and not every frame")
{
	CHECK(!VoiceGate::Ask(Mic::kOff, 1h));
	CHECK(VoiceGate::Ask(Mic::kOn, 1h));  // the first ask after starting
	CHECK(VoiceGate::Ask(Mic::kTail, 1h));
	// 144 fps: every third frame; 60 fps: every other one; 30 fps: every frame.
	CHECK(!VoiceGate::Ask(Mic::kOn, 7ms));
	CHECK(!VoiceGate::Ask(Mic::kOn, 14ms));
	CHECK(VoiceGate::Ask(Mic::kOn, 21ms));
	CHECK(!VoiceGate::Ask(Mic::kOn, 17ms));
	CHECK(VoiceGate::Ask(Mic::kOn, 33ms));
	CHECK(VoiceGate::Ask(Mic::kTail, 20ms));
}

TEST("voice gate: the tail ends when Steam says it stopped recording, or after the limit")
{
	CHECK(!VoiceGate::TailOver(VoiceGate::RESULT_OK, 100ms));
	CHECK(!VoiceGate::TailOver(VoiceGate::RESULT_NO_DATA, 100ms));
	CHECK(VoiceGate::TailOver(VoiceGate::RESULT_NOT_RECORDING, 100ms));
	CHECK(VoiceGate::TailOver(1, 0ms));  // NotInitialized: Steam won't have any
	CHECK(VoiceGate::TailOver(VoiceGate::RESULT_OK, VoiceGate::TAIL_LIMIT));
	CHECK(VoiceGate::TailOver(VoiceGate::RESULT_NO_DATA, 3s));
}
