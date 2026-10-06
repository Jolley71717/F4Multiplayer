#pragma once

// Settings read from Data/F4SE/Plugins/F4Multiplayer.ini (simple "key = value" lines,
// '#' or ';' start a comment). Missing keys keep their defaults.
namespace Config
{
	struct Settings
	{
		// Developer control channel (see DevChannel.h). Off unless explicitly enabled.
		bool          devChannel = false;
		std::uint16_t devChannelPort = 7790;
	};

	void            Load();
	const Settings& Get();
}
