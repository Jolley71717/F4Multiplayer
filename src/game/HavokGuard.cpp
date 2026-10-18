#include "game/HavokGuard.h"

#include "game/HavokNan.h"
#include "game/Hud.h"
#include "game/Puppets.h"

#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <limits>
#include <mutex>

namespace HavokGuard
{
	namespace
	{
		using Clock = HavokNan::Cleanup::Clock;

		// Each copy of the exit test that has our detour: where its count is (inside the detour's memory).
		std::array<std::uint32_t*, HavokNan::SITES.size()> counts{};
		std::atomic<int>                                   installed{ 0 };

		std::uint64_t Counted()
		{
			std::uint64_t total = 0;
			for (const auto count : counts) {
				if (count) {
					total += std::atomic_ref<std::uint32_t>{ *count }.load(std::memory_order_relaxed);
				}
			}
			return total;
		}

		// The main thread's state (Frame, scan, poison).
		HavokNan::Cleanup cleanup;
		std::uint64_t     seenDeep = 0;
		Clock::time_point lastRoots{};
		Clock::time_point lastDeep{};

		// For status, from the channel's thread.
		std::mutex  lastLock;
		std::string lastFound = "none";
		std::atomic<std::uint64_t> looks{ 0 }, deepLooks{ 0 }, put{ 0 };

		bool Finite(const RE::NiTransform& a_transform)
		{
			const auto& r = a_transform.rotate.entry;
			const auto& t = a_transform.translate;
			const std::array<float, 13> values{ r[0].x, r[0].y, r[0].z, r[1].x, r[1].y, r[1].z, r[2].x, r[2].y, r[2].z, t.x, t.y, t.z, a_transform.scale };
			return HavokNan::Finite(values);
		}

		bool PlaceFinite(const RE::TESObjectREFR* a_ref)
		{
			const auto& d = a_ref->data;
			const std::array<float, 6> values{ d.location.x, d.location.y, d.location.z, d.angle.x, d.angle.y, d.angle.z };
			return HavokNan::Finite(values);
		}

		// The first node of a_root's tree (a_root itself when a_deep is false) whose world or local transform is not
		// finite; null when none is. A walk stops after NODE_LIMIT nodes.
		const RE::NiAVObject* BadNode(const RE::NiAVObject* a_root, bool a_deep)
		{
			if (!Finite(a_root->world) || !Finite(a_root->local)) {
				return a_root;
			}
			if (!a_deep) {
				return nullptr;
			}
			std::vector<const RE::NiAVObject*> stack{ a_root };
			std::size_t                        walked = 0;
			while (!stack.empty() && walked++ < HavokNan::NODE_LIMIT) {
				const auto node = stack.back();
				stack.pop_back();
				if (!Finite(node->world) || !Finite(node->local)) {
					return node;
				}
				if (const auto parent = const_cast<RE::NiAVObject*>(node)->IsNode()) {
					for (const auto& child : parent->children) {  // the array runs to its capacity: empty slots are null
						if (child.get()) {
							stack.push_back(child.get());
						}
					}
				}
			}
			return nullptr;
		}

		std::string Describe(const RE::TESObjectREFR* a_ref, const RE::NiAVObject* a_node)
		{
			const auto base = a_ref->GetObjectReference();
			const auto& at = a_ref->data.location;
			const char* name = a_node && a_node->name.c_str() ? a_node->name.c_str() : "";
			return std::format("{:08X} (base {:08X}, type {}) at ({:.0f}, {:.0f}, {:.0f}), node '{}'", a_ref->GetFormID(), base ? base->GetFormID() : 0,
				base ? static_cast<int>(base->GetFormType()) : -1, at.x, at.y, at.z, name);
		}

		bool Loading()
		{
			return Hud::LoadingScreenUp();
		}

