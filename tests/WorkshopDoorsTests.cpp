// A settlement-built door's open state across games (game/WorkshopDoors.h): the session's word on each door, what a game
// reports of its own doors and its copies, and what it sets on them.

#include "Test.h"

#include "game/WorkshopDoors.h"

using WorkshopDoors::Book;
using WorkshopDoors::Do;
using WorkshopDoors::Key;

namespace
{
	constexpr Key HOSTS{ 1, 0xFF000042 };   // the host's door
	constexpr Key FRIENDS{ 2, 0xFF001234 };  // a friend's door
}

TEST("WorkshopDoors: a door of ours is reported as it stands when first seen, then on every change")
{
	Book book;
	CHECK(book.Look(HOSTS, false, true) == Do::kSend);   // placed shut: said
	CHECK(book.Look(HOSTS, false, true) == Do::kNothing);
	CHECK(book.Look(HOSTS, true, true) == Do::kSend);    // our player opened it
	CHECK(book.Open(HOSTS));
	CHECK(book.Look(HOSTS, true, true) == Do::kNothing);
	CHECK(book.Look(HOSTS, false, true) == Do::kSend);   // and shut it
}

TEST("WorkshopDoors: a copy is taken as it stands when first seen, and reported when our player moves it")
{
	Book book;
	CHECK(book.Look(FRIENDS, false, false) == Do::kNothing);
	CHECK(book.Look(FRIENDS, true, false) == Do::kSend);  // opened here: the builder's door follows
	CHECK(book.Look(FRIENDS, true, false) == Do::kNothing);
}

TEST("WorkshopDoors: a word heard is set on the door at the next look, and not sent back")
{
	Book book;
	CHECK(book.Look(FRIENDS, false, false) == Do::kNothing);
	book.Heard(FRIENDS, true);
	CHECK(book.Pending(FRIENDS));
	CHECK(book.Look(FRIENDS, false, false) == Do::kApply);  // our copy is shut: open it
	CHECK(book.Open(FRIENDS));
	CHECK(book.Look(FRIENDS, false, false) == Do::kApply);  // still shut (the set failed, not loaded): asked again
	book.Applied(FRIENDS);
	CHECK(!book.Pending(FRIENDS));
	CHECK(book.Look(FRIENDS, true, false) == Do::kNothing);  // open now, as the word says: nothing sent back
	// a word that already holds: nothing to set
	book.Heard(FRIENDS, true);
	CHECK(book.Look(FRIENDS, true, false) == Do::kNothing);
	CHECK(!book.Pending(FRIENDS));
}

TEST("WorkshopDoors: a friend's word on our own door is set on it too")
{
	Book book;
	CHECK(book.Look(HOSTS, false, true) == Do::kSend);
	book.Heard(HOSTS, true);  // a friend opened their copy
	CHECK(book.Look(HOSTS, false, true) == Do::kApply);
	book.Applied(HOSTS);
	CHECK(book.Look(HOSTS, true, true) == Do::kNothing);
	CHECK(book.Look(HOSTS, false, true) == Do::kSend);  // our player shuts it again: said
}

TEST("WorkshopDoors: a word heard before the door is seen is set on it when it is, and Clear sees everything afresh")
{
	Book book;
	book.Heard(FRIENDS, true);
	CHECK(book.Look(FRIENDS, false, false) == Do::kApply);
	book.Applied(FRIENDS);
	CHECK(book.Size() == 1);
	book.Clear();
	CHECK(book.Size() == 0);
	CHECK(book.Look(HOSTS, true, true) == Do::kSend);
	book.Forget(HOSTS);
	CHECK(book.Look(HOSTS, true, true) == Do::kSend);
}

TEST("WorkshopDoors: words on more doors than a session builds are dropped, and those kept still follow")
{
	Book book;
	for (std::uint32_t i = 0; i < Book::MAX_DOORS + 50; ++i) {
		book.Heard({ 7, 0xFF000000 + i }, true);
	}
	CHECK(book.Size() == Book::MAX_DOORS);
	book.Heard({ 7, 0xFF000000 }, false);  // one already kept: its new word is taken
	CHECK(!book.Open({ 7, 0xFF000000 }));
	CHECK(book.Pending({ 7, 0xFF000000 }));
	CHECK(!book.Pending({ 7, 0xFF000000 + Book::MAX_DOORS + 1 }));
}

