#include "game/ConsolePlace.h"

#include "game/Spawns.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>

namespace ConsolePlace
{
	namespace
	{
		using Execute = RE::SCRIPT_FUNCTION::ExecuteFunction_t;

		struct Command
		{
			const char*                function;  // as the engine names it
			Execute*                   original = nullptr;
			std::atomic<std::uint32_t> ran{ 0 };
		};
		// The console's commands that place an actor at a reference. One wrapper each (Wrapper<N>), as each keeps the
		// engine's own function to call.
		std::array<Command, 2> commands{ Command{ "PlaceAtMe" }, Command{ "PlaceLeveledActorAtMe" } };
		std::atomic_bool       installed{ false };

		template <std::size_t N>
		bool Wrapper(const RE::SCRIPT_PARAMETER* a_parameters, const char* a_compiledParams, RE::TESObjectREFR* a_refObject, RE::TESObjectREFR* a_container, RE::Script* a_script,
			RE::ScriptLocals* a_scriptLocals, float& a_returnValue, std::uint32_t& a_offset)
		{
			auto& command = commands[N];
			++command.ran;
			// Before the engine's function: where the reference stands now is where its NPC is put.
			Spawns::OnConsolePlace(a_refObject);
			return command.original(a_parameters, a_compiledParams, a_refObject, a_container, a_script, a_scriptLocals, a_returnValue, a_offset);
		}

		template <std::size_t N>
		void Hook()
		{
			auto&      command = commands[N];
			const auto function = RE::SCRIPT_FUNCTION::LocateScriptCommand(command.function);
			if (!function || !function->executeFunction) {
				REX::WARN("ConsolePlace: the console's {} was not found in the engine's table: NPCs placed with it are shared only while they fight", command.function);
				return;
			}
			command.original = function->executeFunction;
			REL::WriteSafeData(reinterpret_cast<std::uintptr_t>(std::addressof(function->executeFunction)), reinterpret_cast<std::uintptr_t>(&Wrapper<N>));
			REX::INFO("ConsolePlace: watching the console's {}", command.function);
		}
	}

	void Install()
	{
		if (installed.exchange(true)) {
			return;
		}
		Hook<0>();
		Hook<1>();
	}

	std::string Describe()
	{
		return std::format("consolePlace: placeAtMe={}/{} placeLeveled={}/{}", commands[0].original ? "watched" : "no", commands[0].ran.load(), commands[1].original ? "watched" : "no",
			commands[1].ran.load());
	}
}
