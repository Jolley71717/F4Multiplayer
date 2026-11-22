#pragma once

#include <cstdint>

// A stand-in says nothing of its own (game/Puppets.cpp). Kept free of the game so it can be tested.
//
// A stand-in is a settler's duplicate with its AI off, but the engine still has it speak as it has any NPC speak: its hit and
// pain lines as enemies hit it, its hellos and idle chatter in the moments its own AI runs (a climb into power armor, a seat), all
// in the settler's voice (a player's report, 0.20.1). A friend's own lines reach their stand-in another way, as plain 3D sounds
// played at it (Conversations::Play), which the engine's own speaking state never shows. So the stand-in is kept from speaking
// two ways: told each frame it cannot (the actor's "can speak" flag, Actor::BOOL_FLAGS::kCanSpeak, which the engine's own
// dialogue checks before giving an NPC a line), and any line the engine has it saying all the same is ended the frame it is
// seen (its sound stopped, its voice timers and state cleared), so at most a frame of it is heard.
namespace StandInVoice
{
	// Which of the two are on (a mask; the dev channel sets it: puppetvoice).
	inline constexpr std::uint32_t kQuietFlag = 1;  // the "can speak" flag is cleared every frame
	inline constexpr std::uint32_t kEndLines = 2;   // a line the engine has it saying is ended as it is seen
	inline constexpr std::uint32_t DEFAULT = kQuietFlag | kEndLines;

	// What the engine shows of a stand-in this frame.
	struct Facts
	{
		bool talking = false;  // Actor::IsTalking
		int  voiceState = 0;   // HighProcessData::VOICE_STATE (0 none, 1 a line requested, 2 started, 3 going on)
	};

	// What is kept of one stand-in from frame to frame.
	struct Track
	{
		bool          speaking = false;  // the engine had it speaking last frame
		std::uint32_t lines = 0;         // lines it was found saying (each counted once)
		std::uint32_t ended = 0;         // ... of those, ended by us
	};

	// Whether the engine has the stand-in saying something: talking, or a line on its way (requested or started).
	constexpr bool Speaking(const Facts& a_facts)
	{
		return a_facts.talking || a_facts.voiceState != 0;
	}

	// The frame's decision: whether to end a line now. A line is counted as it is first seen; one that goes on after an end
	// that did not take is ended again each frame, counted once.
	constexpr bool Frame(Track& a_track, const Facts& a_facts, std::uint32_t a_mode)
	{
		const bool speaking = Speaking(a_facts);
		const bool end = speaking && (a_mode & kEndLines) != 0;
		if (speaking && !a_track.speaking) {
			++a_track.lines;
			a_track.ended += end;
		}
		a_track.speaking = speaking;
		return end;
	}
}
