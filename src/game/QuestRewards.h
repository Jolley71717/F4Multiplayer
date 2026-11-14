#pragma once

#include "Protocol.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Shared quest rewards (common/RewardBook.h): on a shared quest, both players get the full reward when either turns it in.
// A stage's own script already runs in every game the stage is set in; this gives friends what never reaches them that
// way: a dialogue line's reward (RewardLines.h) and a turn-in stage's that their games skip (a StageRules kPays stage),
// with its quest XP. Our game claims what our player got from those and reports it; a friend's reward the server sends
// (kQuestReward) is given here once, unless our player got the same in their own game. Main thread only (the container
// and activation sinks queue under a lock).
namespace QuestRewards
{
	// A quest by the file it comes from (StageRules::File: 0 Fallout4.esm, 1 DLCCoast.esm, 2 DLCRobot.esm, 3 DLCNukaWorld.esm)
	// and its form ID there.
	struct Local
	{
		std::uint8_t  file;
		std::uint32_t quest;
	};

	// The companions' quests: each companion's own (hire, dismissal, affinity and the gifts it brings) and their personal
	// quests. Never shared as rewards, either way: our player's pickups around one of their payment stages are not claimed
	// (MacCready's hire is paid at COMMacCreadyQuest 30/40/50), a line of theirs is not claimed (his toy soldier), and a
	// friend's reward for one is not given nor its stage marked done here. A companion is each player's own, and a stage of
	// theirs done in a friend's save would say that game hired them (the stage the game that drives a companion's quest is
	// told by). Read from the four masters' QUST records; sorted by file and form ID.
	inline constexpr Local COMPANION_QUESTS[] = {
		{ 0, 0x0229FE },  // MS07c: Long Time Coming (Nick)
		{ 0, 0x027411 },  // COMMacCreadyQuest: his hire and Long Road Ahead
		{ 0, 0x07921A },  // COMCait
		{ 0, 0x07925B },  // COMPreston
		{ 0, 0x0B9F9D },  // BoS302: Blind Betrayal (Danse)
		{ 0, 0x0BBD94 },  // COMCodsworth
		{ 0, 0x0BBD95 },  // ComX688
		{ 0, 0x0BBD96 },  // COMPiper
		{ 0, 0x0BBD97 },  // COMDanse
		{ 0, 0x0BBD98 },  // COMNick
		{ 0, 0x0BBD99 },  // COMStrong
		{ 0, 0x0BBD9A },  // COMCurie
		{ 0, 0x0BBD9B },  // COMDeacon
		{ 0, 0x0BBD9C },  // COMMacCready
		{ 0, 0x126048 },  // COMHancock
		{ 0, 0x16454E },  // COMCurieQuest: Emergent Behavior
		{ 0, 0x1666C5 },  // COMCaitQuest: Benign Intervention
		{ 1, 0x006E55 },  // DLC03_COMOldLongfellow
		{ 2, 0x0024DE },  // DLC01COMRobotCompanion
		{ 2, 0x0087BE },  // DLC01COMAda
		{ 3, 0x027715 },  // DLC04COMGage
	};

	[[nodiscard]] constexpr bool IsCompanionQuest(std::uint8_t a_file, std::uint32_t a_quest)
	{
		for (const auto& local : COMPANION_QUESTS) {
			if (local.file == a_file && local.quest == a_quest) {
				return true;
			}
		}
		return false;
	}

	// Quests whose paying stages are each player's own sessions, never a shared turn-in: the Hubologists' (their donations,
	// and the rads, rank and objective stages that are StageRules kOwn rows, which share kPays's bit). A friend's game gets
	// nothing for one, and above all its stage is not marked done there: that player has their own sessions to do. Sorted
	// by file and form ID.
	inline constexpr Local OWN_SESSIONS[] = {
		{ 3, 0x00CBD0 },  // DLC04DialogueHubologists
	};

	[[nodiscard]] constexpr bool IsOwnSessions(std::uint8_t a_file, std::uint32_t a_quest)
	{
		for (const auto& local : OWN_SESSIONS) {
			if (local.file == a_file && local.quest == a_quest) {
				return true;
			}
		}
		return false;
	}

	// Registers the event sinks (once, with the session's).
	void Install();

	// Our moments (the line our player hears, the stages seen done), the claims, and a friend's rewards due.
	void Frame(bool a_inSession);

	// A stage of a quest the session shares, done here by our player (not a friend's set here), seen now: a turn-in if
	// its StageRules row is a payment (kPays), which the friends' games never set, and its quest no companion's nor one
	// of each player's own sessions (OWN_SESSIONS).
	void OnOwnStage(std::uint32_t a_quest, std::uint16_t a_stage, std::uint8_t a_ruleFlags);

	// XP our player gained that is their own (Party's: not a friend's share given here, nor ours).
	void OnOwnXp(float a_xp);

	// A friend's reward from the server.
	void OnReward(const Protocol::QuestReward& a_reward);

	// The reports to send (a_sending: welcomed); kept, a few dozen at most, until then.
	std::vector<std::vector<std::uint8_t>> TakeOutgoing(bool a_sending);

	// The session ended (or the connection dropped): moments and gains still open go; friends' rewards not yet given here stay
	// (they are our player's), as do our reports not yet sent (a reconnect sends them).
	void OnSessionEnded();

	// Another server: its ids are new, and nothing of ours is for it.
	void OnServerChanged();

	// A load begins: a_character is the world's being left (CharacterBinding::Character).
	void BeforeLoad(std::uint64_t a_character);

	// A save is loaded (or a new game begun): what our player was turning in belonged to the world before it and goes; friends'
	// rewards not yet given stay only for the same character (RewardBook::KeepsPending). a_character: the loaded save's.
	void OnGameLoaded(bool a_newGame, std::uint64_t a_character);

	// The co-save's record of the friends' rewards judged here (RewardBook::Receiver::SaveRecord): written with each save and
	// read back with it; a save without one has none judged. Called inside the save or load (the hold keeps frames out).
	[[nodiscard]] std::vector<std::uint8_t> SaveRecord();
	void LoadRecord(std::span<const std::uint8_t> a_bytes);
	void RevertRecord();

	// For `net status`.
	[[nodiscard]] std::string Describe();
}
