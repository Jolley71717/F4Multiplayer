#include "game/TerminalSync.h"

#include "Config.h"
#include "game/OpenMenus.h"
#include "game/Papyrus.h"
#include "game/StageLog.h"
#include "game/Story.h"
#include "game/TerminalRules.h"
#include "game/WorldSync.h"

#include <array>
#include <format>
#include <optional>

namespace TerminalSync
{
	namespace
	{
		using Clock = TerminalRules::Clock;
		using TerminalRules::ROWS;

		constexpr auto READ_EVERY = std::chrono::milliseconds(250);

		TerminalRules::Book                    book;
		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      lastRead{};
		Clock::time_point                      terminalClosedAt{};
		bool                                   terminalWasOpen = false;
		// Each row's quest here (0: not looked up yet or not in this game), and since when it has been running here.
		std::array<std::uint32_t, ROWS.size()>                    quests{};
		std::array<std::optional<Clock::time_point>, ROWS.size()> runningSince{};
		std::uint32_t sent = 0, heard = 0, made = 0, failed = 0;

		RE::TESQuest* QuestOf(std::size_t a_row)
		{
			if (quests[a_row] == 0) {
				const auto handler = RE::TESDataHandler::GetSingleton();
				const auto quest = handler ? handler->LookupForm<RE::TESQuest>(ROWS[a_row].quest, ROWS[a_row].plugin) : nullptr;
				quests[a_row] = quest ? quest->GetFormID() : 0;
			}
			return quests[a_row] != 0 ? RE::TESForm::GetFormByID<RE::TESQuest>(quests[a_row]) : nullptr;
		}

		// The row's property as the quest's script has it now, none while the quest isn't running here (or has no script
		// object bound yet).
		std::optional<std::int32_t> ReadRow(RE::TESQuest* a_quest, const TerminalRules::Row& a_row)
		{
			if (!a_quest || !(a_quest->data.flags & StageLog::QUEST_RUNNING)) {
				return std::nullopt;
			}
			const auto object = Papyrus::BoundObject(a_quest, a_row.script, false);
			const auto var = object ? object->GetProperty(RE::BSFixedString{ a_row.property }) : nullptr;
			if (!var || !var->is<std::int32_t>()) {
				return std::nullopt;
			}
			return RE::BSScript::get<std::int32_t>(*var);
		}

		// Our player at a terminal, or just away from one (TerminalRules::CLOSE_GRACE).
		bool AtTerminal(Clock::time_point a_now)
		{
			const bool open = OpenMenus::AnyOpen({ OpenMenus::Menu::kTerminal, OpenMenus::Menu::kTerminalHolotape });
			if (terminalWasOpen && !open) {
				terminalClosedAt = a_now;
			}
			terminalWasOpen = open;
			return open || (terminalClosedAt != Clock::time_point{} && a_now - terminalClosedAt < TerminalRules::CLOSE_GRACE);
		}
	}

	void Frame()
	{
		if (!Config::Get().syncQuests || !WorldSync::InWorld() || Story::InOpening()) {
			return;
		}
		const auto now = Clock::now();
		const bool atTerminal = AtTerminal(now);
		if (now - lastRead < READ_EVERY) {
			return;
		}
		lastRead = now;
		for (std::uint8_t i = 0; i < ROWS.size(); ++i) {
			const auto& row = ROWS[i];
			const auto  quest = QuestOf(i);
			const auto  value = ReadRow(quest, row);
			if (!value) {
				runningSince[i].reset();
			} else if (!runningSince[i]) {
				runningSince[i] = now;
			}
			if (const auto send = book.Read(i, value, atTerminal)) {
				outgoing.push_back(Protocol::Encode(Protocol::TerminalChoice{ 0, i, *send }, Protocol::MessageType::kReportTerminalChoice));
				++sent;
				REX::INFO("TerminalSync: our player set {}.{} to {} at a terminal (from {}); sent", row.script, row.property, *send, *value);
			}
			const bool settled = runningSince[i] && now - *runningSince[i] >= TerminalRules::SETTLE;
			switch (book.Due(i, settled, atTerminal)) {
			case TerminalRules::Step::kWait:
				break;
			case TerminalRules::Step::kDone:
				book.Made(i, false);
				break;
			case TerminalRules::Step::kCall:
				{
					const auto pending = book.Pending(i);
					const bool called = pending && Papyrus::CallMethod(quest, row.script, row.setter, false, static_cast<std::int32_t>(*pending), true);
					if (called) {
						++made;
						REX::INFO("TerminalSync: a friend's choice made here: {}.{}({}) (ours was {})", row.script, row.setter, *pending, value ? *value : -1);
					} else {
						++failed;
						REX::WARN("TerminalSync: could not call {}.{}({}); left as our game has it", row.script, row.setter, pending ? *pending : -1);
					}
					book.Made(i, called);
					break;
				}
			}
		}
	}

	void Apply(const Protocol::TerminalChoice& a_choice)
	{
		if (!Config::Get().syncQuests || !TerminalRules::Valid(a_choice.row, a_choice.value)) {
			return;
		}
		++heard;
		const auto& row = ROWS[a_choice.row];
		REX::INFO("TerminalSync: {} {}.{} = {}: made here once the quest runs here", a_choice.playerId != 0 ? std::format("player {} set", a_choice.playerId) : std::string{ "the session has" },
			row.script, row.property, a_choice.value);
		book.Heard(a_choice.row, a_choice.value);
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void OnGameLoaded()
	{
		book.OnGameLoaded();
		runningSince = {};
		quests = {};
		terminalWasOpen = false;
		terminalClosedAt = {};
	}

	void Reset()
	{
		book.Clear();
		outgoing.clear();
		runningSince = {};
		terminalWasOpen = false;
		terminalClosedAt = {};
	}

	std::string Describe()
	{
		return std::format("terminals: sent={} heard={} made={} failed={}", sent, heard, made, failed);
	}
}
