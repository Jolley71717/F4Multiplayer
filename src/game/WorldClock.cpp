#include "game/WorldClock.h"

#include "Config.h"
#include "game/WorldSync.h"

namespace WorldClock
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto  REPORT_INTERVAL = 5s;
		constexpr float MAX_HOUR_DRIFT = 0.25f;  // game hours (15 game minutes, ~45 real seconds)

		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      nextReport{};
		std::optional<Protocol::WorldTime>     target;  // the host's, waiting to be applied
		std::uint32_t                          hourSets = 0;
		std::uint32_t                          weatherSets = 0;

		RE::TESGlobal* GameHour()
		{
			const auto calendar = RE::Calendar::GetSingleton();
			return calendar ? calendar->gameHour : nullptr;
		}

		std::uint32_t Worldspace(RE::PlayerCharacter* a_player)
		{
			const auto cell = a_player->GetParentCell();
			return cell && !cell->IsInterior() && cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
		}

		void ApplyTarget()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto hour = GameHour();
			if (!target || !player || !player->GetParentCell() || !hour || !WorldSync::InWorld()) {
				return;
			}

			// Shortest way around the clock.
			float drift = std::fabs(hour->value - target->gameHour);
			drift = (std::min)(drift, 24.0f - drift);
			if (drift > MAX_HOUR_DRIFT) {
				hour->value = target->gameHour;
				++hourSets;
			}

			// Weather only where it belongs: outside, in the host's worldspace.
			const auto sky = RE::Sky::GetSingleton();
			const auto worldspace = Worldspace(player);
			if (sky && target->weather && worldspace != 0 && worldspace == target->worldspace &&
				(!sky->currentWeather || sky->currentWeather->GetFormID() != target->weather)) {
				if (const auto weather = RE::TESForm::GetFormByID<RE::TESWeather>(target->weather)) {
					sky->ForceWeather(weather, false);
					++weatherSets;
				}
			}
			target.reset();
		}
	}

	void Frame()
	{
		ApplyTarget();
		const auto now = Clock::now();
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto hour = GameHour();
		if (now < nextReport || !Config::Get().syncTime || !player || !player->GetParentCell() || !hour || !WorldSync::InWorld()) {
			return;
		}
		nextReport = now + REPORT_INTERVAL;
		Protocol::WorldTime time;
		time.gameHour = std::clamp(hour->value, 0.0f, 23.999f);
		time.worldspace = Worldspace(player);
		const auto sky = RE::Sky::GetSingleton();
		time.weather = time.worldspace && sky && sky->currentWeather ? sky->currentWeather->GetFormID() : 0;
		outgoing.push_back(Protocol::Encode(time, Protocol::MessageType::kReportTime));
	}

	std::vector<std::vector<std::uint8_t>> TakeOutgoing()
	{
		return std::exchange(outgoing, {});
	}

	void Apply(const Protocol::WorldTime& a_time)
	{
		if (Config::Get().syncTime) {
			target = a_time;
		}
	}

	void Reset()
	{
		outgoing.clear();
		target.reset();
	}

	std::string Describe()
	{
		const auto hour = GameHour();
		return std::format("clock: hour={:.2f} hourSets={} weatherSets={}", hour ? hour->value : -1.0f, hourSets, weatherSets);
	}
}
