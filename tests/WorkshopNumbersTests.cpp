// The host's settlement figures shown to friends (game/WorkshopNumbers.h, common/BenchNumbersBook.h): what the host's game
// tells and when, what the server and a friend's game keep, and what the friend's workshop bar shows with and without
// figures. The server's part end to end is in ServerBenchNumbersTests.

#include "Test.h"

#include "BenchNumbersBook.h"
#include "Protocol.h"
#include "game/ResourceMirror.h"
#include "game/WorkshopNumbers.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

namespace
{
	using namespace std::chrono_literals;
	using WorkshopNumbers::Bar;
	using WorkshopNumbers::Figures;

	constexpr std::uint32_t SANCTUARY = 0x0002'50FE;
	constexpr std::uint32_t RED_ROCKET = 0x0005'4BAE;
	constexpr std::uint32_t ABERNATHY = 0x0006'F5C5;

	// A settlement with a generator (5 made, 3 used), a turret, two beds, three settlers and too little food.
	constexpr WorkshopNumbers::Read BUILT{
		.power = 5,
		.powerUsed = 3,
		.food = 2,
		.water = 6,
		.defense = 8,
		.beds = 2,
		.population = 3,
		.happiness = 55,
		.happinessChange = -5,
		.foodRating = -1,
		.bedsRating = -1,
	};

	Figures Built(std::uint32_t a_bench)
	{
		return WorkshopNumbers::Of(a_bench, BUILT);
	}

	Figures NotOurs(std::uint32_t a_bench)
	{
		return Figures{ .bench = a_bench };
	}

	std::vector<Figures> Many(std::size_t a_count, std::uint16_t a_food = 1)
	{
		std::vector<Figures> out;
		for (std::size_t i = 0; i < a_count; ++i) {
			auto figures = Built(0x0003'0000 + static_cast<std::uint32_t>(i));
			figures.food = a_food;
			out.push_back(figures);
		}
		return out;
	}
}

TEST("settlement figures: what the host's game tells is what its bar shows: whole numbers, none below 0, happiness within 0 to 100")
{
	const auto figures = Built(SANCTUARY);
	CHECK(figures.bench == SANCTUARY);
	CHECK(figures.flags == (Figures::kHosts | Figures::kFoodShort | Figures::kBedsShort));
	CHECK(figures.power == 5);
	CHECK(figures.powerUsed == 3);
	CHECK(figures.food == 2);
	CHECK(figures.water == 6);
	CHECK(figures.defense == 8);
	CHECK(figures.beds == 2);
	CHECK(figures.population == 3);
	CHECK(figures.happiness == 55);
	CHECK(figures.happinessTarget == 50);

	const auto odd = WorkshopNumbers::Of(SANCTUARY, { .power = -4,
														.powerUsed = 70'000,
														.food = -1,
														.happiness = 130,
														.happinessChange = 40,
														.waterRating = -3,
														.defenseRating = -1,
														.powerRating = -1 });
	CHECK(odd.power == 0);
	CHECK(odd.powerUsed == 65535);
	CHECK(odd.food == 0);
	CHECK(odd.happiness == 100);
	CHECK(odd.happinessTarget == 100);
	CHECK(odd.flags == (Figures::kHosts | Figures::kWaterShort | Figures::kDefenseShort | Figures::kPowerShort));
	const auto low = WorkshopNumbers::Of(SANCTUARY, { .happiness = 10, .happinessChange = -40 });
	CHECK(low.happinessTarget == 0);

	// Every one of them is a message the wire takes.
	for (const auto& entry : { figures, odd, low }) {
		const auto back = Protocol::DecodeBenchNumbers(Protocol::Encode(Protocol::BenchNumbers{ { entry } }));
		REQUIRE(back);
		CHECK(back->benches == std::vector{ entry });
	}
}

TEST("settlement figures: a friend's bar shows the host's figures, red where the host's is, the arrow by the host's target")
{
	const auto figures = Built(SANCTUARY);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kPower) == 5);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kFood) == 2);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kWater) == 6);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kBeds) == 2);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kDefense) == 8);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kPopulation) == 3);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kHappiness) == 55);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kFoodRating) < 0);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kWaterRating) == 0);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kDefenseRating) == 0);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kBedsRating) < 0);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kPowerRating) == 0);
	CHECK(WorkshopNumbers::Shown(figures, Bar::kHappinessChange) == -5);

	auto rising = figures;
	rising.happinessTarget = 80;
	rising.flags |= Figures::kPowerShort;
	CHECK(WorkshopNumbers::Shown(rising, Bar::kHappinessChange) == 25);
	CHECK(WorkshopNumbers::Shown(rising, Bar::kPowerRating) < 0);
}

