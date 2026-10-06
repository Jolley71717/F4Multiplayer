#include "game/Hud.h"

namespace Hud
{
	void Notify(const std::string& a_message)
	{
		REX::INFO("Hud: {}", a_message);
		RE::SendHUDMessage::ShowHUDMessage(a_message.c_str(), "", false, false);
	}
}
