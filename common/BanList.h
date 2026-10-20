#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// The players a host banned, kept on the host's disk (the session folder) so a ban outlives the server. A player is
// known by their Steam account (the transport's identity, which a client can't choose) and by the identity their game
// says hello with (all a UDP join has); the file holds neither, only salted hashes of them, and never a name. The salt
// is the file's own, picked when it is made: the hashes mean nothing without it, nor in another host's file.
//
// Each ban has an alias, "~" and six hex digits of its hash, by which the log names it (the log never names a player)
// and the dedicated server's console unbans it. Pure (files and nothing of the game), so it is unit tested.
class BanList
{
public:
	static constexpr std::size_t MAX_BANS = 1000;

	struct Entry
	{
		std::uint64_t steam = 0;     // the Steam account's hash (0: none known)
		std::uint64_t game = 0;      // their game's hello identity's hash (0: none)
		std::uint64_t bannedAt = 0;  // seconds since 1970 (UTC)
		std::string   name;          // as they were called when banned in this run: for the host's menu, never saved or logged

		[[nodiscard]] std::uint64_t Id() const { return steam ? steam : game; }
		[[nodiscard]] std::string   Alias() const { return AliasOf(Id()); }
	};

	BanList() :
		salt(NewSalt())
	{}
	explicit BanList(std::uint64_t a_salt) :
		salt(a_salt ? a_salt : 1)
	{}

	[[nodiscard]] static std::string AliasOf(std::uint64_t a_id) { return std::format("~{:06x}", a_id >> 40); }

