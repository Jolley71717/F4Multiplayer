#include "Test.h"

#include "SessionFile.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
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
		return Test::TempPath("f4mp-session", ".bin");
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
	// Before the first file (protocol 22), the layouts of the destructibles' and shared storage's own test
	// builds (35, 37), a layout to come (40), and nonsense.
	for (const std::uint16_t version : { std::uint16_t{ 22 }, std::uint16_t{ 35 }, std::uint16_t{ 37 }, std::uint16_t{ 40 }, std::uint16_t{ 0xE526 } }) {
		auto other = bytes;
		std::memcpy(other.data() + 4, &version, sizeof(version));
		CHECK(!SessionFile::Decode(other));
	}
}

// The host's workshop builds come back from the file. A workshop record damaged down to its type byte
// is damage like any other: the file is refused whole, not loaded with that record skipped as an empty batch.
TEST("session file: a workshop record cut to its type byte is refused like any other damage")
{
	auto bytes = SessionFile::Encode(MakeSnapshot());
	REQUIRE(SessionFile::Decode(bytes));
	const std::uint32_t length = 1;
	const auto          at = bytes.size();
	bytes.resize(at + sizeof(length));
	std::memcpy(bytes.data() + at, &length, sizeof(length));
	bytes.push_back(static_cast<std::uint8_t>(Protocol::MessageType::kWorkshopItems));
	CHECK(!SessionFile::Decode(bytes));
	CHECK(!Protocol::DecodeWorkshopItems(std::span(bytes).last(1)));
}

