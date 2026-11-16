// Where a linked-ref entry points (game/LinkedRefs.h): by the form ID the engine may still hold in the pointer field,
// or by a pointer that names itself. Neither an unresolved entry (the Red Rocket terminal's, 0.7.1) nor a pointer left
// behind by a target that went is ever taken as a reference, nor read for the ID it names (the host's wsinfo scan near
// Sanctuary crashed reading a link that held 000AE0F2 as a pointer, 2026-11-16).

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

		std::optional<std::uint32_t> Read(const Ref* a_ref)
		{
			++reads;
			return a_ref == unreadable ? std::nullopt : std::optional{ a_ref->id };
		}

		Ref* Find(std::uint32_t a_id) const
		{
			const auto it = byId.find(a_id);
			return it != byId.end() ? it->second : nullptr;
		}

		Ref* Resolve(std::uintptr_t a_raw)
		{
			return LinkedRefs::Resolve<Ref>(a_raw, [&](const Ref* a_ref) { return Read(a_ref); }, [&](std::uint32_t a_id) { return Find(a_id); });
		}

		std::uint32_t TargetId(std::uintptr_t a_raw)
		{
			return LinkedRefs::TargetId<Ref>(a_raw, [&](const Ref* a_ref) { return Read(a_ref); }, [&](std::uint32_t a_id) { return Find(a_id); });
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

TEST("linked refs: the ID an entry names is the one it holds, read through nothing, whether or not its reference is loaded")
{
	// The host's crash: a static near Sanctuary linked to 000AE0F2, held as the ID. A log names it; nothing reads at it.
	Forms forms;
	CHECK(forms.TargetId(0x000AE0F2) == 0x000AE0F2u);
	CHECK(forms.Resolve(0x000AE0F2) == nullptr);  // not in memory: no reference, and no crash
	Ref piece{ 0x000AE0F2 };
	forms.byId[piece.id] = &piece;
	CHECK(forms.TargetId(0x000AE0F2) == 0x000AE0F2u);
	CHECK(forms.Resolve(0x000AE0F2) == &piece);
	CHECK(forms.TargetId(0xFF001234) == 0xFF001234u);  // a runtime reference's ID
	CHECK(forms.TargetId(0) == 0u);
	CHECK(forms.reads == 0);
}

TEST("linked refs: the ID a pointer names is its target's only when the pointer names itself; a stale one names nothing")
{
	static Ref bench{ 0x000250FE };
	static Ref other{ 0x000250FE };
	Forms      forms;
	if (Raw(&bench) <= 0xFFFFFFFFull) {
		return;  // as above: the test's statics must stand above 4 GB, as the game's memory does
	}
	forms.byId[bench.id] = &bench;
	CHECK(forms.TargetId(Raw(&bench)) == 0x000250FEu);
	CHECK(forms.TargetId(Raw(&other)) == 0u);  // memory that says the bench's ID but isn't the bench
	forms.unreadable = &bench;
	CHECK(forms.TargetId(Raw(&bench)) == 0u);  // memory that can't be read
	forms.unreadable = nullptr;
	forms.byId.erase(bench.id);
	CHECK(forms.TargetId(Raw(&bench)) == 0u);  // the target went
}
