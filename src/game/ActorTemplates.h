#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// Whether an actor base is one workshop mode builds (a turret, or with Automatron a robot), following its templates. The
// keywords that say so (ActorTypeTurret, ActorTypeRobot) are the race's, and a templated NPC takes its race (traits) and its
// keywords from its template: the record itself may carry neither. The friend's game refused a copy of the host's machinegun
// turret so ("its base 000B3036 is NPC_"): the base asked was the template's user, not the turret its template makes. The
// chain is followed here, NPC to NPC and through leveled lists, bounded and loop-safe: what can't be judged is no.
// Free of the game, so it is unit tested; WorkshopSync reads the forms for it (Form).
namespace ActorTemplates
{
	inline constexpr std::size_t   SLOTS = 13;            // TESActorBaseData::templateForms, one per template use flag
	inline constexpr std::size_t   TRAITS_SLOT = 0;       // ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kTraits (the race)
	inline constexpr std::size_t   KEYWORDS_SLOT = 12;    // ...::kKeywords
	inline constexpr std::uint16_t TRAITS_FLAG = 1u << TRAITS_SLOT;
	inline constexpr std::uint16_t KEYWORDS_FLAG = 1u << KEYWORDS_SLOT;
	inline constexpr std::size_t   MAX_DEPTH = 8;         // templates of templates, leveled lists of lists
	inline constexpr std::size_t   MAX_FORMS = 64;        // forms looked at for one base, all branches together

	// One form of the chain, as the game has it.
	struct Form
	{
		enum class Kind
		{
			kNpc,
			kList,   // a leveled actor list (TESLevCharacter)
			kOther,  // anything else: no actor workshop mode builds
		};

		Kind                               kind = Kind::kOther;
		bool                               builds = false;  // an NPC: it or its race has ActorTypeTurret or ActorTypeRobot
		std::uint16_t                      useFlags = 0;    // an NPC: its template use flags
		std::uint32_t                      baseTemplate = 0;  // an NPC: its default template (TPLT)
		std::array<std::uint32_t, SLOTS>   slots{};         // an NPC: the template of each flag, where one is set (TPTA)
		std::vector<std::uint32_t>         entries;         // a leveled list: its entries' forms
	};

	// The templates an NPC takes what says "turret" from: its traits' (the race) and its keywords', each the flag's own slot or
	// the default template; none without either flag, unless it has a default template and no flag at all (a leveled
	// creature can read as using none: Rolls.cpp), which is then followed.
	inline std::vector<std::uint32_t> Sources(const Form& a_npc)
	{
		std::vector<std::uint32_t> out;
		const auto add = [&](std::uint32_t a_id) {
			if (a_id != 0 && std::find(out.begin(), out.end(), a_id) == out.end()) {
				out.push_back(a_id);
			}
		};
		if (a_npc.useFlags & TRAITS_FLAG) {
			add(a_npc.slots[TRAITS_SLOT] != 0 ? a_npc.slots[TRAITS_SLOT] : a_npc.baseTemplate);
		}
		if (a_npc.useFlags & KEYWORDS_FLAG) {
			add(a_npc.slots[KEYWORDS_SLOT] != 0 ? a_npc.slots[KEYWORDS_SLOT] : a_npc.baseTemplate);
		}
		if (a_npc.useFlags == 0) {
			add(a_npc.baseTemplate);
		}
		return out;
	}

	enum class Why
	{
		kBuilds,     // a turret or a robot (its own record, or the template that gives it its race or keywords)
		kNot,        // no turret or robot anywhere its race and keywords come from
		kUnknown,    // a form of the chain isn't in this game
		kEmptyList,  // a leveled list with no entries
		kLoop,       // the chain comes back to a form it passed through
		kTooDeep,    // past MAX_DEPTH or MAX_FORMS: not judged
	};

	struct Verdict
	{
		bool          builds = false;
		Why           why = Why::kNot;
		std::uint32_t decidedAt = 0;  // the form the verdict came from (the turret found, or the one that failed)
	};

	namespace detail
	{
		template <class Look>
		Verdict Walk(std::uint32_t a_id, Look& a_look, std::vector<std::uint32_t>& a_path, std::size_t& a_seen)
		{
			if (a_path.size() >= MAX_DEPTH || a_seen >= MAX_FORMS) {
				return { false, Why::kTooDeep, a_id };
			}
			if (std::find(a_path.begin(), a_path.end(), a_id) != a_path.end()) {
				return { false, Why::kLoop, a_id };
			}
			++a_seen;
			const std::optional<Form> form = a_id != 0 ? a_look(a_id) : std::nullopt;
			if (!form) {
				return { false, Why::kUnknown, a_id };
			}
			a_path.push_back(a_id);
			Verdict verdict{ false, Why::kNot, a_id };
			switch (form->kind) {
			case Form::Kind::kNpc:
				if (form->builds) {
					verdict = { true, Why::kBuilds, a_id };
					break;
				}
				// Any template its race or keywords come from that makes a turret: the engine builds it from that one.
				for (bool first = true; const auto source : Sources(*form)) {
					const auto from = Walk(source, a_look, a_path, a_seen);
					if (from.builds) {
						verdict = from;
						break;
					}
					if (std::exchange(first, false)) {
						verdict = from;  // the first failure says why
					}
				}
				break;
			case Form::Kind::kList:
				// A leveled list builds only if every entry does: one that could roll a raider is no workshop piece.
				if (form->entries.empty()) {
					verdict = { false, Why::kEmptyList, a_id };
					break;
				}
				verdict = { true, Why::kBuilds, a_id };
				for (const auto entry : form->entries) {
					const auto from = Walk(entry, a_look, a_path, a_seen);
					if (!from.builds) {
						verdict = from;
						break;
					}
				}
				break;
			case Form::Kind::kOther:
				break;
			}
			a_path.pop_back();
			return verdict;
		}
	}

	// Whether the actor base a_id is a turret or robot by its template chain. a_look(id) gives the form under the id, or nothing.
	template <class Look>
	Verdict Builds(std::uint32_t a_id, Look&& a_look)
	{
		std::vector<std::uint32_t> path;
		std::size_t                seen = 0;
		return detail::Walk(a_id, a_look, path, seen);
	}

	constexpr const char* WhyName(Why a_why)
	{
		switch (a_why) {
		case Why::kBuilds:
			return "a turret or robot";
		case Why::kNot:
			return "no turret or robot race or keyword";
		case Why::kUnknown:
			return "a template not in this game";
		case Why::kEmptyList:
			return "an empty leveled list";
		case Why::kLoop:
			return "a template loop";
		case Why::kTooDeep:
			return "templates too deep to follow";
		}
		return "?";
	}
}
