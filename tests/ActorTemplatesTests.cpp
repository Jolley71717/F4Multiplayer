#include "Test.h"
#include "game/ActorTemplates.h"

#include <map>

// Whether an actor base is a turret or robot workshop mode builds, following its templates.
namespace
{
	using ActorTemplates::Form;
	using ActorTemplates::Why;

	struct Forms
	{
		std::map<std::uint32_t, Form> forms;
		int                           looks = 0;

		std::optional<Form> operator()(std::uint32_t a_id)
		{
			++looks;
			const auto it = forms.find(a_id);
			return it != forms.end() ? std::optional{ it->second } : std::nullopt;
		}
	};

	Form Npc(bool a_builds, std::uint16_t a_flags = 0, std::uint32_t a_template = 0)
	{
		Form form;
		form.kind = Form::Kind::kNpc;
		form.builds = a_builds;
		form.useFlags = a_flags;
		form.baseTemplate = a_template;
		return form;
	}

	Form List(std::vector<std::uint32_t> a_entries)
	{
		Form form;
		form.kind = Form::Kind::kList;
		form.entries = std::move(a_entries);
		return form;
	}

	constexpr std::uint32_t TURRET_USER = 0x000B3036;  // the host's machinegun turret, as the rig saw it refused
	constexpr std::uint32_t TURRET_LIST = 0x000B3030;
	constexpr std::uint32_t TURRET_1 = 0x000B3031;
	constexpr std::uint32_t TURRET_2 = 0x000B3032;
	constexpr std::uint32_t RAIDER = 0x0001F0C8;
}

TEST("actor templates: a turret's own record (or its race) says so: no template needed")
{
	Forms forms;
	forms.forms[TURRET_1] = Npc(true);
	const auto verdict = ActorTemplates::Builds(TURRET_1, forms);
	CHECK(verdict.builds);
	CHECK(verdict.why == Why::kBuilds);
	CHECK(verdict.decidedAt == TURRET_1);
}

TEST("actor templates: the templated turret the friend's game refused is a turret through its traits template")
{
	Forms forms;
	forms.forms[TURRET_USER] = Npc(false, ActorTemplates::TRAITS_FLAG, TURRET_LIST);
	forms.forms[TURRET_LIST] = List({ TURRET_1, TURRET_2 });
	forms.forms[TURRET_1] = Npc(true);
	forms.forms[TURRET_2] = Npc(true);
	CHECK(ActorTemplates::Builds(TURRET_USER, forms).builds);
}

TEST("actor templates: the keywords template counts too, and a flag's own slot comes before the default template")
{
	Forms forms;
	auto  user = Npc(false, ActorTemplates::KEYWORDS_FLAG, RAIDER);
	user.slots[ActorTemplates::KEYWORDS_SLOT] = TURRET_1;
	forms.forms[TURRET_USER] = user;
	forms.forms[TURRET_1] = Npc(true);
	forms.forms[RAIDER] = Npc(false);
	CHECK(ActorTemplates::Builds(TURRET_USER, forms).builds);

	// The other way round: the slot names a raider, the default template a turret: the slot is what the engine uses.
	user.slots[ActorTemplates::KEYWORDS_SLOT] = RAIDER;
	user.baseTemplate = TURRET_1;
	forms.forms[TURRET_USER] = user;
	CHECK(!ActorTemplates::Builds(TURRET_USER, forms).builds);
}

TEST("actor templates: a template for something else (the inventory) says nothing of the race")
{
	Forms forms;
	auto  user = Npc(false, 1u << 8);  // kInventory only
	user.slots[8] = TURRET_1;
	forms.forms[TURRET_USER] = user;
	forms.forms[TURRET_1] = Npc(true);
	CHECK(!ActorTemplates::Builds(TURRET_USER, forms).builds);
}

TEST("actor templates: with no flag at all the default template is still followed (a leveled creature can read as using none)")
{
	Forms forms;
	forms.forms[TURRET_USER] = Npc(false, 0, TURRET_1);
	forms.forms[TURRET_1] = Npc(true);
	CHECK(ActorTemplates::Builds(TURRET_USER, forms).builds);
}

