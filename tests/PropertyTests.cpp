// Property-style tests: the game-free classes are driven with many random inputs from a fixed seed, and what must hold of
// every answer is checked, rather than one hand-picked case each. A failure prints the seed and the case, so it can be
// pinned down in a plain test.

#include "Test.h"

#include "FormIds.h"
#include "Protocol.h"
#include "Redact.h"
#include "SessionLevel.h"
#include "TokenBucket.h"
#include "WorkshopStore.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace
{
	using namespace Protocol;
	using MT = MessageType;

	// One generator per test, seeded the same every run: a failure is the same failure tomorrow.
	struct Random
	{
		std::mt19937_64 engine;

		explicit Random(std::uint64_t a_seed) :
			engine(a_seed)
		{}

		std::uint32_t U32(std::uint32_t a_low, std::uint32_t a_high) { return std::uniform_int_distribution<std::uint32_t>(a_low, a_high)(engine); }
		std::uint64_t U64() { return engine(); }
		int           Int(int a_low, int a_high) { return std::uniform_int_distribution<int>(a_low, a_high)(engine); }
		float         Float(float a_low, float a_high) { return std::uniform_real_distribution<float>(a_low, a_high)(engine); }
		bool          Chance(double a_p) { return std::uniform_real_distribution<double>(0.0, 1.0)(engine) < a_p; }

		template <class C>
		const auto& Pick(const C& a_choices)
		{
			return a_choices[std::uniform_int_distribution<std::size_t>(0, a_choices.size() - 1)(engine)];
		}
	};

	// Every strict prefix of a packet is refused by a_decode.
	template <class Decode>
	bool RefusesEveryPrefix(const std::vector<std::uint8_t>& a_packet, Decode a_decode)
	{
		for (std::size_t n = 0; n < a_packet.size(); ++n) {
			if (a_decode(std::span<const std::uint8_t>{ a_packet.data(), n })) {
				return false;
			}
		}
		return true;
	}

	constexpr std::array<const char*, 12> PLUGINS{ "Fallout4.esm", "DLCRobot.esm", "DLCworkshop01.esm", "DLCCoast.esm", "DLCworkshop02.esm", "DLCworkshop03.esm",
		"DLCNukaWorld.esm", "Unofficial Fallout 4 Patch.esp", "Homemaker.esm", "SettlementMenuManager.esp", "F4MP.esp", "ccBGSFO4044-HellfirePowerArmor.esl" };
}

// ---- form IDs across load orders -----------------------------------------------------------------------------------

