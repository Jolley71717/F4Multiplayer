#pragma once

#include "Protocol.h"

#include <cstdint>
#include <string>
#include <vector>

// Random encounters that show an objective (a checkpoint under attack, a treasure hunt's note), shared by their run as
// well as by their actors (EncounterRunBook.h has which and how): our run that stands is sent to the friends as a
// radiant's run is (kReportRadiantStart and kReportRadiantStage, which the server relays as it does a radiant's), and a
// friend's game near it starts the same quest with its aliases forced to its copies of our actors and the same places;
// from then on each stage set in one game is set in the other once. A friend's run is followed here the same way. Under
// the session's radiant sharing (RadiantSync::Enabled). Main thread only; QuestSync and Session call it.
namespace EncounterRuns
{
	// This player's id in the session (the welcome's), 0 when none.
	void SetLocalPlayer(std::uint32_t a_id);

	// A friend's run started (kRadiantStarted), or a stage of one (kRadiantStage): true when it is an encounter's, taken
	// here; RadiantSync takes the rest.
	bool OnStart(const Protocol::RadiantStart& a_start);
	bool OnStage(const Protocol::RadiantStage& a_stage);

	// Someone joined, or a friend's game loaded a save: our runs going are sent again with their stages, each once its
	// actors are all shared (a game that knows one ignores it).
	void OnPlayerJoined();

	// A friend left the session: their runs we follow end here as their run's end would end them (RUN_END_WAIT).
	void OnPlayerLeft(std::uint32_t a_id);

	// A friend's game loaded a save: their runs we follow end here as when they leave (their load cleared their book, so
	// no end comes for them, and their copies of our actors are gone; what their loaded world runs is announced anew),
	// and our runs going are sent again, for them (OnPlayerJoined).
	void OnPlayerReloaded(std::uint32_t a_id);

	// The session ends (before Clear): the friends' runs we follow are stopped, their actors' copies gone with it.
	void EndFollowed();

	// Whether the quest's stages go by a run now (ours shared, or a friend's we follow): QuestSync sets a friend's stages
	// of it then.
	[[nodiscard]] bool Shares(std::uint32_t a_quest);

	// Whether our game follows a friend's run of the quest (or is starting it): our own actors of it are not what stands
	// there, and are removed (Spawns).
	[[nodiscard]] bool Follows(std::uint32_t a_quest);

	// Shares our runs that stand, starts and follows friends' and passes stages both ways. a_inSession: welcomed.
	void Frame(bool a_inSession);

	// The messages to send (QuestSync's outbox takes them, as the story's).
	std::vector<std::vector<std::uint8_t>> TakeOutgoing();

	// The session ended, another server, a save loaded: the runs are of another world.
	void Clear();

	std::string Describe();
}
