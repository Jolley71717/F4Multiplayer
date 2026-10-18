#include "Test.h"

#include "net/ModDiff.h"

#include <string>
#include <vector>

using ModDiff::Compare;
using ModDiff::Difference;
using ModDiff::Moved;
using Names = std::vector<std::string>;

namespace
{
	// A load order as a hello carries it: the full plugins, the light mark, the light ones.
	Names Order(Names a_full, const Names& a_light = {})
	{
		return Protocol::ComparedLoadOrder(std::move(a_full), a_light);
	}

	const Names BASE{ "Fallout4.esm", "DLCRobot.esm", "DLCCoast.esm" };

	Names With(Names a_more)
	{
		auto out = BASE;
		out.insert(out.end(), a_more.begin(), a_more.end());
		return out;
	}

	struct English
	{
		English() { Text::Use("en", {}); }
	};
}

TEST("mod diff: the same load order differs in nothing, whatever the case of the names")
{
	const auto diff = Compare(Order(With({ "A.esp", "B.esp" }), { "C.esl" }), Order({ "fallout4.ESM", "dlcrobot.esm", "DLCCoast.esm", "a.esp", "B.ESP" }, { "c.esl" }));
	CHECK(diff.Empty());
	CHECK(diff.Count() == 0u);
	CHECK(diff.mine == 6u);
	CHECK(diff.theirs == 6u);
	CHECK(!diff.cut);
}

TEST("mod diff: an extra plugin on our side is extra only, not every plugin after it out of order")
{
	// The live test's case: an empty .esp only the joiner has, enabled in the middle of the list.
	const auto diff = Compare(Order(With({ "A.esp", "Empty.esp", "B.esp" })), Order(With({ "A.esp", "B.esp" })));
	CHECK(diff.extra == Names{ "Empty.esp" });
	CHECK(diff.missing.empty());
	CHECK(diff.moved.empty());
	CHECK(diff.Count() == 1u);
}

TEST("mod diff: plugins only the host has are missing, in the host's order")
{
	const auto diff = Compare(Order(BASE), Order(With({ "Z.esp", "A.esp" }), { "Light.esl" }));
	CHECK(diff.missing == (Names{ "Z.esp", "A.esp", "Light.esl" }));
	CHECK(diff.extra.empty());
	CHECK(diff.moved.empty());
}

TEST("mod diff: one plugin moved is the one named, not the ones it passed")
{
	// The host: A B C D E. Us: A C D E B (B moved to the end).
	const auto diff = Compare(Order(With({ "A.esp", "C.esp", "D.esp", "E.esp", "B.esp" })), Order(With({ "A.esp", "B.esp", "C.esp", "D.esp", "E.esp" })));
	REQUIRE(diff.moved.size() == 1u);
	CHECK(diff.moved[0] == (Moved{ "B.esp", false, false }));
	CHECK(diff.missing.empty());
	CHECK(diff.extra.empty());

	// Two swapped: one of them is out of place (either keeps the other's order).
	const auto swapped = Compare(Order(With({ "B.esp", "A.esp" })), Order(With({ "A.esp", "B.esp" })));
	CHECK(swapped.moved.size() == 1u);
}

TEST("mod diff: a plugin light in one game and full in the other is out of order, with both kinds said")
{
	const auto diff = Compare(Order(With({ "A.esp" }), { "B.esp" }), Order(With({ "A.esp", "B.esp" })));
	REQUIRE(diff.moved.size() == 1u);
	CHECK(diff.moved[0] == (Moved{ "B.esp", true, false }));
	CHECK(diff.extra.empty());
	CHECK(diff.missing.empty());

	// Light plugins are ordered among themselves: a full plugin between them in the list changes nothing.
	const auto lights = Compare(Order(With({ "A.esp" }), { "L1.esl", "L2.esl" }), Order(With({ "A.esp" }), { "L2.esl", "L1.esl" }));
	CHECK(lights.moved.size() == 1u);
	CHECK(lights.moved[0].lightHere && lights.moved[0].lightThere);
}

