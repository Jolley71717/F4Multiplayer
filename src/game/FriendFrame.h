#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

// Whether our player (or a companion of theirs) is kept out of a power armor frame: one that stands in our game only
// because the session placed a copy of a friend's frame here. Getting into such a copy made it ours ("it is theirs now")
// while the friend's own frame stayed theirs in their game, and their game shared it again after their next load or
// rejoin: a second copy was placed on the same spot, two suits where the friend has one. Until borrowing a friend's
// suit is made (the frame staying the owner's, the friend only using it), our player is told whose it is instead
// (Equipment's frame gate: getting in, before the engine starts getting them in; Transfer, whose menu is closed again).
// A companion ordered into one is sent out of it again (SendsOut, RemotePlayers).
// Every copy the session placed is kept out, whoever used it last: each game makes its own runtime frames (an
// encounter's, a script's), a joiner's game shares a frame only when its player used it, and Spawns places only copies
// of what a friend's game shared, so a frame our own world made is never one of these copies. What our player loses is
// only a suit that only the friend's world has, which is borrowing. Never kept out: a frame from a plugin (every game
// has its own reference of those, the museum's suit in Concord among them, wherever FrameShare has moved ours), our
// player's own (in it, getting in, or got out of it last: a copy taken before this stays usable), and one a quest
// alias of ours holds. Kept free of the game so the rule can be tested.
namespace FriendFrame
{
	struct Facts
	{
		bool          ours = false;     // our player is in it, getting in, or got out of it last
		bool          quest = false;    // it fills a quest alias of ours
		bool          created = false;  // made at runtime (0xFF......): no plugin placed it
		std::uint32_t parkedFor = 0;    // the friend whose parked frame this is our copy of (RemotePlayers' parked copy)
		std::uint32_t leftFor = 0;      // the friend whose stand-in this copy was placed for: worn by it, or left where they got out
		std::uint32_t sharedBy = 0;     // the friend whose game shares the frame this is our copy of (Spawns)
		std::uint32_t waitingFor = 0;   // the friend whose copy this was, let go of by its holder while someone was being sent out of it
	};

	// The friend whose frame it is, when our player is kept out of it; 0 lets them in.
	constexpr std::uint32_t KeptOutFor(const Facts& a_facts)
	{
		if (a_facts.ours || a_facts.quest || !a_facts.created) {
			return 0;
		}
		if (a_facts.parkedFor != 0) {
			return a_facts.parkedFor;
		}
		if (a_facts.leftFor != 0) {
			return a_facts.leftFor;
		}
		if (a_facts.waitingFor != 0) {
			return a_facts.waitingFor;
		}
		return a_facts.sharedBy;
	}

	// Who is in a frame our player is kept out of, as far as sending them out goes.
	struct Occupant
	{
		bool player = false;    // our player (kept out as they get in: never found in one)
		bool standIn = false;   // a friend's stand-in, switched into its frame copy by the plugin
		bool runsHere = false;  // our game runs the actor (no friend's game drives it here)
	};

	// Whether someone in a frame is sent out of it again (Actor.SwitchToPowerArmor with None) rather than the frame given to
	// them as theirs: a companion ordered into a friend's frame copy, mostly. The engine's command walks them to it and has
	// them get in without asking the frame, so it cannot be turned down beforehand. a_keptOutFor: KeptOutFor's answer.
	constexpr bool SendsOut(const Occupant& a_occupant, std::uint32_t a_keptOutFor)
	{
		return a_keptOutFor != 0 && !a_occupant.player && !a_occupant.standIn && a_occupant.runsHere;
	}

	// As someone gets out of a frame, the engine builds its 3D again: on the rig it was gone for a moment a second or two
	// after the companion was out, and a 3D seen back as they got out was the old one. Until the new one is in, the copy is
	// left as it is (RemotePlayers::BeingSentOut): without its 3D it would be taken for one that never loaded and placed
	// again. SETTLE_MIN at least, whatever its 3D, and while it has none never longer than SETTLE_LIMIT, so one that never
	// gets it back is placed again in the end.
	constexpr auto SETTLE_MIN = std::chrono::seconds(10);
	constexpr auto SETTLE_LIMIT = std::chrono::seconds(30);

	// Whether a frame copy got out of a_since ago is still left as it is. a_back: its 3D is in (or the frame is gone).
	constexpr bool Settling(bool a_back, std::chrono::steady_clock::duration a_since)
	{
		return a_since <= SETTLE_MIN || (!a_back && a_since <= SETTLE_LIMIT);
	}

	// Who is in a frame copy as it is to go (a save or load, the friend leaving, their game sharing the frame again, a
	// copy placed anew), and what is done with it then. Nothing buries a copy with anyone in it: an NPC all the way in
	// wears it (the engine disabled it and cleared the seat), and burying it left them in a frame that was gone, or wrote
	// a save whose companion wears a frame the save does not have, which the engine fails on as it loads that save.
	// How our player holds a frame, as the engine tells it.
	struct OurHold
	{
		bool seated = false;    // the furniture they sit in: getting in
		bool wearing = false;   // the engine's worn frame (ExtraPowerArmor's): all the way in
		bool lastUsed = false;  // the frame they got out of last
	};

	// Whether a frame is our player's (Facts::ours, Holders::ours). All the way in, the engine has cleared the seat and has
	// no last frame for them yet: only its worn frame names it. Read from the seat and the last frame alone, a copy our
	// player wore on loan was taken for nobody's and buried from under them (its owner's game sharing it again after a
	// load, or the session ending), and the engine's exit from it never finished: E did nothing, its input layer left on.
	constexpr bool Ours(const OurHold& a_hold)
	{
		return a_hold.seated || a_hold.wearing || a_hold.lastUsed;
	}

