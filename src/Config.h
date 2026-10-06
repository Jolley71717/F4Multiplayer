#pragma once

// Settings read from Data/F4SE/Plugins/F4Multiplayer.ini (simple "key = value" lines,
// '#' or ';' start a comment). Missing keys keep their defaults.
namespace Config
{
	struct Settings
	{
		// Name shown to other players.
		std::string playerName = "Vault Dweller";

		// Host a session on this machine. The host also plays; friends connect to the host's IP.
		bool host = false;

		// Address of the host to join ("ip" or "ip:port"). Ignored when hosting.
		std::string serverAddress;

		std::uint16_t port = 7779;
		std::size_t   maxPlayers = 4;
		std::string   password;

		// NPC base form used for other players' characters.
		std::uint32_t puppetBaseForm = 0x0002268A;  // Magnolia

		// Turn vsync off during loading screens so they finish faster.
		bool fastLoading = true;

		// Developer control channel (see DevChannel.h). Off unless explicitly enabled.
		bool          devChannel = false;
		std::uint16_t devChannelPort = 7790;
	};

	void            Load();
	const Settings& Get();
}
