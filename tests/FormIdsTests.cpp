#include "Test.h"

#include "FormIds.h"

#include <string>
#include <vector>

using FormIds::Compose;
using FormIds::Extend;
using FormIds::Mapper;
using FormIds::Place;
using FormIds::PlaceOf;
using FormIds::Table;
using Names = std::vector<std::string>;

namespace
{
	// A load order as a hello carries it: the full plugins, the light mark, the light ones.
	Names Order(const Names& a_full, const Names& a_light = {})
	{
		return Protocol::ComparedLoadOrder(a_full, a_light);
	}

	const Names BASE{ "Fallout4.esm", "DLCRobot.esm", "DLCCoast.esm" };

	constexpr Place Full(std::uint16_t a_index) { return { false, a_index }; }
	constexpr Place Light(std::uint16_t a_index) { return { true, a_index }; }
}

TEST("form ids: a form ID's place is its plugin's among its kind; 0 and runtime forms have none")
{
	CHECK(PlaceOf(0x00012345) == Full(0));
	CHECK(PlaceOf(0x09000804) == Full(9));
	CHECK(PlaceOf(0xFE012804) == Light(0x012));
	CHECK(PlaceOf(0xFEFFF001) == Light(0xFFF));
	CHECK(!PlaceOf(0));
	CHECK(!PlaceOf(0xFF000123));
	CHECK(FormIds::LocalPart(0x09000804) == 0x000804);
	CHECK(FormIds::LocalPart(0xFE012804) == 0x804);
}

TEST("form ids: composing an ID gives back what was taken apart, and none where no ID can say it")
{
	for (const std::uint32_t id : { 0x00000014u, 0x0A0B0C0Du, 0xFD123456u, 0xFE000801u, 0xFE123FFFu }) {
		CHECK(Compose(*PlaceOf(id), FormIds::LocalPart(id)) == id);
	}
	CHECK(!Compose(Place{ true, 3 }, 0x1000));    // a full plugin's form past the 12 bits a light one numbers
	CHECK(!Compose(Place{ true, 0x1000 }, 1));    // past the light places
	CHECK(!Compose(Place{ false, 0xFE }, 1));     // FE and FF are not full places
	CHECK(Compose(Place{ false, 0xFD }, 0x00FFFFFF) == 0xFDFFFFFFu);
}

TEST("form ids: a table splits a hello's list into its kinds, names in any case, each once")
{
	const auto table = Table::Of(Order({ "Fallout4.esm", "A.esp", "a.ESP" }, { "L1.esl", "L2.esl" }));
	CHECK(table.full == Names({ "Fallout4.esm", "A.esp" }));
	CHECK(table.light == Names({ "L1.esl", "L2.esl" }));
	CHECK(table.Find("a.esp") == Full(1));
	CHECK(table.Find("l2.ESL") == Light(1));
	CHECK(!table.Find("B.esp"));
	REQUIRE(table.At(Place{ true, 0 }));
	CHECK(*table.At(Place{ true, 0 }) == "L1.esl");
	CHECK(!table.At(Place{ false, 2 }));
	CHECK(Table::Of(table.Order()).Same(table));
	// A list without the mark (an older layout) is all full.
	CHECK(Table::Of({ "Fallout4.esm", "X.esl" }).full.size() == 2);
}

TEST("form ids: the same plugins in the same places translate nothing")
{
	const Mapper same(Order(BASE, { "L.esl" }), Order({ "fallout4.ESM", "DLCRobot.esm", "DLCCoast.esm" }, { "l.esl" }));
	CHECK(same.Identity());
	CHECK(Mapper().Identity());
	// Without either list (a peer that sent none, the session's not come yet) as before protocol 36.
	CHECK(Mapper({}, Order(BASE)).Identity());
	CHECK(Mapper(Order(BASE), {}).Identity());
	CHECK(Mapper(Order(BASE), {}).ToSession(0x05000801) == 0x05000801u);
}

