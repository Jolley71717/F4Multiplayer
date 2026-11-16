#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// What runs differently in a session: VATS in real time, by the host's rules (HostRules; game/RealTimeVats.h), the
// group's VATS slow motion (the host's bGroupVatsSlowdown; game/VatsTime.h), and, by the player's own bHideTutorials, the
// game's tutorial hints hidden (its bShowTutorials, put back as it was after). Main thread.
namespace RealTime
{
	// Every frame. a_session: hosting, or joined to someone's.
	void Frame(bool a_session);

	// kTimeScales: who is in VATS, each game's time multiplier and the host's group VATS speed. a_self: our player id.
	void OnTimeScales(const Protocol::TimeScales& a_table, std::uint32_t a_self);

	// The session ended: friends' rows and the group speed are forgotten (the group's slow motion ends next frame).
	void OnSessionEnded();

	// Our own kReportTimeScale, when it is due (game/VatsTime.h Reporter), for the session to send.
	[[nodiscard]] std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// Dev: the game's time multiplier, its timer, VATS, the menus' pause count, the UI's timer and the game hour.
	[[nodiscard]] std::string Describe();

	// Dev (vatstime): who is in VATS and the group's slow motion. a_args "pretend on|off": our player counts as in VATS
	// for the report and the group's slow motion, without the game's VATS (a rig check of a friend's game slowing).
	[[nodiscard]] std::string VatsTimeCommand(std::string_view a_args);

	// The rate friends' games run at, for a stand-in's animation step (game/StandInTime.h, Puppets): the group's VATS
	// slow motion while it is in effect, 1 else (VatsTime::FriendsRate). Any thread.
	[[nodiscard]] float FriendsRate();

	// Whether any friend's player or world runs at another rate than FriendsRate (their Jet, their console's multiplier;
	// VatsTime::Differ): only then does Puppets' hook look up the rate a stand-in was given. Any thread.
	[[nodiscard]] bool FriendsDiffer();

	// The rate a friend's own player runs at in their game, for their stand-in's animation step, and the rate their
	// game's world runs at, for the step of the NPCs it runs (VatsTime::PlayerRate, WorldRate; a Puppets::Motion's
	// rate): FriendsRate for a friend who has said nothing, or is in VATS. Main thread.
	[[nodiscard]] float PlayerRate(std::uint32_t a_playerId);
	[[nodiscard]] float WorldRate(std::uint32_t a_playerId);

	// One line for net status: own=, friendsInVats=, group=, ownPlayer= (our player's pace in our game, as reported),
	// friendsAtOwnPace=.
	[[nodiscard]] std::string Status();
}
