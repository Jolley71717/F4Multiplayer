#include "game/KillBlasts.h"

#include "game/KillBlast.h"
#include "game/TextureFlush.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>

namespace KillBlasts
{
	namespace
	{
		// 1.11.240, by Address Library ID (KillBlast.h: how each was read).
		constexpr REL::ID     KILL_COMMAND{ 2204420 };    // the console's kill (+5C03D0): calls the test at +0x160, then blasts the torso
		constexpr REL::ID     SCRIPT_KILL{ 2251955 };     // Papyrus's Actor.Kill and KillSilent (+1108D30): the same test at +0xE4, the same blast
		constexpr REL::ID     PROCESS_TEST{ 2231157 };    // the test of the actor's AI process before the blast (+CD5A30): (Actor*), true = no blast
		constexpr REL::ID     BODY_PART_DATA{ 2229571 };  // Actor::GetBodyPartData (+C59190)
		constexpr std::size_t CALL_STUB = 14;             // what a write_call<5> takes of the trampoline

		using TestFn = bool (*)(RE::Actor*);
		TestFn originalTest = nullptr;

		// Console commands run on the main thread; the counts are read from the dev channel's.
		std::atomic<bool>          installed{ false };        // the console's kill
		std::atomic<bool>          scriptInstalled{ false };  // Papyrus's
		std::atomic<std::uint32_t> skipped{ 0 };
		std::atomic<std::uint32_t> refused{ 0 };

		struct Body
		{
			KillBlast::Torso torso;
			std::uint32_t    data = 0;  // the body part data's form
		};

		// The torso the kill command would read, the way it reads it.
		Body BodyOf(RE::Actor* a_actor)
		{
			static REL::Relocation<RE::BGSBodyPartData* (*)(RE::Actor*)> bodyPartData{ BODY_PART_DATA };
			const auto data = a_actor ? bodyPartData(a_actor) : nullptr;
			const auto part = data ? reinterpret_cast<const std::uint8_t*>(data->partArray[RE::BGSBodyPartData::PartType::Torso]) : nullptr;
			if (!part) {
				return { .torso = { .present = false } };
			}
			const void* value = nullptr;
			std::memcpy(&value, part + KillBlast::PART_VALUE, sizeof(value));
			return { .torso = { .flags = part[KillBlast::PART_FLAGS], .value = value != nullptr }, .data = data->GetFormID() };
		}

		// In place of the command's test: true leaves the blast out.
		bool TestHook(RE::Actor* a_actor)
		{
			const auto body = BodyOf(a_actor);
			if (KillBlast::Judge(body.torso) == KillBlast::Verdict::kEngine) {
				return originalTest(a_actor);
			}
			const auto count = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
			if (count <= KillBlast::LOGGED) {
				REX::INFO("KillBlasts: {:08X}'s torso always explodes but has no limb value (body part data {:08X}, flags {:02X}): the kill (console or script) leaves its blast out, which crashed the game{}",
					a_actor->GetFormID(), body.data, body.torso.flags, count == KillBlast::LOGGED ? " (the last one logged; net status counts them)" : "");
			}
			return true;
		}
	}

	namespace
	{
		// One call of the AI-process test, checked to be 1.11.240's, through the hook. Both sites call the same test, so
		// the original kept from either is the other's too.
		bool HookSite(const REL::ID& a_function, std::size_t a_call, const char* a_what)
		{
			const auto                  site = a_function.address() + a_call;
			std::array<std::uint8_t, 5> bytes{};
			std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
			if (TextureFlush::CallTarget(bytes, site) != PROCESS_TEST.address()) {
				REX::WARN("KillBlasts: {} doesn't call its test before the blast where 1.11.240 does (another plugin's patch?); left as it is", a_what);
				return false;
			}
			const auto original = reinterpret_cast<TestFn>(REL::GetTrampoline().write_call<5>(site, &TestHook));
			if (!originalTest) {
				originalTest = original;
			}
			REX::INFO("KillBlasts: {} (+{:X}+{:X}) leaves out the blast of a torso with no limb value", a_what, a_function.offset(), a_call);
			return true;
		}
	}

	bool Install()
	{
		if (installed && scriptInstalled) {
			return true;
		}
		// The two calls share one stub (the same hook).
		if (!installed && !scriptInstalled && REL::GetTrampoline().free_size() < CALL_STUB) {
			REX::WARN("KillBlasts: no room for the hook ({} bytes free); a death is not landed on a creature the console's kill would crash on (a flying ant swarm)", REL::GetTrampoline().free_size());
			return false;
		}
		if (!installed && HookSite(KILL_COMMAND, KillBlast::TEST_CALL, "the console's kill")) {
			installed = true;
		} else if (!installed) {
			REX::WARN("KillBlasts: so a death is not landed on a creature the console's kill would crash on (a flying ant swarm)");
		}
		if (!scriptInstalled && HookSite(SCRIPT_KILL, KillBlast::SCRIPT_TEST_CALL, "Papyrus's Actor.Kill")) {
			scriptInstalled = true;
		}
		return installed;
	}

	bool Refused(RE::Actor* a_actor)
	{
		if (installed.load(std::memory_order_relaxed)) {
			return false;
		}
		const auto body = BodyOf(a_actor);
		if (!KillBlast::Refused(false, body.torso)) {
			return false;
		}
		const auto count = refused.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count <= KillBlast::LOGGED) {
			REX::WARN("KillBlasts: {:08X} is not killed here: the console's kill would crash the game on it (its torso always explodes but has no limb value, body part data {:08X}) and the hook that mends it is not in{}",
				a_actor->GetFormID(), body.data, count == KillBlast::LOGGED ? " (the last one logged; net status counts them)" : "");
		}
		return true;
	}

	std::string Describe()
	{
		return std::format("killBlastGuard={}{} killBlastsSkipped={} killsRefused={}", installed ? "on" : "off", scriptInstalled ? "+script" : "", skipped.load(std::memory_order_relaxed), refused.load(std::memory_order_relaxed));
	}
}
