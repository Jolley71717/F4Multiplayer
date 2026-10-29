// Which game the plugin is in and what its F4SE offers (src/RuntimeSupport.h): one DLL for old-gen 1.10.163 and the
// Anniversary update 1.11.240, chosen at load.

#include "Test.h"

#include "RuntimeSupport.h"

#include <array>

using namespace RuntimeSupport;

TEST("RuntimeSupport: F4SE's packed versions unpack")
{
	// F4SE 0.6.23 and 0.7.9, and the games 1.10.163 and 1.11.240, as F4SE packs them.
	CHECK((Unpack(0x00060170) == Version{ 0, 6, 23, 0 }));
	CHECK((Unpack(0x00070090) == Version{ 0, 7, 9, 0 }));
	CHECK((Unpack(0x010A0A30) == Version{ 1, 10, 163, 0 }));
	CHECK((Unpack(0x010B0F00) == Version{ 1, 11, 240, 0 }));
}

TEST("RuntimeSupport: each game version is its runtime, and no other")
{
	CHECK(KindOf({ 1, 10, 163, 0 }) == Kind::kOG);
	CHECK(KindOf({ 1, 10, 984, 0 }) == Kind::kNG);
	CHECK(KindOf({ 1, 10, 980, 0 }) == Kind::kNG);
	CHECK(KindOf({ 1, 11, 240, 0 }) == Kind::kAE);
	CHECK(KindOf({ 1, 11, 137, 0 }) == Kind::kAE);
	CHECK(!KindOf({ 1, 10, 162, 0 }));
	CHECK(!KindOf({ 1, 11, 241, 0 }));
	CHECK(!KindOf({ 1, 9, 4, 0 }));
	CHECK(Name(Kind::kOG) == "old-gen");
	CHECK(Name(Kind::kAE) == "Anniversary");
}

TEST("RuntimeSupport: the old F4SE's query says yes on old-gen only")
{
	CHECK(QueryAccepts({ 1, 10, 163, 0 }, false));
	CHECK(!QueryAccepts({ 1, 10, 163, 0 }, true));   // the Creation Kit
	CHECK(!QueryAccepts({ 1, 10, 984, 0 }, false));  // next-gen: not declared yet
	CHECK(!QueryAccepts({ 1, 11, 240, 0 }, false));  // the version data's business
	CHECK(!QueryAccepts({ 1, 10, 162, 0 }, false));
}

TEST("RuntimeSupport: the save folder's name is asked of F4SE 0.7.1 and later only")
{
	CHECK(!HasSaveFolderName({ 0, 6, 23, 0 }));
	CHECK(!HasSaveFolderName({ 0, 7, 0, 0 }));
	CHECK(HasSaveFolderName({ 0, 7, 1, 0 }));
	CHECK(HasSaveFolderName({ 0, 7, 9, 0 }));
	CHECK(DEFAULT_SAVE_FOLDER == "Fallout4");
}

TEST("RuntimeSupport: permanent tasks come with F4SE 0.7, whatever the task interface's version says")
{
	// 0.6.23's task interface says version 2 too, without AddTaskPermanent: only F4SE's own version tells them apart.
	CHECK(!HasPermanentTasks({ 0, 6, 23, 0 }));  // old-gen
	CHECK(!HasPermanentTasks(Unpack(0x00060170)));
	CHECK(HasPermanentTasks({ 0, 7, 0, 0 }));
	CHECK(HasPermanentTasks({ 0, 7, 2, 0 }));     // next-gen
	CHECK(HasPermanentTasks(Unpack(0x00070090)));  // the Anniversary update's 0.7.9: the permanent task, never our thread
	CHECK(HasPermanentTasks({ 1, 0, 0, 0 }));
}

TEST("RuntimeSupport: an Address Library ID is found only when it is there")
{
	constexpr std::array<Mapping, 4> table{ { { 10, 0x1000 }, { 11, 0x1010 }, { 20, 0x2000 }, { 4796064, 0x30E6E50 } } };
	CHECK(Find(table, 10) == 0x1000u);
	CHECK(Find(table, 20) == 0x2000u);
	CHECK(Find(table, 4796064) == 0x30E6E50u);
	CHECK(!Find(table, 15));        // inside the table's range, yet not in it: not the next ID's offset
	CHECK(!Find(table, 5));         // before the first
	CHECK(!Find(table, 5000000));   // past the last
	CHECK(!Find(table, 0));         // a runtime with no value
	CHECK(!Find({}, 10));
}
