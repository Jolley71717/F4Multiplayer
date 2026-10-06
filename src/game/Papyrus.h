#pragma once

// Calls Papyrus (game script) methods on game objects. Calls are queued to the script
// VM and run asynchronously, so they take effect a frame or so later.
//
// T must be the object's most derived script type (e.g. RE::Actor for actors), because
// the handle's type has to match the script the method is defined on.
namespace Papyrus
{
	template <class T, class... Args>
	bool CallMethod(T* a_object, std::string_view a_scriptName, std::string_view a_function, Args... a_args)
	{
		const auto game = RE::GameVM::GetSingleton();
		const auto vm = game ? game->GetVM() : nullptr;
		if (!vm || !a_object) {
			return false;
		}

		const auto& handles = vm->GetObjectHandlePolicy();
		const auto  handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<T>(), a_object);
		if (handle == handles.EmptyHandle()) {
			REX::WARN("Papyrus: no handle for {}.{}", a_scriptName, a_function);
			return false;
		}

		return vm->DispatchMethodCall(
			handle,
			RE::BSFixedString{ a_scriptName },
			RE::BSFixedString{ a_function },
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>{},
			a_args...);
	}
}
