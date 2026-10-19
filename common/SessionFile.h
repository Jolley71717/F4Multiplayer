#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The session's shared world, saved on the host's disk so it survives the host quitting: the next
// time the host plays, the session carries on (dead NPCs stay dead, loot stays taken, built walls
// stand), with their mods in any order (the file keeps the plugin table its form IDs are numbered by). The file is a header followed by ordinary protocol packets
// (the same ones a joiner gets), each length-prefixed, so nothing here has its own wire format.
namespace SessionFile
{
	inline constexpr std::uint32_t MAGIC = 0x53504D46;  // "FMPS"

	// The file's own layout version, apart from the protocol's: a protocol change to a message the file
	// doesn't hold (protocol 27 changed only the shot, 28 only the live quest stage, which now says who
	// did it; the file's stages don't) must not throw away every host's saved session. Raise it when
	// what Encode writes changes. Until the file had its own, its header carried the protocol version;
	// the layout did not change from the file's first (protocol 23) until 29, which added the deaths an
	// older build saved (Snapshot::healable) after the identities. Three changes each added to 29 on their own,
	// under a number of their own (their test builds wrote it):
	// - 30, the items players dropped for each other (Snapshot::drops, as kDropList packets);
	// - 31, a byte on each workshop item for a power wire's shape (WorkshopItem::wire);
	// - 32, the session's plugin table (Snapshot::plugins, a kPluginTable packet first among the packets), which
	//   numbers the file's form IDs.
	// 33 has all three. A file of any layout from 29 on reads as one without what its layout lacks (no drops, items
	// without wires, no plugin table: its IDs are the load order its hash names); what its layout lacks is damage.
	inline constexpr std::uint16_t FORMAT = 33;
	inline constexpr std::uint16_t BEFORE_WIRES = 29;         // the releases' layout: before drops, wires and the plugin table
	inline constexpr std::uint16_t BEFORE_PLUGIN_TABLE = 29;
	inline constexpr std::uint16_t DROPS_ONLY = 30;
	inline constexpr std::uint16_t WIRES_ONLY = 31;
	inline constexpr std::uint16_t PLUGIN_TABLE_ONLY = 32;

	// The header values of the layout before 29: the protocol versions it was written under before it
	// had its own, 23 to 28 (test builds of protocol 27 wrote 27, and the shared-quest test builds of
	// protocol 28 wrote 28), and 26 once it had. The next layout takes 34.
	[[nodiscard]] constexpr bool Legacy(std::uint16_t a_format)
	{
		return a_format >= 23 && a_format <= 28;
	}

	[[nodiscard]] constexpr bool HasDrops(std::uint16_t a_format) { return a_format == DROPS_ONLY || a_format == FORMAT; }
	[[nodiscard]] constexpr bool HasWires(std::uint16_t a_format) { return a_format == WIRES_ONLY || a_format == FORMAT; }
	[[nodiscard]] constexpr bool HasPluginTable(std::uint16_t a_format) { return a_format == PLUGIN_TABLE_ONLY || a_format == FORMAT; }

	[[nodiscard]] constexpr bool Readable(std::uint16_t a_format)
	{
		return Legacy(a_format) || (a_format >= 29 && a_format <= FORMAT);
	}

	struct Snapshot
	{
		std::uint32_t                                       contentHash = 0;
		std::uint64_t                                       sessionId = 0;
		// Player IDs carry on too, so a returning player (by identity) keeps theirs and a newcomer
		// never gets a saved builder's ID (their objects are keyed by it).
		std::uint32_t                                       nextId = 1;
		std::vector<std::pair<std::uint64_t, std::uint32_t>> identities;  // hello identity -> player ID
		std::vector<std::uint32_t>                          deadActors;
		// The deaths a build before layout 29 saved, which may be mistakes: those builds took an essential NPC
		// going down (Dogmeat) for dead. The host's game can undo one of these (Server's handleClaims), never a
		// death saved since. Every death of an older layout's file is one, until undone or reported again.
		std::vector<std::uint32_t>                          healable;
		std::vector<Protocol::IndexedContainerChange>       containerChanges;  // in index order
		std::vector<std::uint32_t>                          pickedUp;
		std::vector<Protocol::RefState>                     refStates;
		std::vector<Protocol::QuestStage>                   questStages;
		std::optional<Protocol::WorldTime>                  worldTime;
		std::vector<std::pair<std::uint32_t, std::uint8_t>> markers;
		std::vector<Protocol::WorkshopItem>                 workshopItems;
		// Items players dropped for each other (common/DropStore.h): those lying in the session, and those taken (takerId set),
		// which a taker's reload may bring back.
		std::vector<Protocol::DroppedItem>                  drops;
		// The plugins that number every form ID above (Protocol::PluginTable): kept, so a host who changes their mods,
		// or their order, carries on with the same world. Empty in a file of an older layout: its IDs are the load order
		// contentHash names, and only a host with that one carries on.
		std::vector<std::string>                            plugins;