TEST("mod diff: missing, extra and moved together, each in its list")
{
	const auto diff = Compare(Order(With({ "Mine.esp", "B.esp", "A.esp" }), { "MyLight.esl" }), Order(With({ "A.esp", "B.esp", "Theirs.esp" }), { "TheirLight.esl" }));
	CHECK(diff.missing == (Names{ "Theirs.esp", "TheirLight.esl" }));
	CHECK(diff.extra == (Names{ "Mine.esp", "MyLight.esl" }));
	CHECK(diff.moved.size() == 1u);
	CHECK(diff.Count() == 5u);
}

TEST("mod diff: a name twice counts once, and an empty name or a lone light mark is no plugin")
{
	const auto plugins = ModDiff::Plugins({ "A.esp", "a.ESP", "", std::string{ Protocol::LIGHT_PLUGINS }, "B.esl", "A.esp" });
	REQUIRE(plugins.size() == 2u);
	CHECK(plugins[0].name == "A.esp");
	CHECK(!plugins[0].light);
	CHECK(plugins[1].name == "B.esl");
	CHECK(plugins[1].light);

	CHECK(Compare({}, {}).Empty());
	CHECK(Compare(Order(BASE), {}).extra == BASE);
}

TEST("mod diff: when the host's list was cut at the wire's limit, ours is compared only as far")
{
	Names hosts;
	Names mine;
	for (std::size_t i = 0; i < Protocol::MAX_PLUGINS + 40; ++i) {
		mine.push_back(std::format("P{}.esp", i));
	}
	hosts.assign(mine.begin(), mine.begin() + Protocol::MAX_PLUGINS);
	const auto diff = Compare(mine, hosts);
	CHECK(diff.cut);
	CHECK(diff.Empty());
	CHECK(diff.mine == Protocol::MAX_PLUGINS);

	// A shorter host list is whole: what we have past it is extra.
	const auto whole = Compare(Order(With({ "A.esp" })), Order(BASE));
	CHECK(!whole.cut);
	CHECK(whole.extra == Names{ "A.esp" });
}

TEST("mod diff: the comparison of two long lists stays quick and right")
{
	Names hosts;
	for (int i = 0; i < 500; ++i) {
		hosts.push_back(std::format("Mod{}.esp", i));
	}
	auto mine = hosts;
	std::rotate(mine.begin() + 10, mine.begin() + 11, mine.begin() + 400);  // Mod10 moved down to 399th place
	const auto diff = Compare(mine, hosts);
	REQUIRE(diff.moved.size() == 1u);
	CHECK(diff.moved[0].name == "Mod10.esp");
}

TEST("mod diff: the log names every plugin, in English")
{
	const English english;
	const auto    diff = Compare(Order(With({ "Mine.esp" }), { "A.esp" }), Order(With({ "A.esp", "Theirs.esp" })));
	const auto    lines = ModDiff::LogLines(diff);
	REQUIRE(lines.size() == 4u);
	CHECK(lines[0] == "the host's load order has 5 plugins, ours 5: 3 differ");
	CHECK(lines[1] == "missing on our side (1): Theirs.esp");
	CHECK(lines[2] == "extra on our side (1): Mine.esp");
	CHECK(lines[3] == "in a different order (1): A.esp (light here, full for the host)");

	CHECK(ModDiff::LogLines(Compare(Order(BASE), Order(BASE))) == Names{ "the host's load order has 3 plugins, ours 3: the same mods in the same order" });
}

