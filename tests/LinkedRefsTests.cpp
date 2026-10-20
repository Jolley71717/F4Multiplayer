// Where a linked-ref entry points (game/LinkedRefs.h): by the form ID the engine may still hold in the pointer field,
// or by a pointer that names itself. Neither an unresolved entry (the Red Rocket terminal's, 0.7.1) nor a pointer left
// behind by a target that went is ever taken as a reference.

#include "Test.h"

#include "game/LinkedRefs.h"

#include <cstdint>
#include <map>
#include <optional>

namespace
{
	struct Ref
	{
		std::uint32_t id = 0;
	};

	// A game's references by ID, and the memory a pointer reads (unreadable: what a_unreadable names).
	struct Forms
	{
		std::map<std::uint32_t, Ref*> byId;
		const Ref*                    unreadable = nullptr;
		int                           reads = 0;

		Ref* Resolve(std::uintptr_t a_raw)
		{
			return LinkedRefs::Resolve<Ref>(
				a_raw,
				[&](const Ref* a_ref) -> std::optional<std::uint32_t> {
					++reads;
					return a_ref == unreadable ? std::nullopt : std::optional{ a_ref->id };
				},
				[&](std::uint32_t a_id) -> Ref* {
					const auto it = byId.find(a_id);
					return it != byId.end() ? it->second : nullptr;
				});
		}
	};

	std::uintptr_t Raw(const Ref* a_ref)
	{
		return reinterpret_cast<std::uintptr_t>(a_ref);
	}
}

TEST("linked refs: an entry the engine hasn't resolved holds the target's form ID, and is looked up by it, never read")
{
	Ref   door{ 0x00048507 };
	Forms forms;
	forms.byId[door.id] = &door;
	CHECK(forms.Resolve(0x00048507) == &door);
	CHECK(forms.reads == 0);  // an ID is no address: nothing is read through it
	CHECK(forms.Resolve(0x00012345) == nullptr);  // a target not in memory
	CHECK(forms.Resolve(0) == nullptr);
	CHECK(forms.reads == 0);
	CHECK(LinkedRefs::HoldsId(0x00048507));
	CHECK(LinkedRefs::HoldsId(0xFF000801));  // a reference made at runtime
	CHECK(!LinkedRefs::HoldsId(0));
	CHECK(!LinkedRefs::HoldsId(0x100000000ull));
}

TEST("linked refs: a resolved entry's pointer is taken only when the form it names by its own ID is that very form")
{
	// Addresses above 4 GB, as the game's are: the test's own objects may lie anywhere, so they are only read through
	// the fake reader, which the address picks.
	static Ref door{ 0x00048507 };
	static Ref other{ 0x00048507 };
	Forms      forms;
	if (Raw(&door) <= 0xFFFFFFFFull) {
		return;  // a build that puts statics below 4 GB can't stand for the game's memory
	}
	forms.byId[door.id] = &door;
	CHECK(forms.Resolve(Raw(&door)) == &door);
	// Left behind: the memory now holds another form with the door's old ID, or the door's ID now names a new form.
	CHECK(forms.Resolve(Raw(&other)) == nullptr);
	forms.byId.erase(door.id);
	CHECK(forms.Resolve(Raw(&door)) == nullptr);  // the target went: nothing by its ID
	// Memory that can't be read at all, or that says ID 0.
	forms.byId[door.id] = &door;
	forms.unreadable = &door;
	CHECK(forms.Resolve(Raw(&door)) == nullptr);
	forms.unreadable = nullptr;
	door.id = 0;
	CHECK(forms.Resolve(Raw(&door)) == nullptr);
	door.id = 0x00048507;
}
