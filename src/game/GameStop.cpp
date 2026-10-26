#include "game/GameStop.h"

namespace GameStop
{
	bool Now()
	{
		const auto ui = RE::UI::GetSingleton();
		return !ui || ui->menuMode != 0;
	}
}
