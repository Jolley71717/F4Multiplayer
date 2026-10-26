#include "game/GameStop.h"

namespace GameStop
{
	std::optional<std::uint32_t> MenuMode()
	{
		const auto ui = RE::UI::GetSingleton();
		return ui ? std::optional<std::uint32_t>{ ui->menuMode } : std::nullopt;
	}

	bool Now()
	{
		const auto mode = MenuMode();
		return !mode || *mode != 0;
	}
}
