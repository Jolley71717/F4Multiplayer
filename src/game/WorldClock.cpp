#include "game/WorldClock.h"

#include "Config.h"
#include "game/Story.h"
#include "game/WorldSync.h"

namespace WorldClock
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto  REPORT_INTERVAL = 5s;
		constexpr float MAX_HOUR_DRIFT = 0.25f;  // game hours (15 game minutes, ~45 real seconds)
		// A bigger jump means the host loaded a save: line the calendars up again.
		constexpr double MAX_FOLLOW_HOURS = 72.0;
		// Moving the clock forward stops just short of midnight and lets the game roll the date over.
		constexpr float BEFORE_MIDNIGHT = 23.99f;

		std::vector<std::vector<std::uint8_t>> outgoing;
		Clock::time_point                      nextReport{};
		std::optional<Protocol::WorldTime>     target;  // the host's, waiting to be applied
		// The game keeps GameDaysPassed at midnights passed + hour / 24. Ours minus the host's, once
		// lined up; each player's save has its own count.
		std::optional<double>                  dayOffset;
		std::uint32_t                          clockOwner = 0;
		float                                  advanceLeft = 0.0f;  // hours still to move forward
		bool                                   atMidnight = false;  // waiting for the game to roll over
		std::uint32_t                          hourSets = 0;
		std::uint32_t                          weatherSets = 0;
		std::uint32_t                          midnightSteps = 0;

		RE::Calendar* Calendar()
		{
			const auto calendar = RE::Calendar::GetSingleton();
			return calendar && calendar->gameHour && calendar->gameDaysPassed ? calendar : nullptr;
		}

		std::uint32_t Worldspace(RE::PlayerCharacter* a_player)
		{
			const auto cell = a_player->GetParentCell();
			return cell && !cell->IsInterior() && cell->worldSpace ? cell->worldSpace->GetFormID() : 0;
		}

		bool Ready()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			// The opening keeps its own time and weather (and its clock isn't the session's).
			return player && player->GetParentCell() && Calendar() && WorldSync::InWorld() && !Story::InOpening();
		}

		// Moves the clock forward like waiting does, one midnight at a time, so the date and the
		// day count stay right.
		void Advance()
		{
			const auto calendar = Calendar();
			if (advanceLeft <= 0.0f || !calendar) {
				return;
			}
			const float hour = calendar->gameHour->value;
			if (atMidnight) {
				if (hour >= BEFORE_MIDNIGHT) {
					return;  // the game hasn't rolled over yet (paused in a menu)
				}
				atMidnight = false;
			}
			if (hour + advanceLeft < BEFORE_MIDNIGHT) {
				calendar->gameHour->value = hour + advanceLeft;
				advanceLeft = 0.0f;
				++hourSets;
			} else {
				calendar->gameHour->value = BEFORE_MIDNIGHT;
				advanceLeft -= 24.0f - hour;  // the rest of the day, through midnight
				atMidnight = true;
				++midnightSteps;
			}
		}

		void FollowHost(RE::Calendar* a_calendar, const Protocol::WorldTime& a_time)
		{
			const float  hour = a_calendar->gameHour->value;
			const double days = a_calendar->gameDaysPassed->value;
			double       delta = 0.0;  // hours to move
			if (dayOffset && a_time.playerId == clockOwner) {
				delta = (a_time.daysPassed + *dayOffset - days) * 24.0;
			}
			if (!dayOffset || a_time.playerId != clockOwner || std::fabs(delta) > MAX_FOLLOW_HOURS) {
				// Line up with the host's time of day: forward, or back if that stays on the same day.
				delta = std::fmod(a_time.gameHour - hour + 24.0, 24.0);
				if (delta > 12.0 && hour - (24.0 - delta) >= 0.0) {
					delta -= 24.0;
				}
				dayOffset = days + delta / 24.0 - a_time.daysPassed;
				clockOwner = a_time.playerId;
			}
			if (delta > MAX_HOUR_DRIFT) {
				advanceLeft = static_cast<float>(delta);
				Advance();
			} else if (delta < -MAX_HOUR_DRIFT && hour + delta >= 0.0) {
				// Ahead of the host: wait for them within the day (never back across midnight).
				a_calendar->gameHour->value = static_cast<float>(hour + delta);
				++hourSets;
			}
		}

		void ApplyTarget()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto calendar = Calendar();
			if (!target || !Ready()) {
				return;
			}
			if (advanceLeft <= 0.0f) {
				FollowHost(calendar, *target);
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
		if (Ready()) {
			Advance();
		}
		ApplyTarget();
		const auto now = Clock::now();
		const auto calendar = Calendar();
		if (now < nextReport || !Config::Get().syncTime || !Ready()) {
			return;
		}
		nextReport = now + REPORT_INTERVAL;
		Protocol::WorldTime time;
		time.gameHour = std::clamp(calendar->gameHour->value, 0.0f, 23.999f);
		time.daysPassed = (std::max)(calendar->gameDaysPassed->value, 0.0f);
		const auto player = RE::PlayerCharacter::GetSingleton();
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

	void Rebaseline()
	{
		dayOffset.reset();
		advanceLeft = 0.0f;
		atMidnight = false;
	}

	void Reset()
	{
		outgoing.clear();
		target.reset();
		Rebaseline();
	}

	std::string Describe()
	{
		const auto c = Calendar();
		if (!c || !c->gameDay) {
			return "clock: none";
		}
		return std::format("clock: hour={:.3f} days={:.4f} date={}/{} offset={:.4f} owner={} hourSets={} midnightSteps={} weatherSets={}",
			c->gameHour->value, c->gameDaysPassed->value, c->gameMonth ? c->gameMonth->value + 1 : 0.0f, c->gameDay->value,
			dayOffset.value_or(0.0), clockOwner, hourSets, midnightSteps, weatherSets);
	}
}
