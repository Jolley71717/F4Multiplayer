#pragma once

#include "IniFile.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <ios>
#include <sstream>
#include <string>
#include <thread>

// The player id file (F4Multiplayer_player.id, net/Identity.h) read, or made when there is none: the file work of
// Identity::Get, with no game in it, so the tests run it on real files.
namespace IdentityFile
{
	enum class Kept
	{
		Read,        // the file's identity
		Created,     // there was none (or it held no identity): a new one, written as the file
		NotCreated,  // a new one that could not be written: it holds for this run of the game
		Unreadable,  // the file is there and can't be read now: left as it is, and a new one holds for this run
	};

	struct Loaded
	{
		std::uint64_t id = 0;
		Kept          what = Kept::Read;
	};

	// The identity a file's text holds (hexadecimal), 0 for none.
	[[nodiscard]] inline std::uint64_t Parse(const std::string& a_text)
	{
		std::uint64_t      id = 0;
		std::istringstream in{ a_text };
		return (in >> std::hex >> id) ? id : 0;
	}

	// How long a file that can't be read is given before the second try: a sync tool or a virus scan holds a
	// file for a moment, and the identity is read once, as a session starts, not in a frame.
	inline void WaitAMoment()
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}

	// The identity in the file at a_path; with no file, or one that holds none, a new one (a_random(): 64 random bits,
	// asked again while it gives 0) written as the file. A file that is there but can't be read (another program
	// holds it) is tried once more after a_wait(); if it still can't be read it still holds the player's identity: a
	// new one written over it would be theirs for good, and every host's session would take them for a stranger from
	// then on. That file is left as it is, this run goes by a new identity, and the next start reads the file again.
	template <class Random, class Wait = void (*)()>
	[[nodiscard]] Loaded LoadOrCreate(const std::filesystem::path& a_path, Random&& a_random, Wait&& a_wait = WaitAMoment)
	{
		auto file = IniFile::ReadText(a_path);
		if (file.what == IniFile::Found::Unreadable) {
			a_wait();
			file = IniFile::ReadText(a_path);
		}
		if (file.what == IniFile::Found::Read) {
			if (const auto id = Parse(file.text)) {
				return { id, Kept::Read };
			}
		}
		std::uint64_t id = 0;
		while (id == 0) {
			id = a_random();
		}
		if (file.what == IniFile::Found::Unreadable) {
			return { id, Kept::Unreadable };
		}
		return { id, IniFile::WriteWhole(a_path, std::format("{:016x}", id)) ? Kept::Created : Kept::NotCreated };
	}
}