TEST("WorkshopDoors: a word set on a door that hasn't taken it yet is set again, not sent back, for a few looks")
{
	Book book;
	CHECK(book.Look(HOSTS, false, true) == Do::kSend);
	book.Heard(HOSTS, true);  // a friend opened their copy
	CHECK(book.Look(HOSTS, false, true) == Do::kApply);
	book.Applied(HOSTS);
	for (std::uint8_t i = 0; i < WorkshopDoors::SETTLE_LOOKS; ++i) {
		CHECK(book.Look(HOSTS, false, true) == Do::kApply);  // still shut: set again, the friend's open stands
		book.Applied(HOSTS);                                  // no new window: it was not pending
		CHECK(book.Open(HOSTS));
	}
	CHECK(book.Look(HOSTS, false, true) == Do::kSend);  // never took it: shut is the word now, said to the others
	CHECK(!book.Open(HOSTS));
}

TEST("WorkshopDoors: once the door has the word, our player's next change is said at once")
{
	Book book;
	CHECK(book.Look(FRIENDS, false, false) == Do::kNothing);
	book.Heard(FRIENDS, true);
	CHECK(book.Look(FRIENDS, false, false) == Do::kApply);
	book.Applied(FRIENDS);
	CHECK(book.Look(FRIENDS, true, false) == Do::kNothing);  // took it: the window ends
	CHECK(book.Look(FRIENDS, false, false) == Do::kSend);    // our player shuts it
}

TEST("WorkshopDoors: our own doors' words, loaded or not, go to a player who joins; friends' doors don't")
{
	Book book;
	CHECK(book.Look({ 1, 0xFF000010 }, true, true) == Do::kSend);
	CHECK(book.Look({ 1, 0xFF000011 }, false, true) == Do::kSend);
	book.Heard({ 1, 0xFF000012 }, true);  // a friend opened their copy of a door of ours not loaded here
	book.Heard(FRIENDS, true);
	auto own = book.OwnWords(1);
	CHECK(own.size() == 3);
	CHECK((own[0] == std::pair<std::uint32_t, bool>{ 0xFF000010, true }));
	CHECK((own[1] == std::pair<std::uint32_t, bool>{ 0xFF000011, false }));
	CHECK((own[2] == std::pair<std::uint32_t, bool>{ 0xFF000012, true }));
	CHECK(book.OwnWords(0).empty());  // no player id yet: nothing is ours
}

TEST("WorkshopDoors: our built doors are found once each, only runtime ones, and dropped when gone here")
{
	WorkshopDoors::Found found;
	CHECK(found.Note(0xFF000100));
	CHECK(!found.Note(0xFF000100));  // the cell walk and the placed event both find it: one door
	CHECK(!found.Note(0x0001F2A3));  // a plugin's door is DoorShare's
	CHECK(found.Note(0xFF000101));
	CHECK(found.Size() == 2);
	std::uint32_t looked = 0;
	found.Each([&](std::uint32_t a_id) {
		++looked;
		return a_id != 0xFF000100;  // its cell unloaded
	});
	CHECK(looked == 2);
	CHECK(!found.Has(0xFF000100));
	CHECK(found.Has(0xFF000101));
	CHECK(found.Note(0xFF000100));  // its cell attached again
	found.Clear();
	CHECK(found.Size() == 0);
}

TEST("WorkshopDoors: no more built doors are watched than a session builds")
{
	WorkshopDoors::Found found;
	for (std::uint32_t i = 0; i < WorkshopDoors::Found::MAX + 10; ++i) {
		found.Note(0xFF000000 + i);
	}
	CHECK(found.Size() == WorkshopDoors::Found::MAX);
	CHECK(found.Has(0xFF000000));
}
