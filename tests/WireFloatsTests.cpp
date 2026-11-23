// Floats a game reads from its own state are checked before they go on the wire (Protocol::Finite, HealthFraction,
// SaneTransform). The engine can hand us an infinite value or no number at all: a health maximum a console command
// set to infinity made every health share 0, and friends' copies of the NPC were set to no health; a maximum that was
// no number sent a report their games refused, again every quarter of a second. Each test names what players would see
// go wrong if it failed.

#include "Test.h"

#include "Protocol.h"
#include "game/DownRules.h"

#include <array>
#include <limits>
#include <vector>

namespace
{
	constexpr float INF = std::numeric_limits<float>::infinity();
	constexpr float NOT_A_NUMBER = std::numeric_limits<float>::quiet_NaN();
}

TEST("wire floats: Finite is true only when every value is a number, for loose values, arrays and spans")
{
	CHECK(Protocol::Finite(1.0f));
	CHECK(Protocol::Finite(1.0f, -2.0f, 0.0f));
	CHECK(Protocol::Finite(1.0e38f, -1.0e38f));  // large but numbers: the range is each message's to judge
	CHECK(!Protocol::Finite(INF));
	CHECK(!Protocol::Finite(-INF));
	CHECK(!Protocol::Finite(NOT_A_NUMBER));
	CHECK(!Protocol::Finite(1.0f, 2.0f, NOT_A_NUMBER));
	CHECK(!Protocol::Finite(INF, 2.0f, 3.0f));
	const std::array<float, 3> good{ 1.0f, 2.0f, 3.0f };
	const std::array<float, 3> bad{ 1.0f, INF, 3.0f };
	CHECK(Protocol::Finite(good));
	CHECK(!Protocol::Finite(bad));
	const float plain[3] = { 0.0f, NOT_A_NUMBER, 0.0f };
	CHECK(!Protocol::Finite(plain));
	CHECK(Protocol::Finite(std::vector<float>{}));  // nothing to be wrong
	CHECK(Protocol::FiniteOr(2.5f, 0.0f) == 2.5f);
	CHECK(Protocol::FiniteOr(NOT_A_NUMBER, 0.0f) == 0.0f);
	CHECK(Protocol::FiniteOr(-INF, 7.0f) == 7.0f);
}

TEST("wire floats: a health share has none for a maximum that is infinite, no number, 0 or less, or a health that is no number")
{
	// Friends' copies set to no health: an infinite maximum made every share 0.
	CHECK(!Protocol::HealthFraction(150.0f, INF));
	CHECK(!Protocol::HealthFraction(INF, INF));
	// A report their games refuse, sent again and again: no number for the maximum or the health.
	CHECK(!Protocol::HealthFraction(150.0f, NOT_A_NUMBER));
	CHECK(!Protocol::HealthFraction(NOT_A_NUMBER, 200.0f));
	CHECK(!Protocol::HealthFraction(INF, 200.0f));
	CHECK(!Protocol::HealthFraction(-INF, 200.0f));
	// No maximum to take a share of.
	CHECK(!Protocol::HealthFraction(50.0f, 0.0f));
	CHECK(!Protocol::HealthFraction(50.0f, -10.0f));
	CHECK(!Protocol::SharableMaximum(INF));
	CHECK(!Protocol::SharableMaximum(NOT_A_NUMBER));
	CHECK(!Protocol::SharableMaximum(0.0f));
	CHECK(Protocol::SharableMaximum(1.0f));
	// Sound numbers: the share, held to 0..1 (a buffed NPC above its maximum is whole; one past dead has none).
	const auto half = Protocol::HealthFraction(100.0f, 200.0f);
	REQUIRE(half);
	CHECK(*half == 0.5f);
	CHECK(Protocol::HealthFraction(300.0f, 200.0f) == std::optional{ 1.0f });
	CHECK(Protocol::HealthFraction(-20.0f, 200.0f) == std::optional{ 0.0f });
	// A share from sound numbers goes out with its maximum and a friend's game takes both.
	const auto packet = Protocol::Encode(Protocol::ActorHealth{ 0x0002A001, *half, 0, 0, 200.0f }, Protocol::MessageType::kReportHealth);
	const auto heard = Protocol::DecodeActorHealth(packet);
	REQUIRE(heard);
	CHECK(heard->health == 0.5f);
	CHECK(heard->maximum == 200.0f);
}