		// One look at the loaded references; returns how many were found bad.
		int Look(bool a_deep, Clock::time_point a_now)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return 0;
			}
			++looks;
			if (a_deep) {
				++deepLooks;
			}
			struct Bad
			{
				HavokNan::Found found;
				std::string     text;
			};
			std::vector<Bad> bad;
			tes->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref->IsDeleted() || a_ref->IsDisabled()) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto root = a_ref->Get3D();
				const bool placeFinite = PlaceFinite(a_ref);
				const auto node = root ? BadNode(root, a_deep) : nullptr;
				if (node || !placeFinite) {
					const auto actor = a_ref->As<RE::Actor>();
					bad.push_back({ .found = { .refId = a_ref->GetFormID(),
										.player = a_ref->IsPlayerRef(),
										.ours = actor && Puppets::IsRegistered(actor),
										.placeFinite = placeFinite },
						.text = Describe(a_ref, node) + (placeFinite ? "" : ", its place not finite") });
				}
				return bad.size() < 64 ? RE::BSContainer::ForEachResult::kContinue : RE::BSContainer::ForEachResult::kStop;
			});
			for (const auto& one : bad) {
				const auto action = cleanup.Decide(one.found, a_now);
				if (action == HavokNan::Action::kNone) {
					continue;
				}
				REX::WARN("Havok NaN: {} has a transform that is not finite: {}", one.text, HavokNan::Name(action));
				if (action == HavokNan::Action::kReset || action == HavokNan::Action::kDisable) {
					RE::Console::ExecuteCommand(std::format("{:08X}.disable", one.found.refId).c_str());
					++put;
				}
				std::scoped_lock l{ lastLock };
				lastFound = std::format("{}: {}", one.text, HavokNan::Name(action));
			}
			return static_cast<int>(bad.size());
		}

		void EnableDue(Clock::time_point a_now)
		{
			for (const auto id : cleanup.TakeDue(a_now)) {
				const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(id);
				if (!ref || ref->IsDeleted()) {
					REX::WARN("Havok NaN: {:08X} is gone; not enabled again", id);
					continue;
				}
				RE::Console::ExecuteCommand(std::format("{:08X}.enable", id).c_str());
				REX::INFO("Havok NaN: {:08X} enabled again: its 3D and physics made again from its place", id);
			}
		}

		std::string Status()
		{
			std::string sites;
			for (std::size_t i = 0; i < counts.size(); ++i) {
				sites += counts[i] ? std::format(" {}", std::atomic_ref<std::uint32_t>{ *counts[i] }.load()) : " -"s;
			}
			std::scoped_lock l{ lastLock };
			return std::format("detours={}/{} cut={} (per copy:{}) looks={} deep={} put-right={} last={}", installed.load(), counts.size(), Counted(), sites, looks.load(),
				deepLooks.load(), put.load(), lastFound);
		}

		// scan [deep]: one look now, as Frame does.
		std::string Scan(std::string_view a_args)
		{
			if (Loading()) {
				return "error: a loading screen is up";
			}
			const bool deep = a_args != "roots";
			const auto found = Look(deep, Clock::now());
			return std::format("{} bad reference(s) found in a {} look; {}", found, deep ? "deep" : "root", Status());
		}

		// poison <refHex> [dynamic]: what the dump had, made on purpose. Every body under the reference's 3D gets a
		// rotation of NaN (its position kept), and the reference's physics is woken; `dynamic` first makes its bodies
		// dynamic (as a skeleton is once something has knocked it about). With a constraint
		// between two of them, the next physics step reaches the angle loop with a NaN: the game, as it was, hangs
		// there; with the detour the loop ends, is counted, and the reference is found and put right.
		std::string Poison(std::string_view a_args)
		{
			const auto    split = a_args.find(' ');
			const auto    hex = a_args.substr(0, split);
			const bool    dynamic = split != std::string_view::npos && a_args.substr(split + 1) == "dynamic";
			std::uint32_t id = 0;
			if (hex.empty() || std::from_chars(hex.data(), hex.data() + hex.size(), id, 16).ec != std::errc{}) {
				return "error: usage: havoknan poison <refHex> [dynamic]";
			}
			const auto ref = RE::TESForm::GetFormByID<RE::TESObjectREFR>(id);
			const auto root = ref ? ref->Get3D() : nullptr;
			if (!root) {
				return std::format("error: no loaded reference {:08X} with 3D", id);
			}
			if (ref->IsPlayerRef()) {
				return "error: not the player";
			}
			std::vector<RE::bhkNPCollisionObject*> collisions;
			std::vector<RE::NiAVObject*>           stack{ root };
			std::size_t                            walked = 0;
			while (!stack.empty() && walked++ < HavokNan::NODE_LIMIT) {
				const auto node = stack.back();
				stack.pop_back();
				if (const auto object = node->collisionObject.get()) {
					if (const auto collision = object->IsbhkNPCollisionObject()) {
						collisions.push_back(collision);
					}
				}
				if (const auto parent = node->IsNode()) {
					for (const auto& child : parent->children) {
						if (child.get()) {
							stack.push_back(child.get());
						}
					}
				}
			}
			int unmoving = 0;  // bodies with no motion of their own (static): motion id 0
			for (const auto collision : collisions) {
				if (const auto body = collision->GetBody()) {
					std::uint32_t motion = 0;
					std::memcpy(&motion, &body->m_motionId, sizeof(motion));
					unmoving += motion == 0 ? 1 : 0;
				}
				if (dynamic) {
					collision->SetMotionType(RE::hknpMotionPropertiesId::Preset::kDynamic);
				}
			}
			// The game's SetTransform turns a NaN rotation down (the body reads back as it was), so the NaN goes straight
			// into each body's own transform, where the next step's constraint setup reads it, and into the reference's
			// root node, as the dump had it (the root's local and world transforms NaN), which keeps it NaN after the step.
			const float           nan = std::numeric_limits<float>::quiet_NaN();
			std::array<float, 12> rotation{};
			rotation.fill(nan);
			int bodies = 0, nanAfter = 0;
			for (const auto collision : collisions) {
				const auto body = const_cast<RE::hknpBody*>(collision->GetBody());
				if (!body) {
					continue;
				}
				static_assert(sizeof(body->m_transform.m_rotation) == sizeof(rotation));
				std::memcpy(&body->m_transform.m_rotation, rotation.data(), sizeof(rotation));
				++bodies;
				alignas(16) RE::hkTransformf after{};
				std::array<float, 16>        read{};
				if (collision->GetTransform(after)) {
					std::memcpy(read.data(), &after, sizeof(read));
					nanAfter += HavokNan::Finite(read) ? 0 : 1;
				}
			}
			if (bodies == 0) {
				return std::format("error: {:08X} has no physics bodies", id);
			}
			for (auto& row : root->local.rotate.entry) {
				row.x = row.y = row.z = nan;
			}
			RE::bhkWorld::Activate(root, true, true, true);
			REX::WARN("Havok NaN: poison (dev): {} bod(ies) of {} given a NaN rotation ({} static before{}, {} read back NaN)", bodies, Describe(ref, root), unmoving,
				dynamic ? ", all made dynamic" : "", nanAfter);
			return std::format("poisoned {} bod(ies) of {:08X} ({} static before{}, {} read back NaN); cut so far {}", bodies, id, unmoving, dynamic ? ", all made dynamic" : "",
				nanAfter, Counted());
		}
	}

	bool Install()
	{
		static_assert(HavokNan::FUNCTION_ID == 2263389);  // the number spelled out for tools/addrcheck.ps1
		const auto function = REL::ID(2263389).address();
		auto&      trampoline = REL::GetTrampoline();
		for (std::size_t i = 0; i < HavokNan::SITES.size(); ++i) {
			const auto                                     site = function + HavokNan::SITES[i];
			std::array<std::uint8_t, HavokNan::SITE_BYTES.size()> bytes{};
			std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
			const auto targets = HavokNan::SiteMatches(bytes, site);
			if (!targets) {
				REX::WARN("Havok NaN: copy {} of the constraint setup's angle loop isn't as 1.11.240 has it (another plugin's patch?); left as it is", i + 1);
				continue;
			}
			constexpr std::size_t SIZE = HavokNan::DETOUR_SIZE + 16;  // room to put it on 16 (its count is a locked add)
			if (trampoline.free_size() < SIZE) {
				REX::WARN("Havok NaN: no room for the detour of copy {} ({} bytes free); left as it is", i + 1, trampoline.free_size());
				continue;
			}
			const auto memory = reinterpret_cast<std::uintptr_t>(trampoline.allocate(SIZE));
			const auto base = (memory + 15) & ~std::uintptr_t{ 15 };
			const auto detour = HavokNan::Detour(base, *targets);
			const auto patch = HavokNan::Patch(site + HavokNan::PATCH_AT, base);
			if (!detour || !patch) {
				REX::WARN("Havok NaN: the detour of copy {} is out of a jump's reach; left as it is", i + 1);
				continue;
			}
			std::memcpy(reinterpret_cast<void*>(base), detour->data(), detour->size());
			REX::W32::FlushInstructionCache(REX::W32::GetCurrentProcess(), reinterpret_cast<const void*>(base), detour->size());
			counts[i] = reinterpret_cast<std::uint32_t*>(base + HavokNan::DETOUR_COUNT);
			REL::WriteSafe(site + HavokNan::PATCH_AT, patch->data(), patch->size());
			++installed;
		}
		if (installed == 0) {
			return false;
		}
		REX::INFO("Havok NaN: the constraint setup's angle loop ends on a NaN ({} of {} copies)", installed.load(), HavokNan::SITES.size());
		return true;
	}

	void Frame()
	{
		const auto now = Clock::now();
		EnableDue(now);
		if (Loading()) {
			return;
		}
		const auto count = Counted();
		const auto plan = HavokNan::Plan(count, seenDeep, now - lastRoots, now - lastDeep);
		if (plan == HavokNan::Scan::kNone) {
			return;
		}
		lastRoots = now;
		const bool deep = plan == HavokNan::Scan::kDeep;
		if (deep) {
			REX::WARN("Havok NaN: {} NaN angle(s) cut in the constraint setup since the last look ({} in all); looking for the bodies", count - seenDeep, count);
			lastDeep = now;
			seenDeep = count;
		}
		const auto found = Look(deep, now);
		if (deep && found == 0) {
			REX::WARN("Havok NaN: no loaded reference with a 3D or place that is not finite");
		}
	}

	bool OnMainThread(std::string_view a_args)
	{
		return a_args.starts_with("scan") || a_args.starts_with("poison");
	}

	std::string Command(std::string_view a_args)
	{
		const auto split = a_args.find(' ');
		const auto verb = a_args.substr(0, split);
		const auto rest = split == std::string_view::npos ? std::string_view{} : a_args.substr(split + 1);
		if (verb.empty() || verb == "status") {
			return Status();
		}
		if (verb == "scan") {
			return Scan(rest);
		}
		if (verb == "poison") {
			return Poison(rest);
		}
		return "error: usage: havoknan [status | scan [roots] | poison <refHex> [dynamic]]";
	}
}