TEST("property: a form ID translated to the session's numbering comes back as itself, names the same plugin on both sides, and one of a plugin the other side lacks has no number there")
{
	Random random{ 0xF04D1D5 };
	for (int round = 0; round < 200; ++round) {
		// Two load orders drawn from the same plugins: each side loads a subset, in its own order, with its own split
		// between full and light plugins.
		const auto draw = [&] {
			std::vector<std::string> order;
			std::vector<std::string> pool(PLUGINS.begin(), PLUGINS.end());
			std::ranges::shuffle(pool, random.engine);
			const auto full = random.Int(1, static_cast<int>(pool.size()) - 1);
			order.assign(pool.begin(), pool.begin() + full);
			order.emplace_back(LIGHT_PLUGINS);
			const auto light = random.Int(0, static_cast<int>(pool.size()) - full);
			order.insert(order.end(), pool.begin() + full, pool.begin() + full + light);
			return order;
		};
		const auto local = draw();
		const auto session = draw();
		const FormIds::Mapper mapper{ local, session };
		const auto localTable = FormIds::Table::Of(local);
		const auto sessionTable = FormIds::Table::Of(session);

		for (int i = 0; i < 50; ++i) {
			// A form of one of our plugins: a random place among those we load, and a random number in it.
			const bool light = random.Chance(0.4) && !localTable.light.empty();
			const auto index = light ? random.U32(0, static_cast<std::uint32_t>(localTable.light.size()) - 1) : random.U32(0, static_cast<std::uint32_t>(localTable.full.size()) - 1);
			const auto localPart = light ? random.U32(1, FormIds::LIGHT_LOCAL) : random.U32(1, FormIds::FULL_LOCAL);
			const auto id = FormIds::Compose(FormIds::Place{ light, static_cast<std::uint16_t>(index) }, localPart);
			REQUIRE(id);
			const auto& name = light ? localTable.light[index] : localTable.full[index];
			const auto there = sessionTable.Find(name);
			const auto translated = mapper.ToSession(*id);
			const auto where = std::format("round {} id {:08X} ({}) -> {}", round, *id, name, translated ? std::format("{:08X}", *translated) : "none");
			if (!there) {
				if (translated) {
					Test::Fail(__FILE__, __LINE__, "a plugin the session lacks got a number: " + where);
				}
				continue;
			}
			// The session's plugin may be light where ours is full: a number past 12 bits can't be said there.
			if (there->light && localPart > FormIds::LIGHT_LOCAL) {
				if (translated) {
					Test::Fail(__FILE__, __LINE__, "a form past a light plugin's numbers got a number: " + where);
				}
				continue;
			}
			if (!translated) {
				Test::Fail(__FILE__, __LINE__, "a shared plugin's form got no number: " + where);
				continue;
			}
			CHECK(FormIds::LocalPart(*translated) == localPart);
			CHECK(mapper.SessionPlugin(*translated) == name);
			CHECK(mapper.LocalPlugin(*id) == name);
			const auto back = mapper.ToLocal(*translated);
			if (!back || *back != *id) {
				Test::Fail(__FILE__, __LINE__, "didn't come back as itself: " + where);
			}
		}
		// Runtime forms and 0 are nobody's: never translated, never refused.
		CHECK(mapper.ToSession(0) == std::optional<std::uint32_t>{ 0 } || !mapper.ToSession(0));
		const auto runtime = 0xFF000000u | random.U32(1, 0xFFFFFF);
		CHECK(mapper.ToSession(runtime) == std::optional{ runtime });
		CHECK(mapper.ToLocal(runtime) == std::optional{ runtime });
	}
}

// ---- the session's NPC level -------------------------------------------------------------------------------------------

