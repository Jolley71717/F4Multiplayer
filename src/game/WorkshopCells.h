#pragma once

#include <cmath>
#include <compare>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

// Where in the world something waits for its cell to load: the copies of what other players built
// wait for their settlement, and their scraps of the world itself for the scrapped reference. What
// waits is looked at again only when its cell, or one next to it, attaches, or, if it waits for a
// reference, when a cell holding that reference attaches, instead of all of it every few seconds (a
// saved session can hold thousands of scraps).
namespace WorkshopCells
{
	inline constexpr float CELL_SIZE = 4096.0f;  // an exterior cell's side, in game units

	// An interior cell, or a worldspace and the grid coordinates of one of its exterior cells.
	struct Where
	{
		std::uint32_t space = 0;  // the interior cell or the worldspace; 0 is nowhere (its workbench isn't in this game)
		std::int32_t  x = 0;
		std::int32_t  y = 0;

		auto operator<=>(const Where&) const = default;
	};

	// The exterior cell grid coordinate a world coordinate falls in.
	inline std::int32_t Grid(float a_coordinate)
	{
		return static_cast<std::int32_t>(std::floor(a_coordinate / CELL_SIZE));
	}

	inline Where Exterior(std::uint32_t a_worldspace, float a_x, float a_y)
	{
		return { a_worldspace, Grid(a_x), Grid(a_y) };
	}

	inline Where Interior(std::uint32_t a_cell)
	{
		return { a_cell, 0, 0 };
	}

	// Things waiting, each under its key, in one cell.
	template <class Key, class Item>
	class Waiting
	{
	public:
		// Adds the item, or replaces the one under its key (which may have waited somewhere else).
		// a_ref: the reference it waits for (0 if none), by which it's found too, whatever cell it waits
		// in: a scrap tells where its player's piece stood, which isn't always where ours stands.
		void Put(const Where& a_where, const Key& a_key, const Item& a_item, std::uint32_t a_ref = 0)
		{
			Erase(a_key);
			items.emplace(a_key, Entry{ a_where, a_ref, a_item });
			byCell[a_where].insert(a_key);
			if (a_ref != 0) {
				byRef[a_ref].insert(a_key);
			}
		}

		bool Erase(const Key& a_key)
		{
			const auto it = items.find(a_key);
			if (it == items.end()) {
				return false;
			}
			if (const auto cell = byCell.find(it->second.where); cell != byCell.end()) {
				cell->second.erase(a_key);
				if (cell->second.empty()) {
					byCell.erase(cell);
				}
			}
			if (const auto ref = byRef.find(it->second.ref); ref != byRef.end()) {
				ref->second.erase(a_key);
				if (ref->second.empty()) {
					byRef.erase(ref);
				}
			}
			items.erase(it);
			return true;
		}

		[[nodiscard]] const Item* Find(const Key& a_key) const
		{
			const auto it = items.find(a_key);
			return it != items.end() ? &it->second.item : nullptr;
		}

		// The keys waiting in this cell and the eight around it: a reference near a cell's edge may
		// belong to the next cell. An interior has no neighbours (nothing waits at its other
		// coordinates).
		[[nodiscard]] std::vector<Key> Near(const Where& a_cell) const
		{
			std::vector<Key> keys;
			if (a_cell.space == 0) {
				return keys;
			}
			for (std::int32_t dx = -1; dx <= 1; ++dx) {
				for (std::int32_t dy = -1; dy <= 1; ++dy) {
					if (const auto cell = byCell.find({ a_cell.space, a_cell.x + dx, a_cell.y + dy }); cell != byCell.end()) {
						keys.insert(keys.end(), cell->second.begin(), cell->second.end());
					}
				}
			}
			return keys;
		}

		// The keys waiting for this reference, in whatever cell they wait.
		[[nodiscard]] std::vector<Key> For(std::uint32_t a_ref) const
		{
			const auto ref = a_ref != 0 ? byRef.find(a_ref) : byRef.end();
			return ref != byRef.end() ? std::vector<Key>(ref->second.begin(), ref->second.end()) : std::vector<Key>{};
		}

		// Whether anything waits for a reference (a cell's references are worth looking through).
		[[nodiscard]] bool WaitsForRefs() const { return !byRef.empty(); }

		[[nodiscard]] std::size_t Size() const { return items.size(); }
		[[nodiscard]] bool        Empty() const { return items.empty(); }

		void Clear()
		{
			items.clear();
			byCell.clear();
			byRef.clear();
		}

	private:
		struct Entry
		{
			Where         where;
			std::uint32_t ref;  // the reference it waits for; 0 if none
			Item          item;
		};

		std::map<Key, Entry>                             items;
		std::map<Where, std::set<Key>>                   byCell;
		std::unordered_map<std::uint32_t, std::set<Key>> byRef;
	};
}
