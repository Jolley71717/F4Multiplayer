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
		constexpr REL::ID     PROCESS_TEST{ 2231157 };    // the test of the actor's AI process before the blast (+CD5A30): (Actor*), true = no blast
		constexpr REL::ID     BODY_PART_DATA{ 2229571 };  // Actor::GetBodyPartData (+C59190)
		constexpr std::size_t CALL_STUB = 14;             // what a write_call<5> takes of the trampoline

		using TestFn = bool (*)(RE::Actor*);
		TestFn originalTest = nullptr;

		// Console commands run on the main thread; the counts are read from the dev channel's.
		std::atomic<bool>          installed{ false };
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
				REX::INFO("KillBlasts: {:08X}'s torso always explodes but has no limb value (body part data {:08X}, flags {:02X}): the console's kill leaves its blast out, which crashed the game{}",
					a_actor->GetFormID(), body.data, body.torso.flags, count == KillBlast::LOGGED ? " (the last one logged; net status counts them)" : "");
			}
			return true;
		}
	}

	bool Install()
	{
		if (installed) {
			return true;
		}
		auto& trampoline = REL::GetTrampoline();
		if (trampoline.free_size() < CALL_STUB) {
			REX::WARN("KillBlasts: no room for the hook ({} bytes free); a death is not landed on a creature the console's kill would crash on (a flying ant swarm)", trampoline.free_size());
			return false;
		}
		const auto                  site = KILL_COMMAND.address() + KillBlast::TEST_CALL;
		std::array<std::uint8_t, 5> bytes{};
		std::memcpy(bytes.data(), reinterpret_cast<const void*>(site), bytes.size());
		if (TextureFlush::CallTarget(bytes, site) != PROCESS_TEST.address()) {
			REX::WARN("KillBlasts: the console's kill doesn't call its test before the blast where 1.11.240 does (another plugin's patch?); left as it is, and a death is not landed on a creature the command would crash on (a flying ant swarm)");
			return false;
		}
		originalTest = reinterpret_cast<TestFn>(trampoline.write_call<5>(site, &TestHook));
		installed = true;
		REX::INFO("KillBlasts: the console's kill (+{:X}+{:X}) leaves out the blast of a torso with no limb value", KILL_COMMAND.offset(), KillBlast::TEST_CALL);
		return true;
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
		return std::format("killBlastGuard={} killBlastsSkipped={} killsRefused={}", installed ? "on" : "off", skipped.load(std::memory_order_relaxed), refused.load(std::memory_order_relaxed));
	}
}