TEST("session file: the header carries the file's own version, not the protocol's")
{
	const auto bytes = SessionFile::Encode(MakeSnapshot());
	std::uint16_t version = 0;
	std::memcpy(&version, bytes.data() + 4, sizeof(version));
	CHECK(version == SessionFile::OBJECT_PLACES);  // no shared storage: the layout an older build reads too
	auto withOwners = MakeSnapshot();
	withOwners.storageOwners = { { 0x00020000, 1 } };
	const auto ownerBytes = SessionFile::Encode(withOwners);
	std::memcpy(&version, ownerBytes.data() + 4, sizeof(version));
	CHECK(version == SessionFile::STORAGE);
	CHECK(SessionFile::Readable(SessionFile::FORMAT));
	// The layout before 29, under the protocol versions that wrote it before the file had its own version (28: the
	// shared-quest test builds, before rc3 gave the file its own, 26): the same, less the list of deaths that may be
	// mistakes, which are all of them there. (Without workshop items: those had no wire byte then, see below.)
	auto withoutItems = MakeSnapshot();
	withoutItems.workshopItems.clear();
	auto legacy = SessionFile::Encode(withoutItems);
	const std::size_t at = 4 + 2 + 4 + 8 + 4 + 2 + MakeSnapshot().identities.size() * 12;
	legacy.erase(legacy.begin() + at, legacy.begin() + at + 4);
	for (std::uint16_t written = 23; written <= 28; ++written) {
		auto old = legacy;
		std::memcpy(old.data() + 4, &written, sizeof(written));
		const auto out = SessionFile::Decode(old);
		REQUIRE(out);
		CHECK(out->deadActors == MakeSnapshot().deadActors);
		CHECK(out->healable == out->deadActors);
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
	// Its deaths may be mistakes (that build took an essential NPC going down for dead), and stay so when it is written
	// again, in the new layout.
	CHECK(out->healable == out->deadActors);
	const auto again = SessionFile::Decode(SessionFile::Encode(*out));
	REQUIRE(again);
	CHECK(again->healable == out->deadActors);
	CHECK(again->deadActors == out->deadActors);
	CHECK(again->workshopItems.size() == 1);
}

// Layout 29, as the releases before wires wrote it: each workshop item without the byte that says whether a wire's shape follows.
// Made here from the new layout by taking that byte out of each item (the snapshot's one item is the file's last packet).
TEST("session file: a layout 29 file loads with its workshop items, and is written again with their wire byte")
{
	auto in = MakeSnapshot();
	auto bytes = SessionFile::Encode(in);
	REQUIRE(bytes.back() == 0);  // the one item's wire byte: none
	bytes.pop_back();
	// The last packet's length, before its type byte, the item count and one item of 45 bytes.
	constexpr std::size_t PACKET = 1 + 1 + 45;
	const std::size_t     lengthAt = bytes.size() - PACKET - sizeof(std::uint32_t);
	std::uint32_t         length = 0;
	std::memcpy(&length, bytes.data() + lengthAt, sizeof(length));
	REQUIRE(length == PACKET + 1);
	length = PACKET;
	std::memcpy(bytes.data() + lengthAt, &length, sizeof(length));
	CHECK(!SessionFile::Decode(bytes));  // under the new layout's number it is damage
	const std::uint16_t layout = SessionFile::BEFORE_WIRES;
	std::memcpy(bytes.data() + 4, &layout, sizeof(layout));
	const auto out = SessionFile::Decode(bytes);
	REQUIRE(out);
	REQUIRE(out->workshopItems.size() == 1);
	CHECK(out->workshopItems[0] == in.workshopItems[0]);
	CHECK(!out->workshopItems[0].wire);
	CHECK(out->deadActors == in.deadActors);
	CHECK(out->healable.empty());  // a 29 file's deaths are its own kind (see Snapshot::healable)
	const auto again = SessionFile::Decode(SessionFile::Encode(*out));
	REQUIRE(again);
	CHECK(again->workshopItems == out->workshopItems);
}

TEST("session file: a wire's shape is kept through the file")
{
	auto in = MakeSnapshot();
	auto wire = in.workshopItems[0];
	wire.refId = 0xFF000FCA;
	wire.base = 0x0001D971;
	wire.wire = Protocol::WireShape{ { -62.7f, 24.0f, 37.8f }, 0.05f, 1.5f };
	in.workshopItems.push_back(wire);
	const auto out = SessionFile::Decode(SessionFile::Encode(in));
	REQUIRE(out);
	CHECK(out->workshopItems == in.workshopItems);
}

TEST("session file: the deaths an older build saved stay marked through the new layout, and a death since is never one")
{
	auto in = MakeSnapshot();
	in.healable = { 0x0001D41F };
	const auto out = SessionFile::Decode(SessionFile::Encode(in));
	REQUIRE(out);
	CHECK(out->healable == in.healable);
	CHECK(out->deadActors == in.deadActors);

	in.healable.clear();
	const auto none = SessionFile::Decode(SessionFile::Encode(in));
	REQUIRE(none);
	CHECK(none->healable.empty());

	// A list longer than what follows it is damage: the file is refused whole.
	auto bytes = SessionFile::Encode(in);
	const std::size_t   at = 4 + 2 + 4 + 8 + 4 + 2 + in.identities.size() * 12;
	const std::uint32_t lots = 0x40000000;
	std::memcpy(bytes.data() + at, &lots, sizeof(lots));
	CHECK(!SessionFile::Decode(bytes));
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

namespace
{
	// The session as the host left it after sitting a_sitting: a different NPC dead each time, so a file tells
	// which sitting it holds.
	SessionFile::Snapshot Sitting(std::uint32_t a_sitting)
	{
		auto snapshot = MakeSnapshot();
		snapshot.deadActors = { 0x0001F000 + a_sitting };
		return snapshot;
	}

	std::uint32_t SittingIn(const std::optional<SessionFile::Snapshot>& a_saved)
	{
		return a_saved && a_saved->deadActors.size() == 1 ? a_saved->deadActors[0] - 0x0001F000 : 0;
	}

	// The session file and what saving leaves beside it, removed at the end.
	struct Files
	{
		std::filesystem::path path = TempFile();
		std::filesystem::path temp = path.string() + ".tmp";
		std::filesystem::path backup = SessionFile::BackupOf(path);
		std::filesystem::path aside = SessionFile::SetAsideOf(path);
		~Files()
		{
			std::error_code error;
			for (const auto& file : { path, temp, backup, aside }) {
				std::filesystem::remove(file, error);
			}
		}

		// Cuts the session file short, as a full disk or a sync tool can.
		void CutShort() const
		{
			const auto size = std::filesystem::file_size(path);
			std::filesystem::resize_file(path, size / 2);
		}
	};
}

TEST("session file: each save keeps the one before it as the backup, in place of the older backup")
{
	Files files;
	for (std::uint32_t sitting = 1; sitting <= 3; ++sitting) {
		REQUIRE(SessionFile::Save(files.path, Sitting(sitting)));
	}
	CHECK(SittingIn(SessionFile::Load(files.path)) == 3);
	CHECK(SittingIn(SessionFile::Load(files.backup)) == 2);
	CHECK(!std::filesystem::exists(files.temp));
}

TEST("session file: a session file cut short on disk never pushes the good backup out, and the host carries on from that backup")
{
	Files                    files;
	std::vector<std::string> log;
	const auto               say = [&](const std::string& a_line) { log.push_back(a_line); };
	REQUIRE(SessionFile::Save(files.path, Sitting(1)));
	REQUIRE(SessionFile::Save(files.path, Sitting(2)));

	// Cut short while the host plays on: the next save replaces it, but doesn't make it the backup.
	files.CutShort();
	REQUIRE(SessionFile::Save(files.path, Sitting(3)));
	CHECK(SittingIn(SessionFile::Load(files.path)) == 3);
	CHECK(SittingIn(SessionFile::Load(files.backup)) == 1);

	// Cut short after the host quit: the next start carries on from the backup, and keeps the cut file aside.
	files.CutShort();
	CHECK(SittingIn(SessionFile::Recover(files.path, say)) == 1);
	CHECK(std::filesystem::exists(files.aside));
	REQUIRE(SessionFile::Save(files.path, Sitting(4)));
	CHECK(SittingIn(SessionFile::Load(files.backup)) == 1);
	CHECK(log.size() == 2);
}

TEST("session file: a save that can't be written leaves the session and its backup as they were")
{
	Files files;
	REQUIRE(SessionFile::Save(files.path, Sitting(1)));
	REQUIRE(SessionFile::Save(files.path, Sitting(2)));
	std::filesystem::create_directory(files.temp);  // the temporary file can't be made
	CHECK(!SessionFile::Save(files.path, Sitting(3)));
	CHECK(SittingIn(SessionFile::Load(files.path)) == 2);
	CHECK(SittingIn(SessionFile::Load(files.backup)) == 1);
	std::filesystem::remove(files.temp);
	CHECK(SessionFile::Save(files.path, Sitting(3)));  // tried again later, it goes through
	CHECK(SittingIn(SessionFile::Load(files.path)) == 3);
	CHECK(SittingIn(SessionFile::Load(files.backup)) == 2);
}

TEST("session file: a damaged file that another program holds open is read past, and the save after keeps the good backup")
{
	Files                    files;
	std::vector<std::string> log;
	const auto               say = [&](const std::string& a_line) { log.push_back(a_line); };
	REQUIRE(SessionFile::Save(files.path, Sitting(1)));
	REQUIRE(SessionFile::Save(files.path, Sitting(2)));
	{
		// A sync tool writing junk over it, and holding it: it can be read, but not renamed aside.
		std::ofstream held{ files.path, std::ios::binary | std::ios::trunc };
		held << "FMPS and then nothing";
		held.flush();
		CHECK(SittingIn(SessionFile::Recover(files.path, say)) == 1);
		REQUIRE(!log.empty());
#ifdef _WIN32
		CHECK(!std::filesystem::exists(files.aside));
		CHECK(log[0].find("nor renamed") != std::string::npos);
#else
		// Linux lets a file another program holds open be renamed: it is set aside like any damaged file.
		CHECK(std::filesystem::exists(files.aside));
		CHECK(log[0].find("kept as") != std::string::npos);
#endif
	}
	REQUIRE(SessionFile::Save(files.path, Sitting(3)));
	CHECK(SittingIn(SessionFile::Load(files.path)) == 3);
	CHECK(SittingIn(SessionFile::Load(files.backup)) == 1);
}

TEST("session file: the session's plugin table is kept, so the file's form IDs are known whatever the host loads next")
{
	auto in = MakeSnapshot();
	in.plugins = Protocol::ComparedLoadOrder({ "Fallout4.esm", "DLCCoast.esm", "Mod.esp" }, { "Light.esl" });
	const auto bytes = SessionFile::Encode(in);
	const auto out = SessionFile::Decode(bytes);
	REQUIRE(out);
	CHECK(out->plugins == in.plugins);
	CHECK(out->deadActors == in.deadActors);
	CHECK(out->workshopItems.size() == 1);

	// The layout before it (29) had none, and its file still loads, with none: its IDs are its hash's load order's.
	// (Without workshop items: 29 had no wire byte on them.)
	auto withoutItems = MakeSnapshot();
	withoutItems.workshopItems.clear();
	auto old = SessionFile::Encode(withoutItems);
	const std::uint16_t before = SessionFile::BEFORE_PLUGIN_TABLE;
	std::memcpy(old.data() + 4, &before, sizeof(before));
	const auto oldOut = SessionFile::Decode(old);
	REQUIRE(oldOut);
	CHECK(oldOut->plugins.empty());
	CHECK(oldOut->deadActors == MakeSnapshot().deadActors);

	// A table in a file of that layout, or two in one, is damage.
	auto tableIn29 = bytes;
	std::memcpy(tableIn29.data() + 4, &before, sizeof(before));
	CHECK(!SessionFile::Decode(tableIn29));
	const auto table = Protocol::Encode(Protocol::PluginTable{ in.plugins });
	auto twice = bytes;
	const std::uint32_t size = static_cast<std::uint32_t>(table.size());
	const auto* sizeBytes = reinterpret_cast<const std::uint8_t*>(&size);
	twice.insert(twice.end(), sizeBytes, sizeBytes + sizeof(size));
	twice.insert(twice.end(), table.begin(), table.end());
	CHECK(!SessionFile::Decode(twice));
}

// Drops (30), wires (31) and the plugin table (32) were each added to 29 on a branch of their own, under its own number,
// and this layout (33) has all three: a file of each of those still loads, and what its layout lacks is damage in it.
TEST("session file: the layouts of drops, wires and the plugin table each load, and hold only their own")
{
	Protocol::DroppedItem drop{ .dropId = 1, .ownerId = 2, .nonce = 7, .base = 0x0001F276, .count = 120, .templateIndex = 0xFFFF, .cell = 0x0001A2B3 };
	const auto as = [](std::vector<std::uint8_t> a_bytes, std::uint16_t a_layout) {
		std::memcpy(a_bytes.data() + 4, &a_layout, sizeof(a_layout));
		return SessionFile::Decode(a_bytes);
	};

	auto bare = MakeSnapshot();
	bare.workshopItems.clear();
	for (std::uint16_t layout = 29; layout <= SessionFile::FORMAT; ++layout) {
		if (layout == SessionFile::DESTRUCTIBLES_TEST || layout == SessionFile::STORAGE_TEST) {
			continue;  // no build reads it
		}
		const auto out = as(SessionFile::Encode(bare), layout);
		REQUIRE(out);
		CHECK(out->deadActors == bare.deadActors);
		CHECK(out->drops.empty());
		CHECK(out->plugins.empty());
	}

	auto withDrop = bare;
	withDrop.drops = { drop };
	REQUIRE(as(SessionFile::Encode(withDrop), SessionFile::STORAGE));
	const auto drops = as(SessionFile::Encode(withDrop), SessionFile::DROPS_ONLY);
	REQUIRE(drops);
	CHECK(drops->drops == withDrop.drops);
	for (const std::uint16_t layout : { SessionFile::BEFORE_WIRES, SessionFile::WIRES_ONLY, SessionFile::PLUGIN_TABLE_ONLY }) {
		CHECK(!as(SessionFile::Encode(withDrop), layout));
	}

	// This layout's items carry the wire byte: they read under 31 only, of the others.
	const auto items = as(SessionFile::Encode(MakeSnapshot()), SessionFile::WIRES_ONLY);
	REQUIRE(items);
	CHECK(items->workshopItems == MakeSnapshot().workshopItems);
	for (const std::uint16_t layout : { SessionFile::BEFORE_WIRES, SessionFile::DROPS_ONLY, SessionFile::PLUGIN_TABLE_ONLY }) {
		CHECK(!as(SessionFile::Encode(MakeSnapshot()), layout));
	}

	auto withTable = bare;
	withTable.plugins = Protocol::ComparedLoadOrder({ "Fallout4.esm", "Mod.esp" }, {});
	const auto table = as(SessionFile::Encode(withTable), SessionFile::PLUGIN_TABLE_ONLY);
	REQUIRE(table);
	CHECK(table->plugins == withTable.plugins);
	for (const std::uint16_t layout : { SessionFile::BEFORE_PLUGIN_TABLE, SessionFile::DROPS_ONLY, SessionFile::WIRES_ONLY }) {
		CHECK(!as(SessionFile::Encode(withTable), layout));
	}
}

// Layout 34 adds the destructible objects damaged in the session to 33 (with the spent traps): they come back whole,
// never to go off again, and in a file of an older layout they are damage.
TEST("session file: destructible objects come back from layout 34 as wrecks alone")
{
	const auto as = [](std::vector<std::uint8_t> a_bytes, std::uint16_t a_layout) {
		std::memcpy(a_bytes.data() + 4, &a_layout, sizeof(a_layout));
		return SessionFile::Decode(a_bytes);
	};
	auto in = MakeSnapshot();
	in.destructions = { { 0x0001B8AA, 249.5f, true, false }, { 0x000A1B2C, 380.0f, false, false } };
	for (std::uint32_t i = 0; i < 300; ++i) {
		in.destructions.push_back({ 0x00100000 + i, 0.0f, true, false });  // more than one packet holds
	}
	const auto out = SessionFile::Decode(SessionFile::Encode(in));
	REQUIRE(out);
	CHECK(out->destructions == in.destructions);
	CHECK(!out->Empty());

	// One saved as it went off (a build's mistake, or a hand-edited file) is the wreck alone all the same.
	auto live = MakeSnapshot();
	live.destructions = { { 0x0001B8AA, 100.0f, true, true } };
	const auto calm = SessionFile::Decode(SessionFile::Encode(live));
	REQUIRE(calm);
	REQUIRE(calm->destructions.size() == 1);
	CHECK(!calm->destructions[0].explode);

	CHECK(!as(SessionFile::Encode(in), SessionFile::ALL_THREE));
	auto none = MakeSnapshot();
	const auto older = as(SessionFile::Encode(none), SessionFile::ALL_THREE);
	REQUIRE(older);
	CHECK(older->destructions.empty());
	SessionFile::Snapshot only;
	only.destructions = { { 0x0001B8AA, 0.0f, true, false } };
	CHECK(!only.Empty());
	// A file of layout 34 still loads (without moved objects, which it never had).
	const auto layout34 = as(SessionFile::Encode(in), SessionFile::TRAPS_AND_DESTRUCTIONS);
	REQUIRE(layout34);
	CHECK(layout34->destructions == in.destructions);
}

// Layout 36 adds where the loose objects players moved came to rest: a friend who loads the cell after the session
// restarts still finds them there. In a file of an older layout, or as a live move, one is damage.
TEST("session file: moved objects' places come back from layout 36, in the order they were moved")
{
	const auto as = [](std::vector<std::uint8_t> a_bytes, std::uint16_t a_layout) {
		std::memcpy(a_bytes.data() + 4, &a_layout, sizeof(a_layout));
		return SessionFile::Decode(a_bytes);
	};
	const auto place = [](std::uint32_t a_ref, float a_x) {
		return Protocol::ObjectRest{ .refId = a_ref, .kind = Protocol::ObjectRestKind::kRest, .worldspace = 0x0000003C, .position = { a_x, -40.5f, 6.0f }, .rotation = { 0.0f, 1.5f, -2.0f } };
	};
	auto in = MakeSnapshot();
	in.objectPlaces = { place(0x0001A002, 120.0f), place(0x0001A001, -300.0f) };
	const auto out = SessionFile::Decode(SessionFile::Encode(in));
	REQUIRE(out);
	CHECK(out->objectPlaces == in.objectPlaces);
	CHECK(out->deadActors == in.deadActors);
	SessionFile::Snapshot only;
	only.objectPlaces = { place(0x0001A001, 0.0f) };
	CHECK(!only.Empty());

	CHECK(!as(SessionFile::Encode(in), SessionFile::TRAPS_AND_DESTRUCTIONS));
	auto named = in;
	named.objectPlaces[0].playerId = 2;
	CHECK(!SessionFile::Decode(SessionFile::Encode(named)));
	auto letGo = in;
	letGo.objectPlaces[0].kind = Protocol::ObjectRestKind::kLetGo;
	CHECK(!SessionFile::Decode(SessionFile::Encode(letGo)));
}

// Layout 38 adds whose game keeps each shared storage container (after a restart a friend's copy stays closed until that
// game lists it again, and nobody else's claim takes it) and the friends' moves its true copy undid, so none is undone
// twice. In a file of an older layout, they are damage; 37 (shared storage's test builds) isn't read at all.
TEST("session file: storage owners and undone moves come back from layout 38, and a layout 36 file still loads without them")
{
	const auto as = [](std::vector<std::uint8_t> a_bytes, std::uint16_t a_layout) {
		std::memcpy(a_bytes.data() + 4, &a_layout, sizeof(a_layout));
		return SessionFile::Decode(a_bytes);
	};
	auto in = MakeSnapshot();
	for (std::uint32_t i = 0; i < Protocol::MAX_STORAGE_OWNERS_PER_PACKET + 5; ++i) {
		in.storageOwners.push_back({ 0x00020000 + i, 1 + i % 2, i });  // more than one packet holds
	}
	for (std::uint32_t i = 0; i < Protocol::MAX_STORAGE_REVERSALS_PER_PACKET + 3; ++i) {
		in.storageReversals.push_back({ i, 2, i % 2 ? -3 : 4 });
	}
	const auto out = SessionFile::Decode(SessionFile::Encode(in));
	REQUIRE(out);
	CHECK(out->storageOwners == in.storageOwners);
	CHECK(out->storageReversals == in.storageReversals);
	CHECK(!SessionFile::Readable(SessionFile::STORAGE_TEST));
	CHECK(!as(SessionFile::Encode(in), SessionFile::STORAGE_TEST));
	SessionFile::Snapshot only;
	only.storageOwners = { { 0x00020000, 1 } };
	CHECK(!only.Empty());
	SessionFile::Snapshot undone;
	undone.storageReversals = { { 7, 2, -3 } };
	CHECK(!undone.Empty());
	const auto undoneOut = SessionFile::Decode(SessionFile::Encode(undone));
	REQUIRE(undoneOut);
	CHECK(undoneOut->storageReversals == undone.storageReversals);

	CHECK(!as(SessionFile::Encode(in), SessionFile::OBJECT_PLACES));
	auto none = MakeSnapshot();
	const auto older = as(SessionFile::Encode(none), SessionFile::OBJECT_PLACES);
	REQUIRE(older);
	CHECK(older->storageOwners.empty());
	auto nobody = in;
	nobody.storageOwners[0].ownerId = 0;
	CHECK(!SessionFile::Decode(SessionFile::Encode(nobody)));
	auto unnamed = in;
	unnamed.storageReversals[0].playerId = 0;
	CHECK(!SessionFile::Decode(SessionFile::Encode(unnamed)));
}
