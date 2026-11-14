#include "Test.h"
#include "game/WorkshopGraveyard.h"

#include <cstdint>
#include <vector>

// The copies of friends' pieces waiting to be deleted (src/game/WorkshopGraveyard.h): a copy removed again (a friend's
// scrap, then leaving the session) waits once; a big settlement's every copy, buried at once, all wait.
namespace
{
	struct Waiting
	{
		std::uint32_t id = 0;
		int           placed = 0;  // which removal put it there
	};

	struct IdOf
	{
		std::uint32_t operator()(const Waiting& a_w) const { return a_w.id; }
	};

	using Graveyard = WorkshopGraveyard::List<Waiting, IdOf>;
}

TEST("workshop graveyard: a copy waits once, its newer entry in place of the older")
{
	using WorkshopGraveyard::Admitted;
	Graveyard graveyard;
	CHECK(graveyard.Admit({ 0xFF001234, 1 }) == Admitted::kAdded);
	CHECK(graveyard.Admit({ 0xFF005678, 1 }) == Admitted::kAdded);
	CHECK(graveyard.Admit({ 0xFF001234, 2 }) == Admitted::kAlready);
	CHECK(graveyard.Admit({ 0xFF001234, 3 }) == Admitted::kAlready);
	CHECK(graveyard.size() == 2);
	CHECK(graveyard.Contains(0xFF001234));
	CHECK(graveyard.Contains(0xFF005678));
	CHECK(!graveyard.Contains(0xFF009999));
	std::vector<Waiting> seen(graveyard.begin(), graveyard.end());
	CHECK(seen[0].id == 0xFF001234 && seen[0].placed == 3);
	CHECK(seen[1].id == 0xFF005678 && seen[1].placed == 1);
}

TEST("workshop graveyard: those deleted go, the rest are still found by ID, and one may wait again")
{
	using WorkshopGraveyard::Admitted;
	Graveyard graveyard;
	for (std::uint32_t i = 1; i <= 10; ++i) {
		graveyard.Admit({ 0xFF000000 + i, 1 });
	}
	CHECK(graveyard.EraseIf([](Waiting& a_w) { return a_w.id % 2 == 0; }) == 5);
	CHECK(graveyard.size() == 5);
	CHECK(!graveyard.Contains(0xFF000002));
	CHECK(graveyard.Contains(0xFF000009));
	// The index follows the moved entries: a copy waiting again replaces its own entry, not another's.
	CHECK(graveyard.Admit({ 0xFF000009, 2 }) == Admitted::kAlready);
	CHECK(graveyard.Admit({ 0xFF000002, 2 }) == Admitted::kAdded);
	CHECK(graveyard.size() == 6);
	int nines = 0;
	for (const auto& w : graveyard) {
		nines += w.id == 0xFF000009 ? 1 : 0;
		CHECK(w.id != 0xFF000009 || w.placed == 2);
	}
	CHECK(nines == 1);
	graveyard.Clear();
	CHECK(graveyard.empty());
	CHECK(!graveyard.Contains(0xFF000001));
}

TEST("workshop graveyard: leaving a settlement of 22048 copies buries them all, each once")
{
	using WorkshopGraveyard::Admitted;
	constexpr std::uint32_t COPIES = 22048;  // WorkshopAudit::MAX_KEPT: the most the server keeps
	Graveyard               graveyard;
	std::uint32_t           added = 0;
	for (std::uint32_t i = 0; i < COPIES; ++i) {
		added += graveyard.Admit({ 0xFF000001 + i, 1 }) == Admitted::kAdded ? 1u : 0u;
	}
	// Leaving after a scrap: every copy removed again.
	for (std::uint32_t i = 0; i < COPIES; ++i) {
		CHECK(graveyard.Admit({ 0xFF000001 + i, 2 }) == Admitted::kAlready);
	}
	CHECK(added == COPIES);
	CHECK(graveyard.size() == COPIES);
}
