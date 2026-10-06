#include "game/MapShare.h"

#include "Config.h"
#include "game/Hud.h"
#include "game/RemotePlayers.h"
#include "game/WorldSync.h"

namespace MapShare
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto POLL_INTERVAL = 2s;
		// MapMarkerData (not in CommonLibF4): TESFullName (name at +0x08), then the flags byte.
		constexpr std::size_t  NAME_OFFSET = 0x08;
		constexpr std::size_t  FLAGS_OFFSET = 0x10;
		constexpr std::uint8_t VISIBLE = 0x01;
		constexpr std::uint8_t CAN_TRAVEL = 0x02;  // discovered
		constexpr std::uint8_t SHARED = VISIBLE | CAN_TRAVEL;

		std::vector<std::uint32_t>                       markerIds;  // every map marker, found once
		bool                                             scanned = false;
		std::unordered_map<std::uint32_t, std::uint8_t>  known;      // flags last seen
		bool                                             baselined = false;
		std::vector<Protocol::MarkersFound>              waiting;    // until we're in the world
		std::vector<std::vector<std::uint8_t>>           outgoing;
		Clock::time_point                                nextPoll{};
		Clock::time_point                                feedWindow{};
		int                                              feedShown = 0;
		std::uint32_t                                    reported = 0;
		std::uint32_t                                    applied = 0;
		Clock::time_point                                lastDiscovery{};

		const std::uint8_t* MarkerData(std::uint32_t a_id)
		{
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(a_id);
			const auto extra = ref && ref->extraList ? ref->extraList->GetByType<RE::ExtraMapMarker>() : nullptr;
			return extra ? reinterpret_cast<const std::uint8_t*>(extra->mapMarkerData) : nullptr;
		}

		std::optional<std::uint8_t> Flags(std::uint32_t a_id)
		{
			const auto data = MarkerData(a_id);
			return data ? std::optional{ static_cast<std::uint8_t>(data[FLAGS_OFFSET] & SHARED) } : std::nullopt;
		}

		std::string NameOf(std::uint32_t a_id)
		{
			const auto data = MarkerData(a_id);
			const auto name = data ? reinterpret_cast<const RE::BSFixedString*>(data + NAME_OFFSET)->c_str() : nullptr;
			return name ? name : "";
		}

		// Map markers are persistent references: loaded with the game data and never deleted.
		void Scan()
		{
			scanned = true;
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			RE::BSAutoReadLock l{ lock };
			if (!map) {
				return;
			}
			for (const auto& [id, form] : *map) {
				const auto ref = form ? form->As<RE::TESObjectREFR>() : nullptr;
				if (ref && Protocol::IsShareableRef(id) && ref->extraList && ref->extraList->GetByType<RE::ExtraMapMarker>()) {
					markerIds.push_back(id);
				}
			}
			REX::INFO("MapShare: {} map markers", markerIds.size());
		}

		void Poll()
		{
			Protocol::MarkersFound found;
			for (const auto id : markerIds) {
				const auto flags = Flags(id);
				if (!flags) {
					continue;
				}
				auto& last = known[id];
				const std::uint8_t gained = *flags & ~last;
				last = *flags;
				if (baselined && gained != 0) {
					found.markers.push_back({ id, *flags });
					lastDiscovery = Clock::now();
				}
			}
			baselined = true;
			for (std::size_t i = 0; i < found.markers.size(); i += Protocol::MAX_MARKERS_PER_PACKET) {
				const auto end = (std::min)(found.markers.size(), i + Protocol::MAX_MARKERS_PER_PACKET);
				outgoing.push_back(Protocol::Encode(Protocol::MarkersFound{ 0, { found.markers.begin() + i, found.markers.begin() + end } }, Protocol::MessageType::kReportMarkers));
				reported += static_cast<std::uint32_t>(end - i);
			}
		}

		void ApplyNow(const Protocol::MarkersFound& a_found)
		{
			std::vector<std::string> discovered;
			for (const auto& marker : a_found.markers) {
				const auto flags = Flags(marker.refId);
				const std::uint8_t want = marker.flags & SHARED;
				if (!flags || (*flags & want) == want) {
					continue;
				}
				// The console command records the change for the save (writing the flags wouldn't).
				RE::Console::ExecuteCommand(std::format("showmap {:08X}{}", marker.refId, (want & CAN_TRAVEL) ? " 1" : "").c_str());
				known[marker.refId] = Flags(marker.refId).value_or(want);  // not our discovery
				++applied;
				if ((want & CAN_TRAVEL) && !(*flags & CAN_TRAVEL)) {
					discovered.push_back(NameOf(marker.refId));
				}
			}
			if (a_found.playerId == 0 || discovered.empty()) {
				return;
			}
			const auto who = RemotePlayers::NameOf(a_found.playerId);
			const auto now = Clock::now();
			if (now >= feedWindow) {
				feedWindow = now + 5s;
				feedShown = 0;
			}
			for (const auto& name : discovered) {
				if (!name.empty() && ++feedShown <= 3) {
					Hud::Notify(std::format("{} discovered {}", who.empty() ? "A friend" : who, name));
				}
			}
		}
	}

	void Frame()
	{
		if (!Config::Get().shareMap || !WorldSync::InWorld()) {
			return;
		}
		if (!scanned) {
			Scan();
		}
		for (const auto& found : std::exchange(waiting, {})) {
			ApplyNow(found);
		}
		const auto now = Clock::now();
		if (now >= nextPoll) {
			nextPoll = now + POLL_INTERVAL;
			Poll();
		}
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void Apply(const Protocol::MarkersFound& a_found)
	{
		if (!Config::Get().shareMap) {
			return;
		}
		if (!WorldSync::InWorld() || !scanned) {
			if (waiting.size() < 256) {
				waiting.push_back(a_found);
			}
			return;
		}
		ApplyNow(a_found);
	}

	Clock::time_point LastDiscovery()
	{
		return lastDiscovery;
	}

	void Rebaseline()
	{
		known.clear();
		baselined = false;
	}

	void Reset()
	{
		waiting.clear();
		outgoing.clear();
	}

	std::string Describe()
	{
		std::size_t discovered = 0;
		for (const auto& [id, flags] : known) {
			discovered += (flags & CAN_TRAVEL) != 0;
		}
		return std::format("map: markers={} discovered={} reported={} applied={} waiting={}", markerIds.size(), discovered, reported, applied, waiting.size());
	}
}