TEST("wire floats: a player's status health is full, not all but dead, when the maximum or the health is no finite number")
{
	// Friends saw them at 1%: an infinite maximum made the share 0.
	CHECK(DownRules::StatusHealth(false, false, 150.0f, INF) == 100);
	CHECK(DownRules::StatusHealth(false, false, 150.0f, NOT_A_NUMBER) == 100);
	CHECK(DownRules::StatusHealth(false, false, NOT_A_NUMBER, 200.0f) == 100);
	// Sound numbers are told as before: dead is 0, down is at least 1.
	CHECK(DownRules::StatusHealth(false, false, 100.0f, 200.0f) == 50);
	CHECK(DownRules::StatusHealth(true, false, 100.0f, 200.0f) == 0);
	CHECK(DownRules::StatusHealth(true, true, 0.0f, 200.0f) == 1);
	CHECK(DownRules::StatusHealth(false, false, 100.0f, 0.0f) == 0);
}

TEST("wire floats: one NPC's state that is no number would cost every NPC's in the list, so the sender leaves it out")
{
	Protocol::ActorState good;
	good.refId = 0x0002A001;
	good.worldspace = 0x0000003C;
	good.x = 100.0f;
	good.y = 200.0f;
	good.z = 10.0f;
	auto bad = good;
	bad.refId = 0x0002A002;
	bad.heading = NOT_A_NUMBER;
	CHECK(good.IsFinite());
	CHECK(!bad.IsFinite());
	// What the receiver does with a list that holds it: refuses all of it (the sender's check, NpcSync, keeps it out).
	const std::vector<Protocol::ActorState> both{ good, bad };
	const std::vector<Protocol::ActorState> sound{ good };
	CHECK(!Protocol::DecodeActorStates(Protocol::Encode(both, Protocol::MessageType::kActorStates)));
	const auto heard = Protocol::DecodeActorStates(Protocol::Encode(sound, Protocol::MessageType::kActorStates));
	REQUIRE(heard);
	CHECK(heard->size() == 1);
}

TEST("wire floats: a workshop piece's transform is sane only as a report may carry it, and a scrap with it cleared is taken")
{
	Protocol::WorkshopItem item;
	item.refId = 0x0001F00D;
	item.op = Protocol::WorkshopOp::kScrapped;
	item.base = 0x0001A2B3;
	item.workshop = 0x000250FE;
	item.position[0] = 1000.0f;
	item.rotation[2] = 1.5f;
	CHECK(Protocol::SaneTransform(item));
	auto bad = item;
	bad.position[1] = NOT_A_NUMBER;
	CHECK(!Protocol::SaneTransform(bad));
	bad = item;
	bad.rotation[0] = INF;
	CHECK(!Protocol::SaneTransform(bad));
	bad = item;
	bad.scale = NOT_A_NUMBER;
	CHECK(!Protocol::SaneTransform(bad));
	bad = item;
	bad.position[2] = 2.0e6f;  // outside any world
	CHECK(!Protocol::SaneTransform(bad));
	// A friend's game refuses the report with it, and takes the scrap with its transform cleared (WorkshopSync::Push).
	bad = item;
	bad.position[0] = INF;
	CHECK(!Protocol::DecodeWorkshopItem(Protocol::Encode(bad, Protocol::MessageType::kReportWorkshopItem)));
	auto cleared = bad;
	std::ranges::fill(cleared.position, 0.0f);
	std::ranges::fill(cleared.rotation, 0.0f);
	cleared.scale = 1.0f;
	CHECK(Protocol::SaneTransform(cleared));
	const auto heard = Protocol::DecodeWorkshopItem(Protocol::Encode(cleared, Protocol::MessageType::kReportWorkshopItem));
	REQUIRE(heard);
	CHECK(heard->op == Protocol::WorkshopOp::kScrapped);
	CHECK(heard->refId == item.refId);
}

TEST("wire floats: a workshop request with a place or angle that is no number is one the server refuses, so it is not sent")
{
	Protocol::WorkshopRequest request;
	request.seq = 1;
	request.op = Protocol::WorkshopRequestOp::kBuild;
	request.workshop = 0x000250FE;
	request.base = 0x0001A2B3;
	request.position = { 100.0f, 200.0f, 10.0f };
	CHECK(Protocol::IsValid(request));
	auto bad = request;
	bad.position[0] = NOT_A_NUMBER;
	CHECK(!Protocol::IsValid(bad));
	bad = request;
	bad.rotation[1] = INF;
	CHECK(!Protocol::IsValid(bad));
	bad = request;
	bad.scale = INF;
	CHECK(!Protocol::IsValid(bad));
}
