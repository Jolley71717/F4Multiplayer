#include "Test.h"

#include "SessionFile.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

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
		return std::filesystem::temp_directory_path() / std::format("f4mp-session-{:016X}.bin", (std::uint64_t{ std::random_device{}() } << 32) | std::random_device{}());
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

TEST("session file: a truncated, foreign or other-layout file is refused whole")
{
	const auto bytes = SessionFile::Encode(MakeSnapshot());
	for (const std::size_t cut : { std::size_t{ 3 }, std::size_t{ 17 }, bytes.size() - 1 }) {
		CHECK(!SessionFile::Decode(std::span(bytes).first(cut)));
	}
	auto wrongMagic = bytes;
	wrongMagic[0] ^= 0xFF;
	CHECK(!SessionFile::Decode(wrongMagic));
	// Before the first file (protocol 22), a layout to come (28), and nonsense.
	for (const std::uint16_t version : { std::uint16_t{ 22 }, std::uint16_t{ 28 }, std::uint16_t{ 0xE526 } }) {
		auto other = bytes;
		std::memcpy(other.data() + 4, &version, sizeof(version));
		CHECK(!SessionFile::Decode(other));
	}
}

TEST("session file: the header carries the file's own version, not the protocol's")
{
	const auto bytes = SessionFile::Encode(MakeSnapshot());
	std::uint16_t version = 0;
	std::memcpy(&version, bytes.data() + 4, sizeof(version));
	CHECK(version == SessionFile::FORMAT);
	CHECK(SessionFile::Readable(SessionFile::FORMAT));
	// The protocol versions that wrote this same layout before the file had its own version.
	for (std::uint16_t written = 23; written <= 27; ++written) {
		auto old = bytes;
		std::memcpy(old.data() + 4, &written, sizeof(written));
		CHECK(SessionFile::Decode(old));
	}
}

TEST("session file: a file 0.8.5 wrote (protocol 26 in its header) still loads")
{
	// Written by SessionFile::Encode as it was at v0.8.5.
	const std::vector<std::uint8_t> bytes = {
		0x46, 0x4D, 0x50, 0x53, 0x1A, 0x00, 0x84, 0xF3, 0x72, 0xF6, 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45,
		0x23, 0x01, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0xCE, 0x11, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x3D, 0x00, 0x00, 0x00, 0x6B, 0x01, 0x00, 0x00, 0x00, 0xBA, 0xB2, 0x04,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1B, 0xBD, 0x07, 0x00, 0x76, 0xF2, 0x01,
		0x00, 0xFB, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF, 0x50, 0x02,
		0x00, 0x01, 0x00, 0x00, 0x00, 0xB3, 0xA2, 0x01, 0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0xE0,
		0xD5, 0x01, 0x00, 0x14, 0x00, 0x15, 0x00, 0x00, 0x00, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x58, 0x41, 0x00, 0x00, 0xE0, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x00,
		0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x28, 0xF2, 0x01, 0x00, 0x03, 0x2F, 0x00,
		0x00, 0x00, 0x83, 0x01, 0x02, 0x00, 0x00, 0x00, 0x3A, 0x0F, 0x00, 0xFF, 0x00, 0x5C, 0xF4, 0x0D,
		0x00, 0xFE, 0x50, 0x02, 0x00, 0x00, 0xF3, 0x9C, 0xC7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
		0x3F
	};
	const auto out = SessionFile::Decode(bytes);
	REQUIRE(out);
	CHECK(out->contentHash == 0xF672F384);
	CHECK(out->sessionId == 0x0123456789ABCDEFull);
	CHECK(out->nextId == 4);
	REQUIRE(out->identities.size() == 1);
	CHECK(out->identities[0].first == 0xA11CEull);
	CHECK(out->deadActors == std::vector<std::uint32_t>{ 0x0004B2BA });
	REQUIRE(out->containerChanges.size() == 1);
	CHECK(out->containerChanges[0].change.container == 0x0007BD1B);
	CHECK(out->containerChanges[0].change.count == -5);
	CHECK(out->pickedUp == std::vector<std::uint32_t>{ 0x000250FF });
	REQUIRE(out->refStates.size() == 1);
	CHECK(out->refStates[0].refId == 0x0001A2B3);
	REQUIRE(out->questStages.size() == 1);
	CHECK(out->questStages[0].stage == 20);
	REQUIRE(out->worldTime);
	CHECK(out->worldTime->gameHour == 13.5f);
	REQUIRE(out->markers.size() == 1);
	CHECK(out->markers[0].first == 0x0001F228);
	REQUIRE(out->workshopItems.size() == 1);
	CHECK(out->workshopItems[0].refId == 0xFF000F3A);
	CHECK(out->workshopItems[0].position[0] == -80358.0f);
	// Written again, it is the same file: 0.8.5 can still read what this version writes.
	CHECK(SessionFile::Encode(*out) == bytes);
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

TEST("session file: saving keeps the version before as a backup, and a damaged file is set aside for it")
{
	const auto path = TempFile();
	std::vector<std::string> log;
	const auto               say = [&](const std::string& a_line) { log.push_back(a_line); };
	auto       first = MakeSnapshot();
	auto       second = MakeSnapshot();
	second.deadActors.push_back(0x0001D420);
	REQUIRE(SessionFile::Save(path, first));
	REQUIRE(SessionFile::Save(path, second));
	CHECK(SessionFile::Load(SessionFile::BackupOf(path))->deadActors == first.deadActors);
	CHECK(SessionFile::Recover(path, say)->deadActors == second.deadActors);
	CHECK(log.empty());

	{
		std::ofstream damaged{ path, std::ios::binary | std::ios::trunc };
		damaged << "FMPS and then nothing";
	}
	const auto recovered = SessionFile::Recover(path, say);
	REQUIRE(recovered);
	CHECK(recovered->deadActors == first.deadActors);
	CHECK(!std::filesystem::exists(path));
	CHECK(std::filesystem::file_size(SessionFile::SetAsideOf(path)) == std::string_view{ "FMPS and then nothing" }.size());
	CHECK(log.size() == 2);

	// The next save doesn't turn the damaged file into the backup: the good one stays.
	REQUIRE(SessionFile::Save(path, second));
	CHECK(SessionFile::Load(SessionFile::BackupOf(path))->deadActors == first.deadActors);
	for (const auto& file : { path, SessionFile::BackupOf(path), SessionFile::SetAsideOf(path) }) {
		std::filesystem::remove(file);
	}
}