TEST("form ids: a reordered load order maps each plugin's forms to its place in the other")
{
	// Ours: A before B; the session's: B before A.
	const Mapper m(Order(Names{ "Fallout4.esm", "A.esp", "B.esp" }), Order(Names{ "Fallout4.esm", "B.esp", "A.esp" }));
	REQUIRE(!m.Identity());
	CHECK(m.ToSession(0x01000801) == 0x02000801u);
	CHECK(m.ToSession(0x02000802) == 0x01000802u);
	CHECK(m.ToLocal(0x02000801) == 0x01000801u);
	CHECK(m.ToSession(0x0001F00D) == 0x0001F00Du);  // Fallout4.esm in both
	CHECK(m.ToLocal(*m.ToSession(0x01ABCDEF)) == 0x01ABCDEFu);
	CHECK(m.LocalPlugin(0x01000801) == "A.esp");
	CHECK(m.SessionPlugin(0x01000801) == "B.esp");
	CHECK(m.OursOnly() == 0);
	CHECK(m.SessionsOnly() == 0);
}

TEST("form ids: light plugins map among the light ones, and a plugin light in one game and full in the other")
{
	const Mapper m(Order(BASE, { "L1.esl", "L2.esl", "Both.esp" }), Order(Names{ "Fallout4.esm", "DLCRobot.esm", "DLCCoast.esm", "Both.esp" }, { "L2.esl", "L1.esl" }));
	CHECK(m.ToSession(0xFE000801) == 0xFE001801u);  // L1: light 0 here, light 1 there
	CHECK(m.ToSession(0xFE001ABC) == 0xFE000ABCu);
	CHECK(m.ToSession(0xFE002801) == 0x03000801u);  // Both.esp: light here, full there
	CHECK(m.ToLocal(0x03000801) == 0xFE002801u);
	// A form of it past what a light plugin numbers has no ID here.
	CHECK(!m.ToLocal(0x03001000));
}

TEST("form ids: a plugin the other side lacks has no IDs there; runtime forms and 0 pass as they are")
{
	const Mapper m(Order(Names{ "Fallout4.esm", "Mine.esp", "Shared.esp" }, { "MyLight.esl" }), Order(Names{ "Fallout4.esm", "Shared.esp", "Theirs.esp" }));
	CHECK(!m.ToSession(0x01000801));  // Mine.esp
	CHECK(!m.ToSession(0xFE000801));  // MyLight.esl
	CHECK(m.ToSession(0x02000801) == 0x01000801u);
	CHECK(!m.ToLocal(0x02000801));  // Theirs.esp
	CHECK(!m.ToLocal(0x07000801));  // a place the session's table has no plugin at
	CHECK(m.ToSession(0) == 0u);
	CHECK(m.ToSession(0xFF001234) == 0xFF001234u);
	CHECK(m.ToLocal(0xFF001234) == 0xFF001234u);
	CHECK(m.OursOnly() == 2);
	CHECK(m.SessionsOnly() == 1);
	CHECK(m.LocalPlugin(0xFF001234).empty());
}

TEST("form ids: a joiner's other plugins are added after the rest of their kind, so the table's IDs stay")
{
	auto table = Order(BASE, { "L1.esl" });
	const auto before = table;
	CHECK(Extend(table, Order(BASE, { "L1.esl" })) == 0);
	CHECK(table == before);

	CHECK(Extend(table, Order(Names{ "Fallout4.esm", "New.esp", "DLCCoast.esm" }, { "L2.esl", "l1.ESL" })) == 2);
	CHECK(table == Order(Names{ "Fallout4.esm", "DLCRobot.esm", "DLCCoast.esm", "New.esp" }, { "L1.esl", "L2.esl" }));
	// The old IDs are what they were.
	const Mapper m(before, table);
	CHECK(m.ToSession(0x02000801) == 0x02000801u);
	CHECK(m.ToSession(0xFE000801) == 0xFE000801u);

	// An empty table becomes the first list.
	Names empty;
	CHECK(Extend(empty, Order(BASE)) == 3);
	CHECK(empty == Order(BASE));
}

TEST("form ids: the table stops growing where places or the wire's list run out")
{
	Names full;
	for (int i = 0; i < 300; ++i) {
		full.push_back(std::format("Mod{}.esp", i));
	}
	Names table;
	Extend(table, Order(full));
	CHECK(Table::Of(table).full.size() == FormIds::MAX_FULL);

	Names light;
	for (int i = 0; i < 600; ++i) {
		light.push_back(std::format("Light{}.esl", i));
	}
	Extend(table, Order({}, light));
	CHECK(table.size() < Protocol::MAX_PLUGINS);
	CHECK(Table::Of(table).Size() + 1 == table.size());
}
