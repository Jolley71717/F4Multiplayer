#include "Test.h"

#include "SessionFile.h"

#include <cstdio>
#include <filesystem>

// The host's saved session: everything the server remembers, through the file and back.
namespace
{
	SessionFile::Snapshot MakeSnapshot()
	{
		SessionFile::Snapshot s;
		s.contentHash = 0xF672F384;
		s.sessionId = 0x0123456789ABCDEFull;
		s.nextId = 4;
		s.identities = { { 0xA11CEull, 1 }, { 0xB0Bull, 3 } };
		s.deadActors = { 0x0004B2BA, 0x0001D41F };
		Protocol::IndexedContainerChange change;
		change.index = 0;
		change.playerId = 1;
		change.change.container = 0x0007BD1B;
		change.change.item = 0x0001F276;
		change.change.count = -5;
		s.containerChanges.push_back(change);
		s.pickedUp = { 0x000250FF };
		Protocol::RefState state;
		state.refId = 0x0001A2B3;
		state.open = true;
		s.refStates.push_back(state);
		Protocol::QuestStage stage;
		stage.quest = 0x0001D5E0;
		stage.stage = 20;
		s.questStages.push_back(stage);
		Protocol::WorldTime time;
		time.gameHour = 13.5f;
		time.daysPassed = 7.0f;
		s.worldTime = time;
		s.markers = { { 0x0001F228, 3 }, { 0x00017E00, 1 } };
		Protocol::WorkshopItem item;
		item.playerId = 2;
		item.refId = 0xFF000F3A;
		item.base = 0x000DF45C;
		item.workshop = 0x000250FE;
		item.position[0] = -80358.0f;
		s.workshopItems.push_back(item);
		return s;
	}

	std::filesystem::path TempFile()
	{
		return std::filesystem::temp_directory_path() / std::format("f4mp-session-{}.bin", std::rand());
	}
}

TEST("session file: a snapshot round trips through the bytes")
{
	const auto in = MakeSnapshot();
	const auto out = SessionFile::Decode(SessionFile::Encode(in));
	REQUIRE(out);
	CHECK(out->contentHash == in.contentHash);
	CHECK(out->sessionId == in.sessionId);
	CHECK(out->nextId == 4);
	CHECK(out->identities == in.identities);
	CHECK(out->deadActors == in.deadActors);
	REQUIRE(out->containerChanges.size() == 1);
	CHECK(out->containerChanges[0].index == 0);
	CHECK(out->containerChanges[0].change.item == 0x0001F276);
	CHECK(out->pickedUp == in.pickedUp);
	REQUIRE(out->refStates.size() == 1);
	CHECK(out->refStates[0].refId == 0x0001A2B3);
	REQUIRE(out->questStages.size() == 1);
	CHECK(out->questStages[0].stage == 20);
	REQUIRE(out->worldTime);
	CHECK(out->worldTime->daysPassed == 7.0f);
	CHECK(out->markers == in.markers);
	REQUIRE(out->workshopItems.size() == 1);
	CHECK(out->workshopItems[0] == in.workshopItems[0]);
}

TEST("session file: an empty snapshot and a big one both round trip")
{
	SessionFile::Snapshot empty;
	empty.contentHash = 1;
	const auto back = SessionFile::Decode(SessionFile::Encode(empty));
	REQUIRE(back);
	CHECK(back->Empty());

	SessionFile::Snapshot big;
	big.contentHash = 2;
	for (std::uint32_t i = 1; i <= 9000; ++i) {
		big.deadActors.push_back(i);  // more than two world-state chunks
	}
	for (std::uint32_t i = 0; i < 150; ++i) {
		Protocol::WorkshopItem item;
		item.refId = 0xFF000000 + i;
		item.base = 0x000DF45C;
		big.workshopItems.push_back(item);  // more than two batches
	}
	const auto bigBack = SessionFile::Decode(SessionFile::Encode(big));
	REQUIRE(bigBack);
	CHECK(bigBack->deadActors.size() == 9000);
	CHECK(bigBack->workshopItems.size() == 150);
}

TEST("session file: a truncated, foreign or old-version file is refused whole")
{
	const auto bytes = SessionFile::Encode(MakeSnapshot());
	for (const std::size_t cut : { std::size_t{ 3 }, std::size_t{ 17 }, bytes.size() - 1 }) {
		CHECK(!SessionFile::Decode(std::span(bytes).first(cut)));
	}
	auto wrongMagic = bytes;
	wrongMagic[0] ^= 0xFF;
	CHECK(!SessionFile::Decode(wrongMagic));
	auto oldVersion = bytes;
	oldVersion[4] ^= 0xFF;
	CHECK(!SessionFile::Decode(oldVersion));
}

TEST("session file: save then load through a file; a missing file is nothing")
{
	const auto path = TempFile();
	CHECK(!SessionFile::Load(path));
	REQUIRE(SessionFile::Save(path, MakeSnapshot()));
	const auto back = SessionFile::Load(path);
	REQUIRE(back);
	CHECK(back->deadActors.size() == 2);
	CHECK(!std::filesystem::exists(path.string() + ".tmp"));
	std::filesystem::remove(path);
}
