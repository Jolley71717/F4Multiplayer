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
