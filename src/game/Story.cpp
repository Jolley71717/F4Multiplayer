#include "game/Story.h"

namespace Story
{
	namespace
	{
		constexpr std::uint32_t OUT_OF_TIME = 0x0001CC2A;      // MQ102, starts when the player wakes up in Vault 111
		constexpr std::uint16_t EXIT_VAULT_OBJECTIVE = 1;      // "Exit Vault 111"
		constexpr std::uint16_t GO_HOME_STAGE = 10;            // past the vault door
		constexpr std::uint32_t PREWAR_WORLDSPACE = 0x000A7FF4;  // SanctuaryHillsWorld (before the bombs)
		constexpr std::array<std::uint32_t, 2> VAULT_111_LOCATIONS{ 0x0001F3FE, 0x000CA853 };

		constexpr std::uint16_t QUEST_COMPLETED = 0x0002;

		bool    shared = false;
		Opening opening = Opening::kNone;

		bool PastTheVault()
		{
			const auto quest = RE::TESForm::GetFormByID<RE::TESQuest>(OUT_OF_TIME);
			if (!quest || (quest->data.flags & QUEST_COMPLETED) || quest->currentStage >= GO_HOME_STAGE) {
				return true;
			}
			return std::ranges::any_of(quest->objectives, [](const RE::BGSQuestObjective* a_objective) {
				return a_objective && a_objective->index == EXIT_VAULT_OBJECTIVE && (a_objective->state & 2);  // completed (shown or not)
			});
		}

		// Where the opening happens. (A mod that starts the game elsewhere skips it: the player
		// is never here, so they aren't held back even though Out of Time never starts.)
		bool InOpeningPlace(RE::PlayerCharacter* a_player)
		{
			const auto cell = a_player->GetParentCell();
			if (cell && cell->worldSpace && cell->worldSpace->GetFormID() == PREWAR_WORLDSPACE) {
				return true;
			}
			for (auto location = a_player->currentLocation; location; location = location->parentLoc) {
				if (std::ranges::contains(VAULT_111_LOCATIONS, location->GetFormID())) {
					return true;
				}
			}
			return false;
		}

		bool Compute()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetParentCell() && !PastTheVault() && InOpeningPlace(player);
		}
	}

	void SetShared(bool a_shared)
	{
		shared = a_shared;
	}

	bool Shared()
	{
		return shared;
	}

	bool IsStoryType(std::int8_t a_type)
	{
		return (a_type >= 1 && a_type <= 5) || a_type >= 8;
	}

	bool InOpening()
	{
		return opening != Opening::kNone;
	}

	Opening Why()
	{
		return opening;
	}

	void Frame(bool a_unnamed)
	{
		const auto now = Compute() ? Opening::kVault : a_unnamed ? Opening::kUnnamed : Opening::kNone;
		if (now != opening) {
			if (now == Opening::kNone || opening == Opening::kNone) {
				REX::INFO("Story: {} the opening{}", now != Opening::kNone ? "in" : "out of", now == Opening::kUnnamed ? " (a new character with no name yet)" : "");
			} else if (now == Opening::kUnnamed) {
				REX::INFO("Story: still in the opening (a new character with no name yet)");
			}
			opening = now;
		}
	}

	std::string Describe()
	{
		return std::format("story: {} opening={}{}", shared ? "shared" : "own", InOpening(), opening == Opening::kUnnamed ? " (unnamed)" : "");
	}
}