TEST("property: the session's NPC level and legendary chance stay within their bounds and follow their rule for any players")
{
	Random random{ 0x5E55104 };
	for (int round = 0; round < 2000; ++round) {
		std::vector<SessionLevel::Player> players;
		const auto count = random.Int(0, 6);
		for (int i = 0; i < count; ++i) {
			players.push_back({ static_cast<std::uint16_t>(random.Chance(0.2) ? 0 : random.U32(1, 1300)), random.Chance(0.3), static_cast<std::uint16_t>(random.Chance(0.2) ? 0 : random.U32(1, 12000)) });
		}
		const auto current = static_cast<std::uint16_t>(random.Chance(0.3) ? 0 : random.U32(1, MAX_IDENTITY_LEVEL));
		const auto currentLegendary = static_cast<std::uint16_t>(random.Chance(0.3) ? 0 : random.U32(1, MAX_LEGENDARY_MULT));
		const auto heard = [&](auto a_of) {
			std::vector<std::uint32_t> out;
			for (const auto& player : players) {
				if (a_of(player) != 0) {
					out.push_back(a_of(player));
				}
			}
			return out;
		};
		const auto levels = heard([](const SessionLevel::Player& a_player) { return a_player.level; });
		const auto mults = heard([](const SessionLevel::Player& a_player) { return a_player.legendaryMult; });
		const auto say = [&](std::string_view a_what, auto a_value) { return std::format("round {}: {} = {}", round, a_what, a_value); };

		CHECK(SessionLevel::Next(NpcLevelRule::kOwn, current, players) == 0);
		CHECK(SessionLevel::NextLegendary(LegendaryRule::kOwn, currentLegendary, players) == 0);

		const auto highest = SessionLevel::Next(NpcLevelRule::kHighest, current, players);
		CHECK(highest <= MAX_IDENTITY_LEVEL);
		CHECK(highest >= current);  // never goes down
		for (const auto level : levels) {
			if (highest < (std::min<std::uint32_t>)(level, MAX_IDENTITY_LEVEL)) {
				Test::Fail(__FILE__, __LINE__, say("highest below a player's level", highest));
			}
		}
		if (levels.empty()) {
			CHECK(highest == current);
		}

		const auto average = SessionLevel::Next(NpcLevelRule::kAverage, current, players);
		CHECK(average <= MAX_IDENTITY_LEVEL);
		if (levels.empty()) {
			CHECK(average == current);  // nobody heard from: stays
		} else {
			const auto low = (std::min<std::uint32_t>)(std::ranges::min(levels), MAX_IDENTITY_LEVEL);
			const auto high = (std::min<std::uint32_t>)(std::ranges::max(levels), MAX_IDENTITY_LEVEL);
			if (average < low || average > high) {
				Test::Fail(__FILE__, __LINE__, say("average outside the players' levels", average));
			}
			// The mean of what was heard, a half rounded up.
			std::uint64_t sum = 0;
			for (const auto level : levels) {
				sum += level;
			}
			const auto mean = (std::min<std::uint64_t>)((sum + levels.size() / 2) / levels.size(), MAX_IDENTITY_LEVEL);
			CHECK(average == mean);
		}

		const auto host = SessionLevel::Next(NpcLevelRule::kHost, current, players);
		const auto hostPlayer = std::ranges::find_if(players, [](const SessionLevel::Player& a_player) { return a_player.host && a_player.level != 0; });
		if (hostPlayer == players.end()) {
			CHECK(host == current);
		} else {
			CHECK(host == (std::min<std::uint32_t>)(hostPlayer->level, MAX_IDENTITY_LEVEL));
		}

		const auto legendaryAverage = SessionLevel::NextLegendary(LegendaryRule::kAverage, currentLegendary, players);
		CHECK(legendaryAverage <= MAX_LEGENDARY_MULT);
		if (mults.empty()) {
			CHECK(legendaryAverage == currentLegendary);
		} else {
			const auto low = (std::min<std::uint32_t>)(std::ranges::min(mults), MAX_LEGENDARY_MULT);
			const auto high = (std::min<std::uint32_t>)(std::ranges::max(mults), MAX_LEGENDARY_MULT);
			if (legendaryAverage < low || legendaryAverage > high) {
				Test::Fail(__FILE__, __LINE__, say("legendary average outside the games' multipliers", legendaryAverage));
			}
		}
		const auto legendaryHost = SessionLevel::NextLegendary(LegendaryRule::kHost, currentLegendary, players);
		const auto hostMult = std::ranges::find_if(players, [](const SessionLevel::Player& a_player) { return a_player.host && a_player.legendaryMult != 0; });
		CHECK(legendaryHost == (hostMult == players.end() ? currentLegendary : (std::min<std::uint32_t>)(hostMult->legendaryMult, MAX_LEGENDARY_MULT)));
	}
}

// ---- the workshop store --------------------------------------------------------------------------------------------

