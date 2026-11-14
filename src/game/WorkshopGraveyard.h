#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// The copies of friends' workshop pieces waiting to be deleted (WorkshopSync's graveyard): their cell isn't loaded, or
// their 3D still is (DeferredDelete). The graveyard is looked through four times a second, each one not loaded looked up
// by its ID, until it goes or a load lets the whole list go. A copy is removed more than once (a friend's scrap, then
// leaving the session): it waits once, the newer entry in place of the older (an ID names one reference at a time, so
// the newer entry is the one the ID stands for). No cap: one entry per copy, and the copies are as many as the session
// keeps (WorkshopAudit::MAX_KEPT on the server), all buried at once when we leave a big settlement; a copy left out would
// stay in our world, untracked, and be saved. By ID through an index, so burying thousands at once stays linear. Free of
// the game: tests/WorkshopGraveyardTests.cpp.
namespace WorkshopGraveyard
{
	enum class Admitted
	{
		kAdded,    // waits now
		kAlready,  // waited already: its entry is the newer one now
	};

	// Copy is what waits; IdOf (stateless) gives its reference ID, never 0.
	template <class Copy, class IdOf>
	class List
	{
	public:
		Admitted Admit(const Copy& a_copy)
		{
			const std::uint32_t id = IdOf{}(a_copy);
			if (const auto it = at.find(id); it != at.end()) {
				waiting[it->second] = a_copy;
				return Admitted::kAlready;
			}
			at.emplace(id, waiting.size());
			waiting.push_back(a_copy);
			return Admitted::kAdded;
		}

		[[nodiscard]] bool Contains(std::uint32_t a_id) const { return at.contains(a_id); }

		// Drops those a_pred (given each as Copy&, to change) says go; the rest keep their order.
		template <class Pred>
		std::size_t EraseIf(Pred a_pred)
		{
			const auto gone = std::erase_if(waiting, a_pred);
			if (gone > 0) {
				at.clear();
				for (std::size_t i = 0; i < waiting.size(); ++i) {
					at.emplace(IdOf{}(waiting[i]), i);
				}
			}
			return gone;
		}

		void Clear()
		{
			waiting.clear();
			at.clear();
		}

		[[nodiscard]] std::size_t size() const { return waiting.size(); }
		[[nodiscard]] bool        empty() const { return waiting.empty(); }
		[[nodiscard]] auto        begin() const { return waiting.begin(); }
		[[nodiscard]] auto        end() const { return waiting.end(); }

	private:
		std::vector<Copy>                          waiting;
		std::unordered_map<std::uint32_t, std::size_t> at;  // each ID's place in waiting
	};
}