		[[nodiscard]] bool Empty() const
		{
			return deadActors.empty() && containerChanges.empty() && pickedUp.empty() && refStates.empty() && questStages.empty() && !worldTime && markers.empty() && workshopItems.empty() && drops.empty();
		}
	};

	inline std::vector<std::uint8_t> Encode(const Snapshot& a_snapshot)
	{
		std::vector<std::uint8_t> out;
		const auto put = [&](const auto& a_value) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&a_value);
			out.insert(out.end(), bytes, bytes + sizeof(a_value));
		};
		const auto packet = [&](const std::vector<std::uint8_t>& a_packet) {
			put(static_cast<std::uint32_t>(a_packet.size()));
			out.insert(out.end(), a_packet.begin(), a_packet.end());
		};
		put(MAGIC);
		put(FORMAT);
		put(a_snapshot.contentHash);
		put(a_snapshot.sessionId);
		put(a_snapshot.nextId);
		put(static_cast<std::uint16_t>((std::min<std::size_t>)(a_snapshot.identities.size(), 65535)));
		for (std::size_t i = 0; i < a_snapshot.identities.size() && i < 65535; ++i) {
			put(a_snapshot.identities[i].first);
			put(a_snapshot.identities[i].second);
		}
		put(static_cast<std::uint32_t>(a_snapshot.healable.size()));
		for (const auto id : a_snapshot.healable) {
			put(id);
		}

		if (!a_snapshot.plugins.empty()) {
			packet(Protocol::Encode(Protocol::PluginTable{ a_snapshot.plugins }));
		}
		// World state in the same chunks a joiner gets.
		const auto slice = [](const auto& a_list, std::size_t a_offset) {
			using T = typename std::decay_t<decltype(a_list)>::value_type;
			const auto begin = (std::min)(a_list.size(), a_offset);
			const auto end = (std::min)(a_list.size(), begin + Protocol::WORLD_STATE_CHUNK);
			return std::vector<T>(a_list.begin() + begin, a_list.begin() + end);
		};
		const auto longest = (std::max)({ a_snapshot.deadActors.size(), a_snapshot.containerChanges.size(), a_snapshot.pickedUp.size(), a_snapshot.refStates.size(), a_snapshot.questStages.size() });
		for (std::size_t offset = 0; offset < longest; offset += Protocol::WORLD_STATE_CHUNK) {
			Protocol::WorldState world;
			world.deadActors = slice(a_snapshot.deadActors, offset);
			world.containerFirstIndex = static_cast<std::uint32_t>((std::min)(a_snapshot.containerChanges.size(), offset));
			world.containerChanges = slice(a_snapshot.containerChanges, offset);
			world.pickedUp = slice(a_snapshot.pickedUp, offset);
			world.refStates = slice(a_snapshot.refStates, offset);
			world.questStages = slice(a_snapshot.questStages, offset);
			packet(Protocol::Encode(world));
		}
		if (a_snapshot.worldTime) {
			packet(Protocol::Encode(*a_snapshot.worldTime, Protocol::MessageType::kWorldTime));
		}
		for (std::size_t i = 0; i < a_snapshot.markers.size(); i += Protocol::MAX_MARKERS_PER_PACKET) {
			Protocol::MarkersFound batch;
			for (std::size_t j = i; j < a_snapshot.markers.size() && j < i + Protocol::MAX_MARKERS_PER_PACKET; ++j) {
				batch.markers.push_back({ a_snapshot.markers[j].first, a_snapshot.markers[j].second });
			}
			packet(Protocol::Encode(batch, Protocol::MessageType::kMarkersFound));
		}
		for (std::size_t i = 0; i < a_snapshot.workshopItems.size(); i += Protocol::MAX_ACTORS_PER_PACKET) {
			const auto end = (std::min)(a_snapshot.workshopItems.size(), i + Protocol::MAX_ACTORS_PER_PACKET);
			packet(Protocol::Encode(std::vector<Protocol::WorkshopItem>(a_snapshot.workshopItems.begin() + i, a_snapshot.workshopItems.begin() + end)));
		}
		for (std::size_t i = 0; i < a_snapshot.drops.size(); i += Protocol::MAX_DROPS_PER_PACKET) {
			const auto end = (std::min)(a_snapshot.drops.size(), i + Protocol::MAX_DROPS_PER_PACKET);
			packet(Protocol::Encode(Protocol::DropList{ end == a_snapshot.drops.size(), { a_snapshot.drops.begin() + i, a_snapshot.drops.begin() + end } }));
		}
		return out;
	}

	// Nothing for a file of another layout, a truncated file, or a packet that doesn't decode: a
	// damaged save is ignored rather than half-applied.
	inline std::optional<Snapshot> Decode(std::span<const std::uint8_t> a_data)
	{
		std::size_t offset = 0;
		const auto  take = [&](auto& a_value) {
			if (offset + sizeof(a_value) > a_data.size()) {
				return false;
			}
			std::memcpy(&a_value, a_data.data() + offset, sizeof(a_value));
			offset += sizeof(a_value);
			return true;
		};
		std::uint32_t magic = 0;
		std::uint16_t version = 0;
		Snapshot      snapshot;
		std::uint16_t identityCount = 0;
		if (!take(magic) || !take(version) || !take(snapshot.contentHash) || !take(snapshot.sessionId) || !take(snapshot.nextId) || !take(identityCount) || magic != MAGIC || !Readable(version)) {
			return std::nullopt;
		}
		for (std::uint16_t i = 0; i < identityCount; ++i) {
			std::uint64_t identity = 0;
			std::uint32_t id = 0;
			if (!take(identity) || !take(id)) {
				return std::nullopt;
			}
			snapshot.identities.emplace_back(identity, id);
		}
		if (!Legacy(version)) {
			std::uint32_t healableCount = 0;
			if (!take(healableCount) || healableCount > (a_data.size() - offset) / sizeof(std::uint32_t)) {
				return std::nullopt;
			}
			snapshot.healable.resize(healableCount);
			for (auto& id : snapshot.healable) {
				take(id);
			}
		}
		while (offset < a_data.size()) {
			std::uint32_t length = 0;
			if (!take(length) || length == 0 || offset + length > a_data.size()) {
				return std::nullopt;
			}
			const auto packet = a_data.subspan(offset, length);
			offset += length;
			const auto type = Protocol::PeekType(packet);
			if (!type) {
				return std::nullopt;
			}
			switch (*type) {
			case Protocol::MessageType::kPluginTable: {
				auto table = HasPluginTable(version) ? Protocol::DecodePluginTable(packet) : std::nullopt;
				if (!table || !snapshot.plugins.empty()) {
					return std::nullopt;  // one, in the layout that has it
				}
				snapshot.plugins = std::move(table->plugins);
				break;
			}
			case Protocol::MessageType::kWorldState: {
				const auto world = Protocol::DecodeWorldState(packet);
				if (!world) {
					return std::nullopt;
				}
				snapshot.deadActors.insert(snapshot.deadActors.end(), world->deadActors.begin(), world->deadActors.end());
				snapshot.containerChanges.insert(snapshot.containerChanges.end(), world->containerChanges.begin(), world->containerChanges.end());
				snapshot.pickedUp.insert(snapshot.pickedUp.end(), world->pickedUp.begin(), world->pickedUp.end());
				snapshot.refStates.insert(snapshot.refStates.end(), world->refStates.begin(), world->refStates.end());
				snapshot.questStages.insert(snapshot.questStages.end(), world->questStages.begin(), world->questStages.end());
				break;
			}
			case Protocol::MessageType::kWorldTime: {
				const auto time = Protocol::DecodeWorldTime(packet);
				if (!time) {
					return std::nullopt;
				}
				snapshot.worldTime = *time;
				break;
			}
			case Protocol::MessageType::kMarkersFound: {
				const auto markers = Protocol::DecodeMarkers(packet);
				if (!markers) {
					return std::nullopt;
				}
				for (const auto& marker : markers->markers) {
					snapshot.markers.emplace_back(marker.refId, marker.flags);
				}
				break;
			}
			case Protocol::MessageType::kWorkshopItems: {
				const auto items = HasWires(version) ? Protocol::DecodeWorkshopItems(packet) : Protocol::DecodeWorkshopItemsBeforeWires(packet);
				if (!items) {
					return std::nullopt;
				}
				snapshot.workshopItems.insert(snapshot.workshopItems.end(), items->begin(), items->end());
				break;
			}
			case Protocol::MessageType::kDropList: {
				const auto list = HasDrops(version) ? Protocol::DecodeDropList(packet) : std::nullopt;
				if (!list) {
					return std::nullopt;
				}
				snapshot.drops.insert(snapshot.drops.end(), list->drops.begin(), list->drops.end());
				break;
			}
			default:
				return std::nullopt;
			}
		}
		if (Legacy(version)) {
			snapshot.healable = snapshot.deadActors;  // saved by a build that took an essential NPC going down for dead
		}
		return snapshot;
	}

	// Beside the file: the version before the last save, and a file that couldn't be read, set aside.
	[[nodiscard]] inline std::filesystem::path BackupOf(const std::filesystem::path& a_path) { return a_path.string() + ".bak"; }
	[[nodiscard]] inline std::filesystem::path SetAsideOf(const std::filesystem::path& a_path) { return a_path.string() + ".corrupt"; }

	inline std::optional<Snapshot> Load(const std::filesystem::path& a_path)
	{
		std::error_code error;
		if (!std::filesystem::is_regular_file(a_path, error)) {  // a folder in its place opens on Linux, and reading it throws
			return std::nullopt;
		}
		std::ifstream in{ a_path, std::ios::binary };
		if (!in) {
			return std::nullopt;
		}
		std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		return Decode(bytes);
	}

	// Written to a temporary file first, so a crash mid-write leaves the old save intact; the old save
	// then becomes the backup (a crash between the two renames leaves the backup, which Recover reads).
	// A write that fails or comes out short (a full disk) changes nothing, and is false. The file being
	// replaced becomes the backup only when it reads back whole: a damaged one (Recover couldn't set it
	// aside, or something cut it short since) must not push the good backup out.
	inline bool Save(const std::filesystem::path& a_path, const Snapshot& a_snapshot)
	{
		const auto                  bytes = Encode(a_snapshot);
		const std::filesystem::path temp = a_path.string() + ".tmp";
		std::error_code             error;
		const auto                  discard = [&] {
			if (std::filesystem::is_regular_file(temp, error)) {
				std::filesystem::remove(temp, error);
			}
			return false;
		};
		{
			std::ofstream out{ temp, std::ios::binary | std::ios::trunc };
			if (!out) {
				return discard();
			}
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			out.close();  // the last bytes are written here: a full disk shows now
			if (out.fail()) {
				return discard();
			}
		}
		if (const auto size = std::filesystem::file_size(temp, error); error || size != bytes.size()) {
			return discard();
		}
		if (Load(a_path)) {
			std::filesystem::rename(a_path, BackupOf(a_path), error);  // no backup is no reason not to save
		}
		std::filesystem::rename(temp, a_path, error);  // (on failure the whole .tmp stays: Recover reads it)
		return !error;
	}

	// The host's server at start: the session in a_path, or else what Save left of it. A file that is there
	// but can't be read (damaged, or a newer version's) is renamed aside first, so the next save neither
	// writes over it nor turns it into the backup, and the backup is read instead. With no file, only the
	// state a crash between Save's two renames leaves (the new file still temporary, the old one the backup)
	// is read: a player who deleted the file to start over starts over. Says what it did through a_log.
	inline std::optional<Snapshot> Recover(const std::filesystem::path& a_path, const std::function<void(const std::string&)>& a_log)
	{
		std::error_code error;
		const auto      backup = BackupOf(a_path);
		const auto      fromBackup = [&]() -> std::optional<Snapshot> {
			auto snapshot = Load(backup);
			if (snapshot) {
				a_log(std::format("carrying on from the backup, {} (the last few seconds before it may be missing)", backup.filename().string()));
			}
			return snapshot;
		};
		if (std::filesystem::is_regular_file(a_path, error)) {
			// One another program holds for a moment (an antivirus or a sync tool) isn't damaged: it gets a second to let go.
			for (int attempt = 0; attempt < 10 && !std::ifstream{ a_path, std::ios::binary }; ++attempt) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			if (auto snapshot = Load(a_path)) {
				return snapshot;
			}
			const auto aside = SetAsideOf(a_path);
			std::filesystem::rename(a_path, aside, error);
			a_log(error ? std::format("{} can't be read, nor renamed: {}", a_path.filename().string(), error.message()) :
			              std::format("{} can't be read; it is kept as {}", a_path.filename().string(), aside.filename().string()));
			return fromBackup();
		}
		const std::filesystem::path temp = a_path.string() + ".tmp";
		if (!std::filesystem::exists(temp, error) || !std::filesystem::exists(backup, error)) {
			return std::nullopt;
		}
		if (auto snapshot = Load(temp)) {  // written whole (Decode refuses a part), then the game stopped
			a_log(std::format("carrying on from {}, written just before the game stopped", temp.filename().string()));
			return snapshot;
		}
		return fromBackup();
	}
}