TEST("property: whatever is built, moved, scrapped, lost and found at settlements, the workshop store keeps within its caps, relays only what changes, and tells each builder only their own standing pieces")
{
	Random random{ 0xB01D };
	constexpr std::size_t MAX_BUILT = 40;
	constexpr std::size_t MAX_SCRAPS = 12;
	constexpr std::size_t MAX_LOST = 8;
	for (int round = 0; round < 60; ++round) {
		WorkshopStore store{ MAX_BUILT, MAX_SCRAPS, MAX_LOST };
		// What each builder's own world holds standing: the model the store's answers are checked against.
		std::map<WorkshopStore::Key, WorkshopItem> standing;
		std::set<WorkshopStore::Key>               scrappedWorld;
		std::size_t                                relayed = 0;
		const std::array<std::uint64_t, 3>         characters{ 0xC4A1, 0xC4A2, 0xC4A3 };
		for (int step = 0; step < 400; ++step) {
			WorkshopItem item;
			item.playerId = random.U32(1, 3);
			item.character = characters[item.playerId - 1];
			const bool worldPiece = random.Chance(0.2);
			item.refId = worldPiece ? 0x0001F000 + random.U32(0, 15) : 0xFF000000 + random.U32(1, 60);
			item.base = 0x0002A000 + random.U32(1, 9);
			item.workshop = 0x000250FE;
			item.position[0] = random.Float(-100.0f, 100.0f);
			const WorkshopStore::Key key{ item.playerId, item.refId };
			const auto kind = random.Int(0, 9);
			if (kind < 5) {
				item.op = WorkshopOp::kPlaced;
				const bool known = store.Built().contains(key);
				const auto outcome = store.Report(item);
				if (!known && store.Built().size() >= MAX_BUILT && !outcome.relay) {
					CHECK(outcome.full);
				} else {
					CHECK(outcome.relay);
					CHECK(!outcome.full);
					standing[key] = item;
					if (!worldPiece) {
						CHECK(store.Built().at(key) == item);
					}
				}
				if (outcome.relay) {
					++relayed;
				}
			} else if (kind < 8) {
				item.op = WorkshopOp::kScrapped;
				const bool wasBuilt = store.Built().contains(key);
				const bool wasScrapped = store.Scraps().contains(key);
				const auto outcome = store.Report(item);
				standing.erase(key);
				if (wasScrapped) {
					CHECK(!outcome.relay);  // already gone for everyone
				} else if (!worldPiece) {
					CHECK(outcome.relay == wasBuilt);  // nothing anyone knew about, or something they built
				} else if (store.Scraps().size() < MAX_SCRAPS || store.Scraps().contains(key)) {
					CHECK(outcome.relay);
					scrappedWorld.insert(key);
				} else {
					CHECK(outcome.full);
					CHECK(outcome.relay == wasBuilt);
				}
			} else {
				// The builder's game says its world lacks the piece: only a built piece of theirs, of the same base, goes.
				const auto kept = store.Built().find(key);
				const bool goes = kept != store.Built().end() && !worldPiece && (random.Chance(0.7) ? (item.base = kept->second.base, true) : kept->second.base == item.base);
				const auto scrap = store.Gone(item, item.character);
				CHECK(scrap.has_value() == goes);
				if (scrap) {
					CHECK(scrap->op == WorkshopOp::kScrapped);
					CHECK(scrap->refId == item.refId && scrap->playerId == item.playerId);
					CHECK(store.IsLost(key));
					standing.erase(key);
				}
				// Another character's world knows nothing of it.
				CHECK(!store.Gone(item, 0xDEAD));
			}
			CHECK(store.Built().size() <= MAX_BUILT);
			CHECK(store.Scraps().size() <= MAX_SCRAPS);
			CHECK(store.LostCount() <= MAX_LOST);
			CHECK(store.Size() == store.Built().size() + store.Scraps().size());
		}
		// What each builder is asked to look for: their own standing pieces (not the world's they moved) and what they
		// lost, no one else's, and nothing for a character not known.
		for (std::uint32_t player = 1; player <= 3; ++player) {
			const auto own = store.BuiltBy(player, characters[player - 1]);
			std::set<std::uint32_t> refs;
			for (const auto& piece : own) {
				CHECK(piece.playerId == player);
				CHECK(!IsShareableRef(piece.refId));
				CHECK(piece.op == WorkshopOp::kPlaced || piece.op == WorkshopOp::kLost);
				if (piece.op == WorkshopOp::kPlaced) {
					CHECK(standing.contains({ player, piece.refId }));
				} else {
					CHECK(store.IsLost({ player, piece.refId }));
				}
				CHECK(refs.insert(piece.refId).second);  // each once
			}
			for (const auto& [key, piece] : standing) {
				if (key.first == player && !IsShareableRef(key.second)) {
					CHECK(refs.contains(key.second));
				}
			}
			CHECK(store.BuiltBy(player, 0).empty());
			CHECK(store.BuiltBy(player, 0xBEEF).empty());
		}
		// Everything the store keeps reads back through ForEachKept, the lost pieces last and in the order they were lost.
		std::size_t seen = 0;
		bool        lostSeen = false;
		store.ForEachKept([&](const WorkshopStore::Key&, const WorkshopItem& a_item) {
			++seen;
			if (a_item.op == WorkshopOp::kLost) {
				lostSeen = true;
			} else {
				CHECK(!lostSeen);
			}
		});
		CHECK(seen == store.Size() + store.LostCount());
		CHECK(relayed > 0);
	}
}

