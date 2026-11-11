// A roll held for the resolver goes to the NPC it was decided for alone (RollPairing.h, src/game/RollHook.cpp): another
// NPC's CopyFromTemplateForms on the same thread meanwhile is built as the engine rolled it, and the roll stays held for
// its owner.

#include "Test.h"

#include "game/RollPairing.h"

using RollPairing::Held;
using RollPairing::Owner;
using RollPairing::Sources;
using RollPairing::Verdict;

namespace
{
	// What a roll carries, as far as these tests need: the reference it was decided for.
	struct Roll
	{
		std::uint32_t ref = 0;
	};

	constexpr std::uintptr_t RESOLVER_SLOT = 0x0000'00A0'0012'F6C8;  // the resolver's return-address slot, as the hook sees it
	constexpr std::uintptr_t RESOLVER_ARRAY = RESOLVER_SLOT + 0xE8;   // its array of templates (rbp - 0x20)
	constexpr std::uintptr_t OTHER_ARRAY = 0x0000'00A0'0012'F0A0;     // another caller's array, deeper on the same stack

	// A leveled raider: a default template and a few slots templated on its leveled lists.
	Sources Raider()
	{
		Sources out{};
		out[0] = 0x2'0000'1000;  // the default template
		out[1] = 0x2'0000'2000;  // traits
		out[2] = 0x2'0000'2000;  // stats
		out[4] = 0x2'0000'3000;  // inventory
		return out;
	}

	// A leveled gunner: other lists.
	Sources Gunner()
	{
		Sources out{};
		out[0] = 0x2'0000'5000;
		out[1] = 0x2'0000'6000;
		out[4] = 0x2'0000'7000;
		return out;
	}

	Owner Of(std::uintptr_t a_call, const Sources& a_sources)
	{
		return Owner{ a_call, a_sources };
	}
}

TEST("RollPairing: the resolver's own array is found from its return-address slot")
{
	CHECK(RollPairing::TemplatesOf(RESOLVER_SLOT) == RESOLVER_ARRAY);
	// rsp at the call is the slot + 8; the array is rbp - 0x20 with rbp = rsp + 0x100 (sub rsp, 0x178 below rbp = entry - 0xA8).
	static_assert(RollPairing::FRAME_TO_TEMPLATES == 8 + 0x100 - 0x20);
}

TEST("RollPairing: the owner takes its roll, once")
{
	Held<Roll> held;
	held.Arm(Of(RESOLVER_ARRAY, Raider()), Roll{ 0x0001F0B1 });
	REQUIRE(held.Armed());
	const auto take = held.TakeFor(Of(RESOLVER_ARRAY, Raider()));
	CHECK(take.verdict == Verdict::kOwner);
	CHECK(take.value.ref == 0x0001F0B1);
	CHECK(!held.Armed());
	CHECK(held.TakeFor(Of(RESOLVER_ARRAY, Raider())).verdict == Verdict::kNone);
}

TEST("RollPairing: another NPC's build in between doesn't take it, and the owner still gets it")
{
	Held<Roll> held;
	held.Arm(Of(RESOLVER_ARRAY, Raider()), Roll{ 0x0001F0B1 });
	// Another caller's CopyFromTemplateForms of a gunner on this thread before the resolver's own.
	const auto stray = held.TakeFor(Of(OTHER_ARRAY, Gunner()));
	CHECK(stray.verdict == Verdict::kOtherCall);
	CHECK(stray.value.ref == 0);
	CHECK(held.Armed());
	const auto owner = held.TakeFor(Of(RESOLVER_ARRAY, Raider()));
	CHECK(owner.verdict == Verdict::kOwner);
	CHECK(owner.value.ref == 0x0001F0B1);
}

TEST("RollPairing: another NPC built from the same templates elsewhere is still another call")
{
	// Two leveled NPCs with the same lists (a male and a female raider): only the array tells them apart.
	Held<Roll> held;
	held.Arm(Of(RESOLVER_ARRAY, Raider()), Roll{ 0x0001F0B1 });
	CHECK(held.TakeFor(Of(OTHER_ARRAY, Raider())).verdict == Verdict::kOtherCall);
	CHECK(held.Armed());
}

TEST("RollPairing: the resolver's array holding another NPC's templates is another NPC")
{
	Held<Roll> held;
	held.Arm(Of(RESOLVER_ARRAY, Raider()), Roll{ 0x0001F0B1 });
	CHECK(held.TakeFor(Of(RESOLVER_ARRAY, Gunner())).verdict == Verdict::kOtherNpc);
	CHECK(held.Armed());
}

TEST("RollPairing: with the frame unknown, the templates alone decide")
{
	Held<Roll> held;
	held.Arm(Of(0, Raider()), Roll{ 0x0001F0B1 });
	CHECK(held.TakeFor(Of(OTHER_ARRAY, Gunner())).verdict == Verdict::kOtherNpc);
	CHECK(held.Armed());
	const auto take = held.TakeFor(Of(OTHER_ARRAY, Raider()));
	CHECK(take.verdict == Verdict::kOwner);
	CHECK(take.value.ref == 0x0001F0B1);
}

TEST("RollPairing: one slot's template differing is another NPC")
{
	auto other = Raider();
	other[13] = 0x2'0000'9000;  // the last slot
	CHECK(RollPairing::Judge(Of(RESOLVER_ARRAY, Raider()), Of(RESOLVER_ARRAY, other)) == Verdict::kOtherNpc);
	auto noDefault = Raider();
	noDefault[0] = 0;
	CHECK(RollPairing::Judge(Of(RESOLVER_ARRAY, Raider()), Of(RESOLVER_ARRAY, noDefault)) == Verdict::kOtherNpc);
}

TEST("RollPairing: a new resolve drops what is held")
{
	Held<Roll> held;
	CHECK(!held.Disarm());
	held.Arm(Of(RESOLVER_ARRAY, Raider()), Roll{ 0x0001F0B1 });
	CHECK(held.Disarm());
	CHECK(!held.Armed());
	CHECK(held.TakeFor(Of(RESOLVER_ARRAY, Raider())).verdict == Verdict::kNone);
}

TEST("RollPairing: nothing held, nothing taken")
{
	Held<Roll> held;
	const auto take = held.TakeFor(Of(RESOLVER_ARRAY, Raider()));
	CHECK(take.verdict == Verdict::kNone);
	CHECK(take.value.ref == 0);
}

TEST("RollPairing: a held roll lives in a thread_local without dynamic init")
{
	static constinit thread_local Held<Roll> held;
	CHECK(!held.Armed());
	held.Arm(Of(RESOLVER_ARRAY, Raider()), Roll{ 1 });
	CHECK(held.Disarm());
}
