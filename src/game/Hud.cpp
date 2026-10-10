#include "game/Hud.h"

namespace Hud
{
	void Notify(const std::string& a_message)
	{
		// Some of this text comes from other players (names, lines, reject reasons): control characters
		// and markup are dropped before the HUD's text field sees them.
		std::string clean;
		clean.reserve(a_message.size());
		for (const unsigned char c : a_message) {
			if (c >= 0x20 && c != 0x7F && c != '<' && c != '>') {
				clean.push_back(static_cast<char>(c));
			}
		}
		REX::INFO("Hud: {}", clean);
		RE::SendHUDMessage::ShowHUDMessage(clean.c_str(), "", false, false);
	}
}