// ---- the log's privacy net ----------------------------------------------------------------------------------------------

TEST("property: whatever a log line holds, Scrub leaves no SteamID64 and no whole address in it, changes nothing else, and reads the same scrubbed twice")
{
	Random random{ 0x5C7B };
	const std::regex steamId{ "7656[0-9]{13}" };
	const std::regex address{ "(^|[^0-9.])([0-9]{1,3})\\.([0-9]{1,3})\\.([0-9]{1,3})\\.([0-9]{1,3})($|[^0-9.])" };
	const std::array<const char*, 10> words{ "server:", "player", "joined", "from", "lobby", "world", "0x0001F00D", "FF000042", "took", "door" };
	for (int round = 0; round < 1000; ++round) {
		std::string line;
		std::string plain;  // the same line with no id or address in it
		bool        secret = false;
		const auto  parts = random.Int(1, 8);
		for (int i = 0; i < parts; ++i) {
			std::string part;
			std::string safe;
			switch (random.Int(0, 6)) {
			case 0:
				part = std::format("7656{:013}", random.U64() % 10000000000000ull);  // a SteamID64
				safe = "~000000";
				secret = true;
				break;
			case 1:
				part = std::format("{}.{}.{}.{}", random.Int(1, 223), random.Int(0, 255), random.Int(0, 255), random.Int(0, 255));  // an address
				if (!part.starts_with("127.")) {
					secret = true;
				}
				safe = "x.x.x.x";
				break;
			case 2:
				part = std::format("109775{:012}", random.U64() % 1000000000000ull);  // a lobby id, 18 digits: stays
				safe = part;
				break;
			case 3:
				part = std::format("{}", random.U32(0, 99999));
				safe = part;
				break;
			case 4:
				part = "127.0.0.1";  // loopback stays
				safe = part;
				break;
			default:
				part = random.Pick(words);
				safe = part;
				break;
			}
			line += (i ? " " : "") + part;
			plain += (i ? " " : "") + safe;
		}
		const auto once = Redact::Scrub(line);
		const auto where = std::format("round {}: \"{}\" -> \"{}\"", round, line, once);
		if (std::regex_search(once, steamId)) {
			Test::Fail(__FILE__, __LINE__, "a SteamID64 survived: " + where);
		}
		for (std::sregex_iterator it{ once.begin(), once.end(), address }, end; it != end; ++it) {
			const auto& match = *it;
			if (match[2] != "127") {
				Test::Fail(__FILE__, __LINE__, "a whole address survived: " + where);
			}
		}
		if (Redact::Scrub(once) != once) {
			Test::Fail(__FILE__, __LINE__, "not the same scrubbed twice: " + where);
		}
		if (!secret && once != line) {
			Test::Fail(__FILE__, __LINE__, "a line with nothing to hide was changed: " + where);
		}
		if (Redact::Scrub(plain) != plain) {
			Test::Fail(__FILE__, __LINE__, std::format("round {}: the plain line was changed: \"{}\" -> \"{}\"", round, plain, Redact::Scrub(plain)));
		}
	}
}

// ---- rate budgets --------------------------------------------------------------------------------------------------------