TEST("mod diff: the Mods page lists each kind under its heading, a page of names at a time")
{
	const English english;
	const auto    diff = Compare(Order(With({ "Mine.esp" }), { "A.esp" }), Order(With({ "A.esp", "Theirs.esp" })));
	const auto    pages = ModDiff::Pages(diff);
	REQUIRE(pages.size() == 1u);
	CHECK(pages[0] == "Your mods compared with the host's: 3 to change. The game loads mods only when it starts: change yours to match, then restart it."
	                  "\n\nMissing in your game (install and enable these):\nTheirs.esp"
	                  "\n\nOnly in your game (disable these):\nMine.esp"
	                  "\n\nIn another place in your load order (sort these like the host's):\nA.esp (light in your game, full in the host's)");

	// 25 missing at 10 a page: three pages, the heading again at the top of each.
	Names theirs = BASE;
	for (int i = 0; i < 25; ++i) {
		theirs.push_back(std::format("M{}.esp", i));
	}
	const auto many = ModDiff::Pages(Compare(Order(BASE), Order(theirs)));
	REQUIRE(many.size() == 3u);
	for (const auto& page : many) {
		CHECK(page.starts_with("Your mods compared with the host's: 25 to change."));
		CHECK(page.find("\n\nMissing in your game (install and enable these):\n") != std::string::npos);
	}
	CHECK(many[0].find("M9.esp") != std::string::npos);
	CHECK(many[0].find("M10.esp") == std::string::npos);
	CHECK(many[2].find("M24.esp") != std::string::npos);

	// Nothing differs: one page that says so.
	CHECK(ModDiff::Pages(Difference{}) == Names{ "Your mods match the host's." });
}

TEST("mod diff: the page is in the player's language, the names as they are")
{
	Text::Use("de", Text::Parse("mods.missing = Fehlen in deinem Spiel:\nmods.page_intro = {n} zu \xC3\xA4ndern."));
	const auto pages = ModDiff::Pages(Compare(Order(BASE), Order(With({ "Theirs.esp" }))));
	Text::Use("en", {});
	REQUIRE(pages.size() == 1u);
	CHECK(pages[0] == "1 zu \xC3\xA4ndern.\n\nFehlen in deinem Spiel:\nTheirs.esp");
}

TEST("mod diff: the HUD hears of a difference once, again when it changes or when the player asked")
{
	ModDiff::Tracker tracker;
	const auto       one = Compare(Order(With({ "Empty.esp" })), Order(BASE));
	const auto       two = Compare(Order(With({ "Empty.esp", "Other.esp" })), Order(BASE));
	CHECK(!tracker.Known());

	// Refused: the list comes before every refusal, a minute apart.
	tracker.OnConnected();
	CHECK(tracker.OnList(one));
	CHECK(tracker.Differing() == 1u);
	tracker.OnConnected();
	CHECK(!tracker.OnList(one));  // the same: the HUD already said it
	tracker.OnConnected();
	CHECK(tracker.OnList(two));   // changed (the host's list did, or ours after a restart)
	CHECK(tracker.Differing() == 2u);

	// Asked: told whatever it says, a match too.
	tracker.OnAsked();
	CHECK(tracker.Asking());
	CHECK(tracker.OnList(two));
	CHECK(!tracker.Asking());
	tracker.OnAsked();
	CHECK(tracker.OnList(Difference{}));
	CHECK(tracker.Known());
	CHECK(tracker.Differing() == 0u);
	// A match nobody asked about is no news.
	CHECK(!tracker.OnList(Difference{}));
}

TEST("mod diff: let in without a list on that connection, an earlier connection's difference is forgotten")
{
	ModDiff::Tracker tracker;
	const auto       one = Compare(Order(With({ "Empty.esp" })), Order(BASE));
	tracker.OnConnected();
	CHECK(tracker.OnList(one));

	// The player fixed their mods and restarted: the next hello matches, no list comes, the welcome does.
	tracker.OnConnected();
	tracker.OnWelcome();
	CHECK(!tracker.Known());
	// Refused again later with the same difference: news again.
	tracker.OnConnected();
	CHECK(tracker.OnList(one));

	// Let in with a list (a difference only in plugins the host lets differ): it stays known.
	tracker.OnConnected();
	CHECK(!tracker.OnList(one));
	tracker.OnWelcome();
	CHECK(tracker.Known());

	tracker.Reset();
	CHECK(!tracker.Known());
	CHECK(!tracker.Asking());
}

TEST("mod diff: the menu key's name")
{
	CHECK(ModDiff::KeyName(0x70) == "F1");
	CHECK(ModDiff::KeyName(0x7B) == "F12");
	CHECK(ModDiff::KeyName(0x4D) == "key 4D");
}