	struct Holders
	{
		bool ours = false;        // our player is in it (getting in, or wearing it), or got out of it last (Ours)
		bool sendingOut = false;  // someone our game sends out of it (SendsOut) is in it, getting in, or on their way out
		bool inUse = false;       // anyone else here is in it or getting in (a friend's NPC, say)
	};

	enum class Leave
	{
		kBury,     // nobody is in it: it goes, as every copy of ours does
		kWait,     // it stays ours, as it is, until the one sent out of it is out; then it goes
		kRelease,  // it is theirs now: left to the game, and saved as the game's
	};

	constexpr Leave ChooseLeave(const Holders& a_holders)
	{
		if (a_holders.ours) {
			return Leave::kRelease;
		}
		if (a_holders.sendingOut) {
			return Leave::kWait;
		}
		return a_holders.inUse ? Leave::kRelease : Leave::kBury;
	}

	// A copy kept through a save (a companion being sent out of it), noted in that save's co-save: read back only with that
	// save, so no other load (an older save, a new game) can name a frame of its own world by the same runtime id.
	struct KeptCopy
	{
		std::uint32_t id = 0;     // its form ID
		std::uint32_t base = 0;   // the frame's base
		std::uint32_t owner = 0;  // the friend whose copy it was
		bool operator==(const KeptCopy&) const = default;
	};

	constexpr std::size_t MAX_KEPT_COPIES = 64;

	inline std::vector<std::uint8_t> Encode(const std::vector<KeptCopy>& a_copies)
	{
		std::vector<std::uint8_t> out;
		for (std::size_t i = 0; i < a_copies.size() && i < MAX_KEPT_COPIES; ++i) {
			for (const auto value : { a_copies[i].id, a_copies[i].base, a_copies[i].owner }) {
				for (int shift = 0; shift < 32; shift += 8) {
					out.push_back(static_cast<std::uint8_t>(value >> shift));
				}
			}
		}
		return out;
	}

	// Nothing at all for bytes that are not whole entries.
	inline std::vector<KeptCopy> Decode(const std::vector<std::uint8_t>& a_bytes)
	{
		constexpr std::size_t SIZE = 12;
		std::vector<KeptCopy> out;
		if (a_bytes.size() % SIZE != 0 || a_bytes.size() / SIZE > MAX_KEPT_COPIES) {
			return out;
		}
		const auto word = [&](std::size_t a_at) {
			std::uint32_t value = 0;
			for (std::size_t i = 0; i < 4; ++i) {
				value |= static_cast<std::uint32_t>(a_bytes[a_at + i]) << (8 * i);
			}
			return value;
		};
		for (std::size_t at = 0; at < a_bytes.size(); at += SIZE) {
			out.push_back({ word(at), word(at + 4), word(at + 8) });
		}
		return out;
	}

	// A copy kept through a save is waited on again once that save is loaded, when the form under its id is still the same
	// frame: made at runtime, not deleted, and of the base noted at the save.
	constexpr bool SameCopy(std::uint32_t a_notedBase, std::uint32_t a_base, bool a_created, bool a_deleted)
	{
		return a_notedBase != 0 && a_base == a_notedBase && a_created && !a_deleted;
	}

	// The frame gate's word for the engine's seating thread. Our player getting into a frame, and the look at one that comes
	// with it (Equipment's UseHook, through RemotePlayers::KeepOutOfFrame), are asked on the thread the engine seats them on,
	// beside the main one, which may be changing what the answer comes from: the session's players, Spawns' copies, the
	// loans. So the main thread puts down each frame what every frame here is kept for, and that thread only finds the frame
	// in it; what is told, or asked of an owner, it hands to the main thread. A frame not in it lets our player in.
	struct Kept
	{
		std::uint32_t frame = 0;    // its form ID
		std::uint32_t lentTo = 0;   // our own frame, lent to this friend (FrameLoans): nobody here gets into it
		std::uint32_t owner = 0;    // a friend's frame, our copy of it (KeptOutFor)
		const char*   kind = "";    // which copy of ours it is
		std::uint32_t spawnId = 0;  // the spawn owner's game shares it as (Spawns): our player may borrow it (FrameLoans)
	};

	inline const Kept* Find(const std::vector<Kept>& a_kept, std::uint32_t a_frame)
	{
		if (a_frame == 0) {
			return nullptr;
		}
		const auto it = std::ranges::find(a_kept, a_frame, &Kept::frame);
		return it != a_kept.end() ? &*it : nullptr;
	}

	enum class Enter
	{
		kIn,       // nothing keeps our player out
		kLent,     // our own frame, lent out: kept out, and told to whom
		kBorrow,   // a friend's shared frame: FrameLoans lets them in once it is lent to us, or asks its owner
		kKeptOut,  // a friend's frame: kept out, and told whose
	};

	constexpr Enter ChooseEnter(const Kept* a_kept)
	{
		if (!a_kept) {
			return Enter::kIn;
		}
		if (a_kept->lentTo != 0) {
			return Enter::kLent;
		}
		if (a_kept->owner == 0) {
			return Enter::kIn;
		}
		return a_kept->spawnId != 0 ? Enter::kBorrow : Enter::kKeptOut;
	}

	// The look (the log's, paframes'): whether our player would be kept out as a friend's frame, as the main thread says.
	constexpr bool Looked(const Kept* a_kept)
	{
		return a_kept && a_kept->owner != 0;
	}
}