TEST("property: a token bucket never grants more than its burst plus what the time since refilled, and always grants once it has had time to refill one")
{
	Random random{ 0x70C3 };
	using Clock = TokenBucket::Clock;
	for (int round = 0; round < 300; ++round) {
		const auto burst = static_cast<double>(random.Int(1, 20));
		const auto rate = static_cast<double>(random.Int(1, 50));
		TokenBucket bucket{ burst, rate };
		auto        now = Clock::time_point{} + std::chrono::seconds(random.Int(0, 1000));
		const auto  start = now;
		double      granted = 0;
		double      sinceEmpty = 0;    // seconds since the last refusal (the budget was empty then)
		double      grantedSince = 0;  // takes granted since
		for (int step = 0; step < 300; ++step) {
			const auto wait = random.Chance(0.3) ? std::chrono::milliseconds(0) : std::chrono::milliseconds(random.Int(1, 2000));
			now += wait;
			sinceEmpty += std::chrono::duration<double>(wait).count();
			const bool got = bucket.Take(now);
			if (got) {
				++granted;
				++grantedSince;
			}
			const auto elapsed = std::chrono::duration<double>(now - start).count();
			if (granted > burst + elapsed * rate + 1e-6) {
				Test::Fail(__FILE__, __LINE__, std::format("round {} step {}: {} granted in {:.3f} s with burst {} and {} a second", round, step, granted, elapsed, burst, rate));
				break;
			}
			// A refusal only while the budget is empty: everything the time since the last refusal refilled (less than
			// one token's worth) was granted meanwhile.
			if (!got) {
				const auto refilled = (std::min)(burst, sinceEmpty * rate);
				if (refilled - grantedSince >= 1.0 + 1e-6) {
					Test::Fail(__FILE__, __LINE__, std::format("round {} step {}: refused with {:.2f} refilled and {} granted since the last refusal ({} a second)", round, step, refilled, grantedSince, rate));
					break;
				}
				sinceEmpty = 0;
				grantedSince = 0;
			}
		}
	}
}

// ---- the wire -------------------------------------------------------------------------------------------------------------

TEST("property: door states, container changes and workshop pieces read back exactly as sent for any values, and every prefix of each is refused")
{
	Random random{ 0xA11CE };
	for (int round = 0; round < 500; ++round) {
		RefState state;
		state.refId = random.Chance(0.5) ? random.U32(1, 0x00FFFFFF) : (0xFE000000u | random.U32(1, 0xFFFFFF));
		state.open = static_cast<std::uint8_t>(random.Int(0, 2));
		state.locked = static_cast<std::uint8_t>(random.Int(0, 2));
		state.playerId = random.Chance(0.5) ? 0 : random.U32(1, 1000);
		for (const auto type : { MT::kReportRefState, MT::kRefStateChanged }) {
			const auto packet = Encode(state, type);
			const auto back = DecodeRefState(packet);
			REQUIRE(back);
			CHECK(*back == state);
			CHECK(RefusesEveryPrefix(packet, [](auto a_d) { return DecodeRefState(a_d); }));
		}

		ContainerChange change{ random.U32(1, 0xFFFFFFFF), random.U32(1, 0x00FFFFFF), static_cast<std::int32_t>(random.U32(0, 0xFFFFFFFF)) };
		const auto changePacket = Encode(change, MT::kReportContainer);
		const auto changeBack = DecodeContainerChange(changePacket);
		REQUIRE(changeBack);
		CHECK(*changeBack == change);
		CHECK(RefusesEveryPrefix(changePacket, [](auto a_d) { return DecodeContainerChange(a_d); }));

		WorkshopItem item;
		item.playerId = random.U32(0, 100);
		item.refId = random.Chance(0.5) ? random.U32(1, 0x00FFFFFF) : (0xFF000000u | random.U32(1, 0xFFFFFF));
		item.op = random.Chance(0.3) ? WorkshopOp::kScrapped : WorkshopOp::kPlaced;  // a move is a placement at a new transform
		item.base = 0x00000001 + random.U32(0, 0x00FFFFFE);
		item.workshop = random.U32(1, 0x00FFFFFF);
		for (float& v : item.position) {
			v = random.Float(-200000.0f, 200000.0f);
		}
		for (float& v : item.rotation) {
			v = random.Float(-7.0f, 7.0f);
		}
		item.scale = random.Float(0.01f, 100.0f);
		if (random.Chance(0.4)) {
			WireShape wire;
			for (float& v : wire.halfExtents) {
				v = random.Float(-MAX_WIRE_HALF_LENGTH, MAX_WIRE_HALF_LENGTH);
			}
			wire.slack = random.Float(0.0f, MAX_WIRE_SLACK);
			wire.thickness = random.Float(0.01f, MAX_WIRE_THICKNESS);
			item.wire = wire;
		}
		for (const auto type : { MT::kReportWorkshopItem, MT::kWorkshopItem }) {
			const auto packet = Encode(item, type);
			const auto back = DecodeWorkshopItem(packet);
			if (!back) {
				Test::Fail(__FILE__, __LINE__, std::format("round {}: a sane workshop item was refused (op {}, scale {}, wire {})", round, item.op, item.scale, item.wire.has_value()));
				continue;
			}
			CHECK(*back == item);
			CHECK(RefusesEveryPrefix(packet, [](auto a_d) { return DecodeWorkshopItem(a_d); }));
		}
		// The same items many to a packet, with the server's own character tail or without: all read back.
		std::vector<WorkshopItem> items;
		const auto count = random.Int(1, 8);
		for (int i = 0; i < count; ++i) {
			auto piece = item;
			piece.refId = 0xFF000000u | random.U32(1, 0xFFFFFF);
			piece.op = random.Chance(0.3) ? WorkshopOp::kScrapped : WorkshopOp::kPlaced;
			piece.character = random.Chance(0.5) ? 0 : random.U64();
			items.push_back(piece);
		}
		const auto kept = EncodeKeptWorkshopItems(items);
		const auto keptBack = DecodeKeptWorkshopItems(kept);
		REQUIRE(keptBack);
		CHECK(*keptBack == items);
		CHECK(RefusesEveryPrefix(kept, [](auto a_d) { return DecodeKeptWorkshopItems(a_d); }));
		for (auto& piece : items) {
			piece.character = 0;  // never on the wire
		}
		const auto wire = Encode(items);
		const auto wireBack = DecodeWorkshopItems(wire);
		REQUIRE(wireBack);
		CHECK(*wireBack == items);
		CHECK(RefusesEveryPrefix(wire, [](auto a_d) { return DecodeWorkshopItems(a_d); }));
		CHECK(!DecodeKeptWorkshopItems(wire));  // a wire packet has no characters: refused as a kept one
	}
}

