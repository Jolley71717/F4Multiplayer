#include "Test.h"

#include "net/ModDiff.h"

#include <chrono>
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

TEST("mod diff: a plugin the host lets differ that only one game has, after the rest of its kind, is no difference")
{
	const Names ignore{ "hdtextures.esm", "LightTextures.esl" };
	// Only the host has it, last of the full ones; only we have it; only we have the light one, last of those.
	CHECK(Compare(Order(With({ "A.esp" })), Order(With({ "A.esp", "HDTextures.esm" })), ignore).Empty());
	CHECK(Compare(Order(With({ "A.esp", "HDTEXTURES.ESM" })), Order(With({ "A.esp" })), ignore).Empty());
	CHECK(Compare(Order(With({ "A.esp" }), { "W.esl", "LightTextures.esl" }), Order(With({ "A.esp" }), { "W.esl" }), ignore).Empty());
	// A full one at the end moves no light plugin: they are numbered apart.
	CHECK(Compare(Order(With({ "A.esp" }), { "W.esl" }), Order(With({ "A.esp", "HDTextures.esm" }), { "W.esl" }), ignore).Empty());
	// Not let differ, the same plugin is missing.
	CHECK(Compare(Order(With({ "A.esp" })), Order(With({ "A.esp", "HDTextures.esm" }))).missing == Names{ "HDTextures.esm" });
}

TEST("mod diff: a plugin the host lets differ in the same place in both games, or swapped with another let differ, is no difference")
{
	const Names ignore{ "X.esp", "Y.esp" };
	CHECK(Compare(Order(With({ "A.esp", "X.esp", "B.esp" })), Order(With({ "A.esp", "X.esp", "B.esp" })), ignore).Empty());
	// X and Y swapped: B is in the same place in both.
	CHECK(Compare(Order(With({ "A.esp", "Y.esp", "X.esp", "B.esp" })), Order(With({ "A.esp", "X.esp", "Y.esp", "B.esp" })), ignore).Empty());
	// One each, one for the other, before B: B is still the same form IDs in both.
	CHECK(Compare(Order(With({ "A.esp", "Y.esp", "B.esp" })), Order(With({ "A.esp", "X.esp", "B.esp" })), ignore).Empty());
}

TEST("mod diff: a plugin the host lets differ is named where it moves another plugin, as missing, extra or out of order")
{
	const Names ignore{ "Textures.esp", "LightTextures.esl" };
	// The host's loads before the story mod, and we haven't it: the story mod's forms are 09 there and 08 here.
	const auto missing = Compare(Order(With({ "Story.esp" })), Order(With({ "Textures.esp", "Story.esp" })), ignore);
	CHECK(missing.missing == Names{ "Textures.esp" });
	CHECK(missing.Count() == 1u);
	const auto extra = Compare(Order(With({ "Textures.esp", "Story.esp" })), Order(With({ "Story.esp" })), ignore);
	CHECK(extra.extra == Names{ "Textures.esp" });
	CHECK(extra.Count() == 1u);
	// Both have it, ours after the story mod.
	const auto moved = Compare(Order(With({ "Story.esp", "Textures.esp" })), Order(With({ "Textures.esp", "Story.esp" })), ignore);
	REQUIRE(moved.moved.size() == 1u);
	CHECK(moved.moved[0] == (Moved{ "Textures.esp", false, false }));
	CHECK(moved.Count() == 1u);
	// Light plugins the same way, among themselves.
	const auto light = Compare(Order(With({ "Story.esp" }), { "Weapons.esl" }), Order(With({ "Story.esp" }), { "LightTextures.esl", "Weapons.esl" }), ignore);
	CHECK(light.missing == Names{ "LightTextures.esl" });
	CHECK(light.Count() == 1u);
}

TEST("mod diff: the plugins the host lets differ are left out of the order: one between others puts none of them out of place")
{
	const Names ignore{ "X.esp" };
	// The host: A X B C D. Us: A D B C (no X). D is the one out of place; X before B in the host's game moves B's
	// form IDs, so it is named too, as missing.
	const auto diff = Compare(Order(With({ "A.esp", "D.esp", "B.esp", "C.esp" })), Order(With({ "A.esp", "X.esp", "B.esp", "C.esp", "D.esp" })), ignore);
	REQUIRE(diff.moved.size() == 1u);
	CHECK(diff.moved[0].name == "D.esp");
	CHECK(diff.missing == Names{ "X.esp" });
	CHECK(diff.extra.empty());

	// X in its place in both, our extra plugin after it: the extra one only.
	const auto extra = Compare(Order(With({ "A.esp", "X.esp", "Mine.esp", "B.esp" })), Order(With({ "A.esp", "X.esp", "B.esp" })), ignore);
	CHECK(extra.extra == Names{ "Mine.esp" });
	CHECK(extra.Count() == 1u);
}

