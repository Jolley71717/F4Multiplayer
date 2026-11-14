#include "Test.h"
#include "game/WorkshopGraveyard.h"

#include <cstdint>
#include <vector>

// The copies of friends' pieces waiting to be deleted (src/game/WorkshopGraveyard.h): a copy removed again (a friend's
// scrap, then leaving the session) waits once, and the list holds no more than its cap.
namespace
{
	struct Waiting
	{
		std::uint32_t id = 0;
		int           placed = 0;  // which removal put it there
	};

	constexpr auto ID_OF = [](const Waiting& a_w) { return a_w.id; };
}

TEST("workshop graveyard: a copy waits once, its newer entry in place of the older")
{
	using WorkshopGraveyard::Admitted;
	std::vector<Waiting> graveyard;
	CHECK(WorkshopGraveyard::Admit(graveyard, Waiting{ 0xFF001234, 1 }, ID_OF) == Admitted::kAdded);
	CHECK(WorkshopGraveyard::Admit(graveyard, Waiting{ 0xFF005678, 1 }, ID_OF) == Admitted::kAdded);
	CHECK(WorkshopGraveyard::Admit(graveyard, Waiting{ 0xFF001234, 2 }, ID_OF) == Admitted::kAlready);
	CHECK(WorkshopGraveyard::Admit(graveyard, Waiting{ 0xFF001234, 3 }, ID_OF) == Admitted::kAlready);
	CHECK(graveyard.size() == 2);
	CHECK(graveyard[0].id == 0xFF001234);
	CHECK(graveyard[0].placed == 3);
	CHECK(graveyard[1].placed == 1);
}

TEST("workshop graveyard: the cap holds, and a copy already waiting is still taken at it")
{
	using WorkshopGraveyard::Admitted;
	std::vector<Waiting> graveyard;
	for (std::uint32_t i = 0; i < WorkshopGraveyard::MAX_WAITING; ++i) {
		CHECK(WorkshopGraveyard::Admit(graveyard, Waiting{ 0xFF000001 + i, 1 }, ID_OF) == Admitted::kAdded);
	}
	CHECK(graveyard.size() == WorkshopGraveyard::MAX_WAITING);
	CHECK(WorkshopGraveyard::Admit(graveyard, Waiting{ 0xFF100000, 1 }, ID_OF) == Admitted::kFull);
	CHECK(graveyard.size() == WorkshopGraveyard::MAX_WAITING);
	CHECK(WorkshopGraveyard::Admit(graveyard, Waiting{ 0xFF000001, 2 }, ID_OF) == Admitted::kAlready);
	CHECK(graveyard.size() == WorkshopGraveyard::MAX_WAITING);
	CHECK(graveyard.front().placed == 2);
	// One gone (deleted) makes room again.
	graveyard.erase(graveyard.begin());
	CHECK(WorkshopGraveyard::Admit(graveyard, Waiting{ 0xFF100000, 1 }, ID_OF) == Admitted::kAdded);
}