TEST("property: a world state of any size reads back whole on the wire and in the session file, with the pickers the file alone keeps, and refuses every prefix")
{
	Random random{ 0x90B1D };
	for (int round = 0; round < 60; ++round) {
		WorldState in;
		const auto dead = random.Int(0, 30);
		for (int i = 0; i < dead; ++i) {
			in.deadActors.push_back(random.U32(1, 0x00FFFFFF));
		}
		in.containerFirstIndex = random.U32(0, 100000);
		const auto changes = random.Int(0, 20);
		for (int i = 0; i < changes; ++i) {
			in.containerChanges.push_back({ in.containerFirstIndex + static_cast<std::uint32_t>(i), random.U32(0, 10), { random.U32(1, 0x00FFFFFF), random.U32(1, 0x00FFFFFF), random.Int(-50, 50) } });
		}
		const auto picked = random.Int(0, 25);
		for (int i = 0; i < picked; ++i) {
			in.pickedUp.push_back(random.U32(1, 0x00FFFFFF));
			in.pickers.push_back(random.U32(0, 10));
		}
		const auto doors = random.Int(0, 20);
		for (int i = 0; i < doors; ++i) {
			in.refStates.push_back({ random.U32(1, 0x00FFFFFF), static_cast<std::uint8_t>(random.Int(0, 2)), static_cast<std::uint8_t>(random.Int(0, 2)), 0 });
		}
		const auto stages = random.Int(0, 20);
		for (int i = 0; i < stages; ++i) {
			in.questStages.push_back({ random.U32(1, 0x00FFFFFF), static_cast<std::uint16_t>(random.U32(0, 0xFFFF)), random.U32(0, 10) });
		}
		if (random.Chance(0.3)) {
			in.pickers.clear();  // a session file from before pickers were kept
		}
		// A world state tells the stages reached, not who reached them: a stage's player is never on the wire or in the file.
		auto stagesSaid = in.questStages;
		for (auto& stage : stagesSaid) {
			stage.playerId = 0;
		}

		const auto wire = Encode(in);
		const auto onWire = DecodeWorldState(wire);
		REQUIRE(onWire);
		CHECK(onWire->deadActors == in.deadActors);
		CHECK(onWire->containerFirstIndex == in.containerFirstIndex);
		REQUIRE(onWire->containerChanges.size() == in.containerChanges.size());
		for (std::size_t i = 0; i < in.containerChanges.size(); ++i) {
			CHECK(onWire->containerChanges[i].index == in.containerChanges[i].index);
			CHECK(onWire->containerChanges[i].playerId == in.containerChanges[i].playerId);
			CHECK(onWire->containerChanges[i].change == in.containerChanges[i].change);
		}
		CHECK(onWire->pickedUp == in.pickedUp);
		CHECK(onWire->refStates == in.refStates);
		CHECK(onWire->questStages == stagesSaid);
		CHECK(onWire->pickers.empty());
		CHECK(RefusesEveryPrefix(wire, [](auto a_d) { return DecodeWorldState(a_d); }));

		const auto kept = EncodeKeptWorldState(in);
		const auto inFile = DecodeKeptWorldState(kept);
		REQUIRE(inFile);
		CHECK(inFile->pickedUp == in.pickedUp);
		CHECK(inFile->pickers == in.pickers);
		CHECK(inFile->refStates == in.refStates);
		CHECK(inFile->questStages == stagesSaid);
		// A kept state is a wire state with a tail (the pickers' count, 0 or one for each picked-up item, then their IDs). The
		// wire decoder refuses the tail. The file decoder refuses every cut of the record but one: cut exactly before the tail
		// it is a wire packet, which reads as a state naming no pickers (a session file from before pickers were kept has that
		// shape), never as a different state.
		CHECK(!DecodeWorldState(kept));
		for (std::size_t n = 0; n < kept.size(); ++n) {
			const auto cut = DecodeKeptWorldState(std::span<const std::uint8_t>{ kept.data(), n });
			if (n == wire.size()) {
				if (!cut || !cut->pickers.empty() || cut->pickedUp != in.pickedUp || cut->questStages != stagesSaid) {
					Test::Fail(__FILE__, __LINE__, std::format("round {}: the record without its tail didn't read as the wire's state", round));
				}
			} else if (cut) {
				Test::Fail(__FILE__, __LINE__, std::format("round {}: a file record cut at {} of {} bytes was read", round, n, kept.size()));
			}
		}
	}
}