TEST("settlement figures: the host's game reads its settlements once a second")
{
	WorkshopNumbers::Reporter         reporter;
	const WorkshopNumbers::Clock::time_point start{ 10s };
	CHECK(reporter.Due(start));
	CHECK(!reporter.Due(start));
	CHECK(!reporter.Due(start + 999ms));
	CHECK(reporter.Due(start + 1s));
	CHECK(!reporter.Due(start + 1500ms));
	CHECK(reporter.Due(start + 2s));
}

TEST("settlement figures: the host's game first says forget them all, then tells each settlement once and after that only what changed")
{
	WorkshopNumbers::Reporter reporter;
	const std::array          owned{ Built(SANCTUARY), Built(RED_ROCKET) };
	const auto                first = reporter.Update(owned);
	REQUIRE(first.size() == 2);
	CHECK(first[0].benches.empty());  // forget them all: the server's table may be another session's
	CHECK(first[1].benches.size() == 2);
	CHECK(reporter.Benches() == 2);
	CHECK(reporter.Update(owned).empty());  // nothing changed: nothing goes out

	// The host builds a generator at Sanctuary: that settlement alone is told.
	auto more = owned;
	more[0].power = 10;
	const auto changed = reporter.Update(more);
	REQUIRE(changed.size() == 1);
	CHECK(changed[0].benches == std::vector{ more[0] });
	CHECK(reporter.Told(SANCTUARY) == more[0]);
	CHECK(reporter.Update(more).empty());

	// A figure going red alone is a change too.
	more[1].flags |= Figures::kPowerShort;
	const auto red = reporter.Update(more);
	REQUIRE(red.size() == 1);
	CHECK(red[0].benches == std::vector{ more[1] });
	CHECK(reporter.Reports() == 4);
}

TEST("settlement figures: with no settlement of its own the host's game says only forget them all, once")
{
	WorkshopNumbers::Reporter reporter;
	const auto                first = reporter.Update({});
	REQUIRE(first.size() == 1);
	CHECK(first[0].benches.empty());
	CHECK(reporter.Update({}).empty());
}

TEST("settlement figures: a settlement the host no longer owns is told once as not its own; owned again, its figures go again")
{
	WorkshopNumbers::Reporter reporter;
	const std::array          both{ Built(SANCTUARY), Built(RED_ROCKET) };
	REQUIRE(reporter.Update(both).size() == 2);
	const std::array one{ Built(SANCTUARY) };
	const auto       lost = reporter.Update(one);
	REQUIRE(lost.size() == 1);
	CHECK(lost[0].benches == std::vector{ NotOurs(RED_ROCKET) });
	CHECK(reporter.Update(one).empty());
	CHECK(!reporter.Told(RED_ROCKET));
	const auto again = reporter.Update(both);
	REQUIRE(again.size() == 1);
	CHECK(again[0].benches == std::vector{ both[1] });

	// A workbench named twice, or none, is told once, or not.
	WorkshopNumbers::Reporter twice;
	const std::array          odd{ Built(SANCTUARY), Built(SANCTUARY), Built(0) };
	const auto                told = twice.Update(odd);
	REQUIRE(told.size() == 2);
	CHECK(told[1].benches.size() == 1);
}

TEST("settlement figures: after a welcome or a load the host's game says forget them all and tells every settlement again")
{
	WorkshopNumbers::Reporter reporter;
	const std::array          owned{ Built(SANCTUARY), Built(RED_ROCKET) };
	REQUIRE(reporter.Update(owned).size() == 2);
	reporter.Forget();
	const auto again = reporter.Update(owned);
	REQUIRE(again.size() == 2);
	CHECK(again[0].benches.empty());
	CHECK(again[1].benches.size() == 2);

	// A settlement that was ours before the load and isn't after needs no word of its own: forget them all covers it.
	reporter.Forget();
	const std::array one{ Built(SANCTUARY) };
	const auto       after = reporter.Update(one);
	REQUIRE(after.size() == 2);
	CHECK(after[0].benches.empty());
	CHECK(after[1].benches == std::vector{ one[0] });
}