	// The list in a_path, replacing this one. A missing file is an empty list (with a new salt, kept by the first
	// Save). Returns what went wrong, "" when nothing did; lines it can't read are skipped (and counted in it).
	std::string Load(const std::filesystem::path& a_path)
	{
		entries.clear();
		std::error_code error;
		if (!std::filesystem::exists(a_path, error)) {
			return {};
		}
		std::ifstream in{ a_path, std::ios::binary };
		if (!in) {
			return std::format("could not read {}", a_path.filename().string());
		}
		std::size_t skipped = 0;
		bool        salted = false;
		for (std::string line; std::getline(in, line);) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.empty() || line.front() == '#') {
				continue;
			}
			std::vector<std::string_view> words;
			for (std::string_view rest = line; !rest.empty();) {
				const auto start = rest.find_first_not_of(' ');
				if (start == std::string_view::npos) {
					break;
				}
				rest.remove_prefix(start);
				const auto end = rest.find(' ');
				words.push_back(rest.substr(0, end));
				rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
			}
			if (words.size() == 2 && words[0] == "salt" && Hex(words[1], salt) && salt != 0) {
				salted = true;
				continue;
			}
			Entry entry;
			if (words.size() == 4 && words[0] == "ban" && Hex(words[1], entry.steam) && Hex(words[2], entry.game) && Number(words[3], entry.bannedAt) &&
				entry.Id() != 0 && entries.size() < MAX_BANS) {
				entries.push_back(std::move(entry));
				continue;
			}
			++skipped;
		}
		if (!salted && !entries.empty()) {
			entries.clear();
			return std::format("{} has no salt line: its bans can't be matched, so none are kept", a_path.filename().string());
		}
		return skipped ? std::format("skipped {} unreadable line{} of {}", skipped, skipped == 1 ? "" : "s", a_path.filename().string()) : std::string{};
	}

	// Writes the list (to a temporary file first, then over the old one). False when it couldn't.
	[[nodiscard]] bool Save(const std::filesystem::path& a_path) const
	{
		std::error_code error;
		if (a_path.has_parent_path()) {
			std::filesystem::create_directories(a_path.parent_path(), error);
		}
		const std::filesystem::path temp = a_path.string() + ".tmp";
		{
			std::ofstream out{ temp, std::ios::binary | std::ios::trunc };
			out << "# F4Multiplayer: the players this host banned, one a line, as salted hashes of their Steam account and of their\n"
				   "# game's identity (no names, no Steam IDs). Unban in the F1 menu (Host controls), with the dedicated server's\n"
				   "# unban command, or by deleting the line while the server isn't running.\n";
			out << std::format("salt {:016x}\n", salt);
			for (const auto& entry : entries) {
				out << std::format("ban {:016x} {:016x} {}\n", entry.steam, entry.game, entry.bannedAt);
			}
			if (!out.flush()) {
				return false;
			}
		}
		std::filesystem::rename(temp, a_path, error);
		return !error;
	}

	// The ban that a player with these identities falls under (0: none known), or null.
	[[nodiscard]] const Entry* Find(std::uint64_t a_steamId, std::uint64_t a_gameId) const
	{
		const auto steam = a_steamId ? Hash('s', a_steamId) : 0;
		const auto game = a_gameId ? Hash('g', a_gameId) : 0;
		const auto it = std::ranges::find_if(entries, [&](const Entry& a_entry) {
			return (steam && a_entry.steam == steam) || (game && a_entry.game == game);
		});
		return it != entries.end() ? &*it : nullptr;
	}

	// Bans the player with these identities (an earlier ban of either one takes the other too). Null when there is
	// nothing to ban by (both 0) or the list is full.
	const Entry* Add(std::uint64_t a_steamId, std::uint64_t a_gameId, std::uint64_t a_now, std::string a_name)
	{
		const auto steam = a_steamId ? Hash('s', a_steamId) : 0;
		const auto game = a_gameId ? Hash('g', a_gameId) : 0;
		if (!steam && !game) {
			return nullptr;
		}
		const auto it = std::ranges::find_if(entries, [&](const Entry& a_entry) { return (steam && a_entry.steam == steam) || (game && a_entry.game == game); });
		if (it != entries.end()) {
			it->steam = it->steam ? it->steam : steam;
			it->game = it->game ? it->game : game;
			it->name = std::move(a_name);
			return &*it;
		}
		if (entries.size() >= MAX_BANS) {
			return nullptr;
		}
		entries.push_back({ steam, game, a_now, std::move(a_name) });
		return &entries.back();
	}

	// Lifts the ban of that id (Entry::Id). False when there's none.
	bool Remove(std::uint64_t a_id)
	{
		return a_id != 0 && std::erase_if(entries, [&](const Entry& a_entry) { return a_entry.Id() == a_id; }) > 0;
	}

	// The ban of that alias ("~3fa2c1", the "~" optional), or null; the first, should two share one.
	[[nodiscard]] const Entry* FindAlias(std::string_view a_alias) const
	{
		if (!a_alias.starts_with('~')) {
			return FindAlias(std::format("~{}", a_alias));
		}
		const auto it = std::ranges::find_if(entries, [&](const Entry& a_entry) { return a_entry.Alias() == a_alias; });
		return it != entries.end() ? &*it : nullptr;
	}

	// Lifts every ban; how many there were.
	std::size_t Clear() { return std::exchange(entries, {}).size(); }

	[[nodiscard]] const std::vector<Entry>& Entries() const { return entries; }
	[[nodiscard]] std::size_t               Size() const { return entries.size(); }

private:
	static std::uint64_t NewSalt()
	{
		std::random_device rd;
		std::uint64_t      value = 0;
		while (value == 0) {
			value = (static_cast<std::uint64_t>(rd()) << 32) | rd();
		}
		return value;
	}

	static std::uint64_t Mix(std::uint64_t a_x)
	{
		a_x ^= a_x >> 30;
		a_x *= 0xBF58476D1CE4E5B9ull;
		a_x ^= a_x >> 27;
		a_x *= 0x94D049BB133111EBull;
		a_x ^= a_x >> 31;
		return a_x;
	}

	// Never 0 (0 means "none").
	[[nodiscard]] std::uint64_t Hash(char a_kind, std::uint64_t a_id) const
	{
		const auto hash = Mix(Mix(a_id ^ salt) ^ (salt + static_cast<std::uint8_t>(a_kind)));
		return hash ? hash : 1;
	}

	static bool Hex(std::string_view a_text, std::uint64_t& a_out)
	{
		const auto [ptr, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), a_out, 16);
		return ec == std::errc{} && ptr == a_text.data() + a_text.size();
	}

	static bool Number(std::string_view a_text, std::uint64_t& a_out)
	{
		const auto [ptr, ec] = std::from_chars(a_text.data(), a_text.data() + a_text.size(), a_out);
		return ec == std::errc{} && ptr == a_text.data() + a_text.size();
	}

	std::uint64_t      salt;
	std::vector<Entry> entries;
};