TEST("mod diff: the comparison agrees with the server's: nothing to change exactly when a joiner is let in")
{
	const Names ignore{ "Textures.esp", "LightTextures.esl" };
	const std::vector<Names> games{
		Order(With({ "Story.esp" }), { "Weapons.esl" }),
		Order(With({ "Story.esp", "Textures.esp" }), { "Weapons.esl" }),
		Order(With({ "Textures.esp", "Story.esp" }), { "Weapons.esl" }),
		Order(With({ "Story.esp" }), { "LightTextures.esl", "Weapons.esl" }),
		Order(With({ "Story.esp" }), { "Weapons.esl", "LightTextures.esl" }),
		Order(With({ "Story.esp", "Textures.esp" }), { "Weapons.esl", "LightTextures.esl" }),
		Order(With({ "Story.esp", "Other.esp" }), { "Weapons.esl" }),
		Order(With({ "Other.esp", "Textures.esp", "Story.esp" }), { "Weapons.esl" }),
		Order(With({ "Story.esp" }), { "Weapons.esl", "Textures.esp" }),
		Order(With({ "Textures.esp", "Story.esp" }), { "LightTextures.esl", "Weapons.esl" }),
	};
	for (const auto& mine : games) {
		for (const auto& hosts : games) {
			CHECK(Compare(mine, hosts, ignore).Empty() == Protocol::SameLoadOrderIgnoring(mine, hosts, ignore));
		}
	}
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
	CHECK(pages[0] == "Your mods compared with the host's: 2 only one of you has. You can still play together: what comes from those (an NPC, "
	                  "an item, a quest) is left out in the other game. To share it, match the host's mods, then restart the game: it loads "
	                  "mods only when it starts."
	                  "\n\nMissing in your game (install and enable these):\nTheirs.esp"
	                  "\n\nOnly in your game (disable these):\nMine.esp"
	                  "\n\nIn another place in your load order (no need to change these):\nA.esp (light in your game, full in the host's)");

	// 25 missing at 10 a page: three pages, the heading again at the top of each.
	Names theirs = BASE;
	for (int i = 0; i < 25; ++i) {
		theirs.push_back(std::format("M{}.esp", i));
	}
	const auto many = ModDiff::Pages(Compare(Order(BASE), Order(theirs)));
	REQUIRE(many.size() == 3u);
	for (const auto& page : many) {
		CHECK(page.starts_with("Your mods compared with the host's: 25 only one of you has."));
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
	using namespace std::chrono_literals;
	ModDiff::Tracker tracker;
	const auto       one = Compare(Order(With({ "Empty.esp" })), Order(BASE));
	const auto       two = Compare(Order(With({ "Empty.esp", "Other.esp" })), Order(BASE));
	const auto       t0 = ModDiff::Tracker::Clock::time_point{} + 1h;
	CHECK(!tracker.Known());

	// Refused: the list comes before every refusal, a minute apart, and every refusal is told as the difference.
	tracker.OnConnected();
	CHECK(!tracker.OnList(one, t0));  // not before the refusal or the welcome
	CHECK(tracker.Differing() == 1u);
	CHECK(tracker.OnRefused());
	tracker.OnDisconnected();
	tracker.OnConnected();
	CHECK(!tracker.OnList(one, t0 + 1min));
	CHECK(tracker.OnRefused());
	tracker.OnDisconnected();

	// Let in with a list: told once the welcome is in, when it is news.
	tracker.OnConnected();
	CHECK(!tracker.OnList(two, t0 + 2min));
	CHECK(tracker.OnWelcome());  // changed (the host's list did, or ours after a restart)
	CHECK(tracker.Differing() == 2u);
	tracker.OnDisconnected();
	tracker.OnConnected();
	CHECK(!tracker.OnList(two, t0 + 3min));
	CHECK(!tracker.OnWelcome());  // the same: the HUD already said it

	// Asked: told whatever it says, a match too.
	auto now = t0 + 4min;
	CHECK(tracker.OnAsk(now));
	CHECK(tracker.Asking(now));
	CHECK(tracker.OnList(two, now + 100ms));
	CHECK(!tracker.Asking(now + 100ms));
	now += 1min;
	CHECK(tracker.OnAsk(now));
	CHECK(tracker.OnList(Difference{}, now + 100ms));
	CHECK(tracker.Known());
	CHECK(tracker.Differing() == 0u);
	// A match nobody asked about is no news.
	CHECK(!tracker.OnList(Difference{}, now + 1min));
}

TEST("mod diff: a refusal for another reason, or with nothing that differs, is told as it came")
{
	ModDiff::Tracker tracker;
	tracker.OnConnected();
	CHECK(!tracker.OnRefused());  // no list on this connection
	tracker.OnConnected();
	CHECK(!tracker.OnList(Difference{}, ModDiff::Tracker::Clock::time_point{}));
	CHECK(!tracker.OnRefused());  // a list, but the same mods (one cut at the wire's limit): the refusal's own text
}

TEST("mod diff: a joiner asks the host for its list at most once in a few seconds of the last list or ask, as the server answers")
{
	using namespace std::chrono_literals;
	ModDiff::Tracker tracker;
	const auto       one = Compare(Order(With({ "Empty.esp" })), Order(BASE));
	const auto       t0 = ModDiff::Tracker::Clock::time_point{} + 1h;

	// Let in with the list: the server just sent it, and would not answer an ask for another 5 s. "Ask the host
	// again" at once waits (the player is told so); after 5 s it asks.
	tracker.OnConnected();
	CHECK(!tracker.OnList(one, t0));
	CHECK(tracker.OnWelcome());
	CHECK(!tracker.OnAsk(t0 + 1s));
	CHECK(!tracker.Asking(t0 + 1s));
	CHECK(!tracker.OnAsk(t0 + 4900ms));
	CHECK(tracker.OnAsk(t0 + 5s));
	CHECK(tracker.Asking(t0 + 5s));

	// One ask out at a time.
	CHECK(!tracker.OnAsk(t0 + 6s));
	// The answer never came (lost, or the server dropped it): after 10 s the ask is over, and may go again.
	CHECK(tracker.Asking(t0 + 14900ms));
	CHECK(!tracker.Asking(t0 + 15s));
	CHECK(tracker.OnAsk(t0 + 15s));
	CHECK(tracker.OnList(one, t0 + 15500ms));  // its answer, told though nothing changed
	CHECK(!tracker.OnAsk(t0 + 16s));            // that list is recent
	CHECK(tracker.OnAsk(t0 + 21s));
	CHECK(!tracker.OnList(one, t0 + 32s));  // an answer too late is no answer: the same as before, no news

	// Disconnected while asking: the ask is over, and the next connection's limit is its own (the server's is per
	// connection, counted from the list it sent).
	CHECK(tracker.OnAsk(t0 + 40s));
	tracker.OnDisconnected();
	CHECK(!tracker.Asking(t0 + 40s));
	tracker.OnConnected();
	CHECK(!tracker.OnWelcome());  // our hello matched: no list
	CHECK(tracker.OnAsk(t0 + 41s));
	// A new connection while an ask is out forgets it too.
	tracker.OnConnected();
	CHECK(!tracker.Asking(t0 + 41s));
	CHECK(tracker.OnAsk(t0 + 41s));
}

TEST("mod diff: let in without a list on that connection, an earlier connection's difference is forgotten")
{
	ModDiff::Tracker tracker;
	const auto       one = Compare(Order(With({ "Empty.esp" })), Order(BASE));
	const auto       t = ModDiff::Tracker::Clock::time_point{};
	tracker.OnConnected();
	CHECK(!tracker.OnList(one, t));
	CHECK(tracker.OnRefused());

	// The player fixed their mods and restarted: the next hello matches, no list comes, the welcome does.
	tracker.OnConnected();
	CHECK(!tracker.OnWelcome());
	CHECK(!tracker.Known());

	// Let in later with a list that differs (one cut at the wire's limit, say): news again, and it stays known.
	tracker.OnConnected();
	CHECK(!tracker.OnList(one, t));
	CHECK(tracker.OnWelcome());
	CHECK(tracker.Known());

	tracker.Reset();
	CHECK(!tracker.Known());
	CHECK(!tracker.Asking(t));
}

TEST("mod diff: the menu key's name")
{
	CHECK(ModDiff::KeyName(0x70) == "F1");
	CHECK(ModDiff::KeyName(0x7B) == "F12");
	CHECK(ModDiff::KeyName(0x4D) == "key 4D");
}

TEST("mod diff: plugins only in another place are no news to the HUD since form IDs are translated, but a refusal names them")
{
	const English english;
	ModDiff::Tracker tracker;
	const auto       moved = Compare(Order(Names{ "Fallout4.esm", "DLCCoast.esm", "DLCRobot.esm" }), Order(BASE));
	REQUIRE(moved.moved.size() == 1u);
	CHECK(moved.Matters() == 0u);
	CHECK(moved.Count() == 1u);

	tracker.OnConnected();
	CHECK(!tracker.OnList(moved, ModDiff::Tracker::Clock::time_point{}));
	CHECK(!tracker.OnWelcome());  // in: nothing to tell
	CHECK(tracker.Differing() == 0u);
	CHECK(tracker.Differing(true) == 1u);

	// A session that still needs the same load order (one of the games sent no list) refuses for it: told as the move.
	tracker.OnConnected();
	CHECK(!tracker.OnList(moved, ModDiff::Tracker::Clock::time_point{}));
	CHECK(tracker.OnRefused());

	// The Mods page says the mods match, and lists the moved one as nothing to change.
	const auto pages = ModDiff::Pages(moved);
	REQUIRE(pages.size() == 1u);
	CHECK(pages[0] == "Your mods match the host's.\n\nIn another place in your load order (no need to change these):\nDLCRobot.esm");
}
