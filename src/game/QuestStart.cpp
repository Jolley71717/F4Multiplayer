#include "game/QuestStart.h"

#include "MainThread.h"

#include <array>
#include <cstring>
#include <format>
#include <optional>

namespace QuestStart
{
	namespace
	{
		// 1.11.240, by Address Library ID (QuestStartRules.h: how each was read).
		constexpr REL::ID ENSURE_STARTED{ 2207742 };  // TESQuest::EnsureQuestStarted(bool& a_starting, bool a_now) (+66B440)
		constexpr REL::ID FILL_ALIASES{ 2207786 };    // TESQuest::FillAliases (+66EE80): called by it at +0x4F, with no event
		constexpr REL::ID BEGIN_START_UP{ 2214209 };  // BGSStoryTeller::BeginStartUpQuest (+7FF280): called by it at +0x62
		static_assert(ENSURE_STARTED.id() == QuestStartRules::ENSURE_STARTED);
		static_assert(FILL_ALIASES.id() == QuestStartRules::FILL_ALIASES);
		static_assert(BEGIN_START_UP.id() == QuestStartRules::BEGIN_START_UP);

		std::optional<bool> asRead;  // checked at the first start
		std::uint32_t       asked = 0;
		std::uint32_t       started = 0;
		std::uint32_t       refused = 0;  // before the engine was asked
		std::uint32_t       failed = 0;   // the engine was asked and didn't start it
		std::uint32_t       notEnabled = 0;  // ... of them, those it said it started (QuestStartRules::Outcome::kNotEnabled)

		// Whether the engine's function is the one that was read, looked at once.
		bool AsRead()
		{
			if (asRead) {
				return *asRead;
			}
			const auto                                         at = ENSURE_STARTED.address();
			std::array<std::uint8_t, QuestStartRules::BEGINS.size()> begins{};
			std::array<std::uint8_t, 5>                        fillCall{};
			std::array<std::uint8_t, 5>                        beginCall{};
			std::memcpy(begins.data(), reinterpret_cast<const void*>(at), begins.size());
			std::memcpy(fillCall.data(), reinterpret_cast<const void*>(at + QuestStartRules::FILL_CALL), fillCall.size());
			std::memcpy(beginCall.data(), reinterpret_cast<const void*>(at + QuestStartRules::BEGIN_CALL), beginCall.size());
			asRead = QuestStartRules::AsRead(begins, fillCall, at + QuestStartRules::FILL_CALL, FILL_ALIASES.address(), beginCall, at + QuestStartRules::BEGIN_CALL,
				BEGIN_START_UP.address());
			if (!*asRead) {
				REX::WARN("QuestStart: the game's quest start (+{:X}) is not as 1.11.240 has it (another plugin's patch?); no quest is started through it, so friends' radiant runs and the host's settlement attacks aren't started here",
					ENSURE_STARTED.offset());
			}
			return *asRead;
		}

		QuestStartRules::Facts FactsOf(const RE::TESQuest* a_quest)
		{
			QuestStartRules::Facts facts;
			facts.flags = a_quest->data.flags;
			facts.promoting = a_quest->promoteTask != nullptr;
			facts.refsAliased = a_quest->totalRefsAliased;
			facts.locsAliased = static_cast<std::uint32_t>(a_quest->aliasedLocMap.size());
			facts.eventData = a_quest->startEventData != nullptr;
			facts.alreadyRun = a_quest->alreadyRun;
			return facts;
		}
	}

	QuestStartRules::Verdict Judge(const RE::TESQuest* a_quest)
	{
		return QuestStartRules::Judge(FactsOf(a_quest));
	}

	Result Start(RE::TESQuest* a_quest)
	{
		MainThread::Assert("QuestStart::Start");
		++asked;
		if (!a_quest) {
			++refused;
			return { false, false, "no such quest" };
		}
		if (!AsRead()) {
			++refused;
			return { false, false, "the game's quest start is not as 1.11.240 has it" };
		}
		const auto verdict = Judge(a_quest);
		if (verdict != QuestStartRules::Verdict::kStart) {
			++refused;
			return { false, QuestStartRules::Waits(verdict), std::string{ QuestStartRules::Why(verdict) } };
		}
		using ensure_t = bool(RE::TESQuest*, bool&, bool);
		static REL::Relocation<ensure_t> ensure{ ENSURE_STARTED };
		bool       starting = false;
		const bool said = ensure(a_quest, starting, true);
		const auto outcome = QuestStartRules::After(said, a_quest->data.flags);
		if (outcome != QuestStartRules::Outcome::kStarted) {
			++failed;
			if (outcome == QuestStartRules::Outcome::kNotEnabled && ++notEnabled == 1) {
				// Not what was read (the engine enables a quest it starts before it returns): told once, with what the quest holds
				// now, since a start that left fills on it is refused as kFilled until they are cleared.
				const auto after = FactsOf(a_quest);
				REX::WARN("QuestStart: the game said it started {:08X}, but it isn't running (flags {:04X}, {} reference(s) and {} location(s) aliased, event data {}): "
						  "not as 1.11.240 was read; counted as not started (told once)",
					a_quest->GetFormID(), after.flags, after.refsAliased, after.locsAliased, after.eventData);
			}
			return { false, false, std::string{ QuestStartRules::Why(outcome) } };
		}
		++started;
		return { true, false, {} };
	}

	std::string Describe()
	{
		return std::format("quest starts: the game's start {}; {} asked, {} started, {} not asked of the game, {} it didn't start ({} said started but not running)",
			asRead ? (*asRead ? "is as read" : "is NOT as read (none made)") : "not looked at yet", asked, started, refused, failed, notEnabled);
	}
}
