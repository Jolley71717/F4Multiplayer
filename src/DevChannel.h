#pragma once

// Developer control channel: a line-based text protocol on 127.0.0.1 that lets local
// test tools query game state and run console commands.
//
// Safety:
//   - disabled unless bDevChannel = true in F4Multiplayer.ini
//   - bound to the loopback interface only
//   - every connection must first send "auth <token>"; the token is regenerated each
//     launch and written to Documents/My Games/Fallout4/F4SE/F4Multiplayer_dev.token,
//     which only the current Windows user can read
namespace DevChannel
{
	// Starts the listener on a background thread if enabled in the config.
	void Start();
}
