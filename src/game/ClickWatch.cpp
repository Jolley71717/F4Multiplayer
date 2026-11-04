#include "game/ClickWatch.h"

#include <mutex>

namespace ClickWatch
{
	namespace
	{
		std::mutex        lock;
		ClickRates::Rates rates;
	}

	void Note(ClickRates::Source a_source, std::uint32_t a_count)
	{
		std::uint32_t inWindow = 0;
		bool          report = false;
		{
			std::scoped_lock l{ lock };
			report = rates.Note(a_source, ClickRates::Clock::now(), a_count);
			inWindow = rates.InWindow(a_source);
		}
		if (report) {
			const auto& info = ClickRates::InfoOf(a_source);
			REX::WARN("ClickWatch: {} happened {} times in under {} s here (more than {} is a repeating sound): if something clicks over and over, this is where it comes from",
				info.label, inWindow, std::chrono::duration_cast<std::chrono::seconds>(ClickRates::WINDOW).count(), info.limit);
		}
	}

	std::string Describe()
	{
		std::scoped_lock l{ lock };
		return rates.Describe();
	}
}