TEST("settlement figures: no message holds more settlements than the wire carries; the rest go with the next, and none starves")
{
	WorkshopNumbers::Reporter reporter;
	const auto                many = Many(Protocol::MAX_BENCH_NUMBERS + 10);
	const auto                first = reporter.Update(many);
	REQUIRE(first.size() == 2);
	CHECK(first[1].benches.size() == Protocol::MAX_BENCH_NUMBERS);
	REQUIRE(Protocol::DecodeBenchNumbers(Protocol::Encode(first[1])));
	const auto second = reporter.Update(many);
	REQUIRE(second.size() == 1);
	CHECK(second[0].benches.size() == 10);
	CHECK(reporter.Benches() == many.size());
	CHECK(reporter.Update(many).empty());

	// Every settlement changes every second: each still gets its turn within two.
	std::map<std::uint32_t, int> told;
	for (std::uint16_t round = 2; round < 6; ++round) {
		const auto word = reporter.Update(Many(Protocol::MAX_BENCH_NUMBERS + 10, round));
		REQUIRE(word.size() == 1);
		CHECK(word[0].benches.size() == Protocol::MAX_BENCH_NUMBERS);
		for (const auto& entry : word[0].benches) {
			++told[entry.bench];
		}
	}
	CHECK(told.size() == many.size());
	for (const auto& [bench, times] : told) {
		CHECK(times >= 3);
	}
}

TEST("settlement figures: a message that could not go out is told again")
{
	WorkshopNumbers::Reporter reporter;
	const std::array          both{ Built(SANCTUARY), Built(RED_ROCKET) };
	auto                      first = reporter.Update(both);
	REQUIRE(first.size() == 2);
	reporter.Unsent(first[0]);  // the queue was full at the first: neither went
	reporter.Unsent(first[1]);
	const auto again = reporter.Update(both);
	REQUIRE(again.size() == 2);
	CHECK(again[0].benches.empty());
	CHECK(again[1].benches.size() == 2);

	const std::array one{ Built(SANCTUARY) };
	const auto       lost = reporter.Update(one);
	REQUIRE(lost.size() == 1);
	reporter.Unsent(lost[0]);
	const auto retold = reporter.Update(one);
	REQUIRE(retold.size() == 1);
	CHECK(retold[0].benches == std::vector{ NotOurs(RED_ROCKET) });
	CHECK(reporter.Update(one).empty());

	auto more = one;
	more[0].beds = 9;
	const auto changed = reporter.Update(more);
	REQUIRE(changed.size() == 1);
	reporter.Unsent(changed[0]);
	const auto rechanged = reporter.Update(more);
	REQUIRE(rechanged.size() == 1);
	CHECK(rechanged[0].benches == std::vector{ more[0] });
}

TEST("settlement figures: the table keeps the last figures of each settlement and passes on only what changed")
{
	BenchNumbersBook::Book book;
	CHECK(!book.Take({}));  // forget them all with nothing kept: nothing to pass on
	const auto first = book.Take({ { Built(SANCTUARY), Built(RED_ROCKET) } });
	REQUIRE(first);
	CHECK(first->benches.size() == 2);
	CHECK(!book.Take({ { Built(SANCTUARY), Built(RED_ROCKET) } }));  // the same again: nothing

	auto more = Built(SANCTUARY);
	more.defense = 20;
	const auto changed = book.Take({ { more, Built(RED_ROCKET) } });
	REQUIRE(changed);
	CHECK(changed->benches == std::vector{ more });
	CHECK(book.Of(SANCTUARY) == more);
	CHECK(!book.Of(ABERNATHY));

	// Not the host's any more: forgotten, and said so once.
	const auto lost = book.Take({ { NotOurs(RED_ROCKET), NotOurs(ABERNATHY) } });
	REQUIRE(lost);
	CHECK(lost->benches == std::vector{ NotOurs(RED_ROCKET) });
	CHECK(!book.Holds(RED_ROCKET));
	CHECK(!book.Take({ { NotOurs(RED_ROCKET) } }));
	CHECK(book.Benches() == 1);

	// Forget them all.
	const auto forgot = book.Take({});
	REQUIRE(forgot);
	CHECK(forgot->benches.empty());
	CHECK(book.Benches() == 0);
	CHECK(!book.Clear());
	REQUIRE(book.Take({ { Built(SANCTUARY) } }));
	CHECK(book.Clear());
	CHECK(!book.Of(SANCTUARY));
}

