#pragma once

#include "Protocol.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The session's shared world, saved on the host's disk so it survives the host quitting: the next
// time the host plays with the same load order, the session carries on (dead NPCs stay dead, loot
// stays taken, built walls stand). The file is a header followed by ordinary protocol packets
// (the same ones a joiner gets), each length-prefixed, so nothing here has its own wire format.
namespace SessionFile
{
	inline constexpr std::uint32_t MAGIC = 0x53504D46;  // "FMPS"

	struct Snapshot
	{
		std::uint32_t                                       contentHash = 0;
		std::uint64_t                                       sessionId = 0;
		// Player IDs carry on too, so a returning player (by identity) keeps theirs and a newcomer
		// never gets a saved builder's ID (their objects are keyed by it).
		std::uint32_t                                       nextId = 1;
		std::vector<std::pair<std::uint64_t, std::uint32_t>> identities;  // hello identity -> player ID
		std::vector<std::uint32_t>                          deadActors;
		std::vector<Protocol::IndexedContainerChange>       containerChanges;  // in index order
		std::vector<std::uint32_t>                          pickedUp;
		std::vector<Protocol::RefState>                     refStates;
		std::vector<Protocol::QuestStage>                   questStages;
		std::optional<Protocol::WorldTime>                  worldTime;
		std::vector<std::pair<std::uint32_t, std::uint8_t>> markers;
		std::vector<Protocol::WorkshopItem>                 workshopItems;

		[[nodiscard]] bool Empty() const
		{
			return deadActors.empty() && containerChanges.empty() && pickedUp.empty() && refStates.empty() && questStages.empty() && !worldTime && markers.empty() && workshopItems.empty();
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
		put(Protocol::VERSION);
		put(a_snapshot.contentHash);
		put(a_snapshot.sessionId);
		put(a_snapshot.nextId);
		put(static_cast<std::uint16_t>((std::min<std::size_t>)(a_snapshot.identities.size(), 65535)));
		for (std::size_t i = 0; i < a_snapshot.identities.size() && i < 65535; ++i) {
			put(a_snapshot.identities[i].first);
			put(a_snapshot.identities[i].second);
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
		return out;
	}

	// Nothing for a file from another protocol version, a truncated file, or a packet that doesn't
	// decode: a damaged save is ignored rather than half-applied.
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
		if (!take(magic) || !take(version) || !take(snapshot.contentHash) || !take(snapshot.sessionId) || !take(snapshot.nextId) || !take(identityCount) || magic != MAGIC || version != Protocol::VERSION) {
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
				const auto items = Protocol::DecodeWorkshopItems(packet);
				if (!items) {
					return std::nullopt;
				}
				snapshot.workshopItems.insert(snapshot.workshopItems.end(), items->begin(), items->end());
				break;
			}
			default:
				return std::nullopt;
			}
		}
		return snapshot;
	}

	// Written to a temporary file first, so a crash mid-write leaves the old save intact.
	inline bool Save(const std::filesystem::path& a_path, const Snapshot& a_snapshot)
	{
		const auto bytes = Encode(a_snapshot);
		const auto temp = a_path.string() + ".tmp";
		{
			std::ofstream out{ temp, std::ios::binary | std::ios::trunc };
			if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
				return false;
			}
		}
		std::error_code error;
		std::filesystem::rename(temp, a_path, error);
		return !error;
	}

	inline std::optional<Snapshot> Load(const std::filesystem::path& a_path)
	{
		std::ifstream in{ a_path, std::ios::binary };
		if (!in) {
			return std::nullopt;
		}
		std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		return Decode(bytes);
	}
}