TEST("actor templates: a leveled list is a turret only if every entry is: one that could roll a raider is refused")
{
	Forms forms;
	forms.forms[TURRET_USER] = Npc(false, ActorTemplates::TRAITS_FLAG, TURRET_LIST);
	forms.forms[TURRET_LIST] = List({ TURRET_1, RAIDER });
	forms.forms[TURRET_1] = Npc(true);
	forms.forms[RAIDER] = Npc(false);
	const auto verdict = ActorTemplates::Builds(TURRET_USER, forms);
	CHECK(!verdict.builds);
	CHECK(verdict.why == Why::kNot);
	CHECK(verdict.decidedAt == RAIDER);
}

TEST("actor templates: an empty list, a form not in this game or a template of another kind is no")
{
	Forms forms;
	forms.forms[TURRET_USER] = Npc(false, ActorTemplates::TRAITS_FLAG, TURRET_LIST);
	forms.forms[TURRET_LIST] = List({});
	CHECK(ActorTemplates::Builds(TURRET_USER, forms).why == Why::kEmptyList);

	forms.forms[TURRET_LIST] = List({ 0x12345678 });
	const auto unknown = ActorTemplates::Builds(TURRET_USER, forms);
	CHECK(!unknown.builds);
	CHECK(unknown.why == Why::kUnknown);
	CHECK(unknown.decidedAt == 0x12345678);

	forms.forms[TURRET_LIST] = Form{};  // kOther
	CHECK(!ActorTemplates::Builds(TURRET_USER, forms).builds);
	CHECK(!ActorTemplates::Builds(0, forms).builds);
}

TEST("actor templates: a loop of templates ends, refused")
{
	Forms forms;
	forms.forms[TURRET_USER] = Npc(false, ActorTemplates::TRAITS_FLAG, TURRET_LIST);
	forms.forms[TURRET_LIST] = List({ TURRET_USER });
	const auto verdict = ActorTemplates::Builds(TURRET_USER, forms);
	CHECK(!verdict.builds);
	CHECK(verdict.why == Why::kLoop);

	forms.forms[TURRET_USER] = Npc(false, ActorTemplates::TRAITS_FLAG, TURRET_USER);  // its own template
	CHECK(ActorTemplates::Builds(TURRET_USER, forms).why == Why::kLoop);
}

TEST("actor templates: the same turret twice in one list is no loop")
{
	Forms forms;
	forms.forms[TURRET_LIST] = List({ TURRET_1, TURRET_1 });
	forms.forms[TURRET_1] = Npc(true);
	CHECK(ActorTemplates::Builds(TURRET_LIST, forms).builds);
}

TEST("actor templates: a chain deeper than MAX_DEPTH is not followed to its end, and refused")
{
	Forms forms;
	const std::uint32_t first = 0x00100000;
	for (std::uint32_t i = 0; i < ActorTemplates::MAX_DEPTH + 2; ++i) {
		forms.forms[first + i] = Npc(false, ActorTemplates::TRAITS_FLAG, first + i + 1);
	}
	forms.forms[first + ActorTemplates::MAX_DEPTH + 2] = Npc(true);
	const auto verdict = ActorTemplates::Builds(first, forms);
	CHECK(!verdict.builds);
	CHECK(verdict.why == Why::kTooDeep);
	CHECK(forms.looks <= static_cast<int>(ActorTemplates::MAX_DEPTH));
}

TEST("actor templates: a wide list is looked at no further than MAX_FORMS forms")
{
	Forms                      forms;
	std::vector<std::uint32_t> entries;
	for (std::uint32_t i = 0; i < 200; ++i) {
		entries.push_back(0x00200000 + i);
		forms.forms[0x00200000 + i] = Npc(true);
	}
	forms.forms[TURRET_LIST] = List(entries);
	const auto verdict = ActorTemplates::Builds(TURRET_LIST, forms);
	CHECK(!verdict.builds);
	CHECK(verdict.why == Why::kTooDeep);
	CHECK(forms.looks <= static_cast<int>(ActorTemplates::MAX_FORMS));
}

TEST("actor templates: a turret template among the race's sources is enough when the keywords' template is a plain NPC")
{
	Forms forms;
	auto  user = Npc(false, ActorTemplates::TRAITS_FLAG | ActorTemplates::KEYWORDS_FLAG);
	user.slots[ActorTemplates::TRAITS_SLOT] = TURRET_1;
	user.slots[ActorTemplates::KEYWORDS_SLOT] = RAIDER;
	forms.forms[TURRET_USER] = user;
	forms.forms[TURRET_1] = Npc(true);
	forms.forms[RAIDER] = Npc(false);
	CHECK(ActorTemplates::Builds(TURRET_USER, forms).builds);
	CHECK(ActorTemplates::Sources(user).size() == 2);
}