TEST("property: a status with any level, health and flags reads back as sent, and one past the health the wire allows is refused")
{
	Random random{ 0x57A7 };
	for (int round = 0; round < 500; ++round) {
		PlayerStatus status;
		status.playerId = random.U32(0, 1000);
		status.location = random.U32(0, 0x00FFFFFF);
		status.cell = random.U32(0, 0x00FFFFFF);
		status.health = static_cast<std::uint8_t>(random.Int(0, 100));
		status.level = static_cast<std::uint16_t>(random.U32(0, 0xFFFF));
		status.downed = random.Chance(0.5);
		status.opening = random.Chance(0.5);
		status.light = random.Chance(0.5);
		status.powerArmor = random.Chance(0.5);
		status.lends = random.Chance(0.5);
		const auto packet = Encode(status, MT::kReportStatus);
		const auto back = DecodePlayerStatus(packet);
		REQUIRE(back);
		CHECK(back->level == status.level);
		CHECK(back->health == status.health);
		CHECK(back->location == status.location && back->cell == status.cell);
		CHECK(back->downed == status.downed && back->opening == status.opening && back->light == status.light && back->powerArmor == status.powerArmor && back->lends == status.lends);
		CHECK(RefusesEveryPrefix(packet, [](auto a_d) { return DecodePlayerStatus(a_d); }));
		auto hurt = status;
		hurt.health = static_cast<std::uint8_t>(random.Int(101, 255));
		CHECK(!DecodePlayerStatus(Encode(hurt, MT::kReportStatus)));
	}
}