TEST("settlement figures: a joiner is told every settlement, in messages the wire takes; the table has its limit")
{
	BenchNumbersBook::Book book;
	CHECK(book.All().empty());
	const auto many = Many(Protocol::MAX_BENCH_NUMBERS);
	REQUIRE(book.Take({ many }));
	REQUIRE(book.Take({ Many(10, 7) }));  // ten of them change
	REQUIRE(book.Take({ { Built(SANCTUARY) } }));
	const auto all = book.All();
	REQUIRE(all.size() == 2);
	CHECK(all[0].benches.size() == Protocol::MAX_BENCH_NUMBERS);
	CHECK(all[1].benches.size() == 1);
	std::size_t changed = 0;
	for (const auto& word : all) {
		const auto back = Protocol::DecodeBenchNumbers(Protocol::Encode(word, Protocol::MessageType::kBenchNumbers));
		REQUIRE(back);
		CHECK(*back == word);
		for (const auto& entry : word.benches) {
			CHECK(book.Of(entry.bench) == entry);
			changed += entry.food == 7 ? 1 : 0;
		}
	}
	CHECK(changed == 10);

	// Past the limit a new settlement is not taken; one already kept still changes.
	BenchNumbersBook::Book full;
	for (std::uint32_t i = 0; i < BenchNumbersBook::Book::MAX_BENCHES + 5; ++i) {
		(void)full.Take({ { Built(0x0004'0000 + i) } });
	}
	CHECK(full.Benches() == BenchNumbersBook::Book::MAX_BENCHES);
	auto kept = Built(0x0004'0000);
	kept.water = 99;
	CHECK(full.Take({ { kept } }));
}

TEST("settlement figures: a friend's bar answers from the host's figures while it holds them, and from its own game without")
{
	WorkshopNumbers::Heard heard;
	// Nothing told yet (the first second after a join, the host away, the rule off): ask the engine, as before.
	CHECK(!heard.Hosts(SANCTUARY));
	CHECK(!heard.Of(SANCTUARY));
	for (std::size_t what = 0; what < WorkshopNumbers::BARS; ++what) {
		CHECK(!heard.OnBar(SANCTUARY, static_cast<Bar>(what)));
	}

	CHECK(heard.Take({ { Built(SANCTUARY) } }));
	CHECK(!heard.Take({ { Built(SANCTUARY) } }));
	CHECK(heard.Hosts(SANCTUARY));
	CHECK(heard.OnBar(SANCTUARY, Bar::kPower) == 5);
	CHECK(heard.OnBar(SANCTUARY, Bar::kPopulation) == 3);
	CHECK(heard.OnBar(SANCTUARY, Bar::kFoodRating) == -1);
	CHECK(heard.OnBar(SANCTUARY, Bar::kWaterRating) == 0);
	CHECK(!heard.OnBar(RED_ROCKET, Bar::kPower));  // not the host's: our own world's figure
	CHECK(!heard.OnBar(0, Bar::kPower));

	// The host builds a turret: the next answer is the new figure.
	auto more = Built(SANCTUARY);
	more.defense = 13;
	CHECK(heard.Take({ { more } }));
	CHECK(heard.OnBar(SANCTUARY, Bar::kDefense) == 13);

	// The host no longer owns it, or left (forget them all), or we were welcomed anew: our own game's figures again.
	CHECK(heard.Take({ { NotOurs(SANCTUARY) } }));
	CHECK(!heard.OnBar(SANCTUARY, Bar::kDefense));
	CHECK(heard.Take({ { Built(SANCTUARY), Built(RED_ROCKET) } }));
	CHECK(heard.Benches() == 2);
	CHECK(heard.Take({}));
	CHECK(!heard.Hosts(SANCTUARY));
	CHECK(!heard.Hosts(RED_ROCKET));
	CHECK(heard.Take({ { Built(SANCTUARY) } }));
	CHECK(heard.Clear());
	CHECK(!heard.Clear());
	CHECK(!heard.OnBar(SANCTUARY, Bar::kPower));
}

TEST("settlement figures: no worth of friends' pieces is put on the host's workbench, and what was on it comes off")
{
	using namespace ResourceMirror;
	WorkshopNumbers::Heard heard;
	std::map<std::uint32_t, Values> totals{ { SANCTUARY, Values{ 2.0f, 0.0f, 5.0f, 1.0f, 3.0f, 0.0f } }, { RED_ROCKET, Values{ 0.0f, 4.0f, 0.0f, 0.0f, 0.0f, 0.0f } } };

	// No figures: the worth goes on as before.
	auto before = totals;
	CHECK(WorkshopNumbers::WithoutHosts(before, heard) == 0);
	CHECK(before == totals);

	// The workbenches' modifiers, as the engine holds them.
	struct Benches
	{
		std::map<std::uint32_t, Values> on;

		bool Add(std::uint32_t a_bench, Kind a_kind, float a_delta)
		{
			on[a_bench][a_kind] += a_delta;
			return true;
		}
	};
	Benches edit;
	auto&   bench = edit.on;
	Applier applier;
	(void)applier.Apply(edit, before, Clock::time_point{ 1s });
	CHECK(bench[SANCTUARY][kSafety] == 5.0f);
	CHECK(bench[RED_ROCKET][kWater] == 4.0f);

	// The host's figures for Sanctuary arrive: its worth is left out, so the applier takes off what it had put there.
	REQUIRE(heard.Take({ { Built(SANCTUARY) } }));
	auto after = totals;
	CHECK(WorkshopNumbers::WithoutHosts(after, heard) == 1);
	CHECK(!after.contains(SANCTUARY));
	CHECK(after.contains(RED_ROCKET));
	(void)applier.Apply(edit, after, Clock::time_point{ 2s });
	CHECK(!Any(bench[SANCTUARY]));
	CHECK(bench[RED_ROCKET][kWater] == 4.0f);

	// The host leaves: the worth goes back on.
	REQUIRE(heard.Take({}));
	auto later = totals;
	CHECK(WorkshopNumbers::WithoutHosts(later, heard) == 0);
	(void)applier.Apply(edit, later, Clock::time_point{ 3s });
	CHECK(bench[SANCTUARY][kSafety] == 5.0f);
}

TEST("settlement figures: entering workshop mode at the host's settlement says its figures once; at one of our own, nothing")
{
	WorkshopNumbers::Heard     heard;
	WorkshopNumbers::Announcer announcer;
	CHECK(!announcer.Update(false, 0, heard));
	CHECK(!announcer.Update(true, SANCTUARY, heard));  // no figures yet: nothing said
	REQUIRE(heard.Take({ { Built(SANCTUARY) } }));
	const auto said = announcer.Update(true, SANCTUARY, heard);  // they came while it was open: said now
	REQUIRE(said);
	CHECK(*said == Built(SANCTUARY));
	CHECK(!announcer.Update(true, SANCTUARY, heard));  // once a stay
	auto more = Built(SANCTUARY);
	more.food = 9;
	REQUIRE(heard.Take({ { more } }));
	CHECK(!announcer.Update(true, SANCTUARY, heard));  // a change is the bar's to show, not another line

	CHECK(!announcer.Update(false, 0, heard));
	const auto again = announcer.Update(true, SANCTUARY, heard);  // the next stay
	REQUIRE(again);
	CHECK(again->food == 9);
	CHECK(!announcer.Update(true, 0, heard));  // the workbench not known this frame
	CHECK(!announcer.Update(true, RED_ROCKET, heard));  // our own settlement
	CHECK(!announcer.Update(false, 0, heard));
	CHECK(!announcer.Update(true, RED_ROCKET, heard));
}

TEST("settlement figures: every half minute the host's game tells every settlement again, and the table passes nothing of it on")
{
	WorkshopNumbers::Reporter               reporter;
	BenchNumbersBook::Book                  server;
	const WorkshopNumbers::Clock::time_point start{ 10s };
	const std::array                        both{ Built(SANCTUARY), Built(RED_ROCKET) };
	REQUIRE(reporter.Due(start));
	const auto first = reporter.Update(both);
	REQUIRE(first.size() == 2);
	CHECK(!server.Take(first[0]));
	CHECK(server.Take(first[1]));

	const std::array one{ Built(SANCTUARY) };
	REQUIRE(reporter.Due(start + 1s));
	const auto lost = reporter.Update(one);
	REQUIRE(lost.size() == 1);  // a busy second: the server's cap drops this one, so its table still holds Red Rocket
	for (auto after = 2s; after < 30s; after += 1s) {
		REQUIRE(reporter.Due(start + after));
		CHECK(reporter.Update(one).empty());
	}
	REQUIRE(reporter.Due(start + 30s));
	const auto again = reporter.Update(one);
	REQUIRE(again.size() == 1);
	CHECK((again[0].benches == std::vector{ NotOurs(RED_ROCKET), one[0] }));
	const auto news = server.Take(again[0]);  // the dropped word is made up for; Sanctuary's same figures are no news
	REQUIRE(news);
	CHECK(news->benches == std::vector{ NotOurs(RED_ROCKET) });
	REQUIRE(reporter.Due(start + 31s));
	CHECK(reporter.Update(one).empty());

	REQUIRE(reporter.Due(start + 60s));
	const auto third = reporter.Update(one);
	REQUIRE(third.size() == 1);
	CHECK(!server.Take(third[0]));  // nothing changed: the friends hear nothing
}
