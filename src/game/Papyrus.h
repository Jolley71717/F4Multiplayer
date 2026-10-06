#pragma once

// Calls Papyrus (game script) methods on game objects. Calls are queued to the script
// VM and run asynchronously, so they take effect a frame or so later.
//
// IVirtualMachine declares overloaded virtuals (CreateObject, DispatchMethodCall). MSVC lays
// overloads out in reverse declaration order, so calling them through CommonLibF4's class
// hits the neighbouring slot. We call the real slots explicitly instead.
namespace Papyrus
{
	namespace detail
	{
		using ArgsFn = RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<RE::BSScript::Variable>&)>;
		using Callback = RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>;

		// Slots from the game's IVirtualMachine vtable.
		constexpr std::size_t CREATE_OBJECT_SLOT = 0x17;          // CreateObject(name, out)
		constexpr std::size_t DISPATCH_METHOD_OBJECT_SLOT = 0x2D;  // DispatchMethodCall(object, ...)

		inline std::uintptr_t Slot(void* a_object, std::size_t a_index)
		{
			return (*reinterpret_cast<std::uintptr_t**>(a_object))[a_index];
		}

		inline bool CreateObject(RE::BSScript::IVirtualMachine* a_vm, const RE::BSFixedString& a_name, RE::BSTSmartPointer<RE::BSScript::Object>& a_out)
		{
			using fn_t = bool (*)(RE::BSScript::IVirtualMachine*, const RE::BSFixedString&, RE::BSTSmartPointer<RE::BSScript::Object>&);
			return reinterpret_cast<fn_t>(Slot(a_vm, CREATE_OBJECT_SLOT))(a_vm, a_name, a_out);
		}

		inline bool DispatchMethodCall(RE::BSScript::IVirtualMachine* a_vm, const RE::BSTSmartPointer<RE::BSScript::Object>& a_self, const RE::BSFixedString& a_function, const ArgsFn& a_args)
		{
			using fn_t = bool (*)(RE::BSScript::IVirtualMachine*, const RE::BSTSmartPointer<RE::BSScript::Object>&, const RE::BSFixedString&, const ArgsFn&, const Callback&);
			const Callback none{};
			return reinterpret_cast<fn_t>(Slot(a_vm, DISPATCH_METHOD_OBJECT_SLOT))(a_vm, a_self, a_function, a_args, none);
		}
	}

	// T must be the object's script type (e.g. RE::Actor for actors).
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

		// Objects created at runtime may not have a script object bound yet.
		const RE::BSFixedString                   scriptName{ a_scriptName };
		RE::BSTSmartPointer<RE::BSScript::Object> object;
		if (!vm->FindBoundObject(handle, scriptName.c_str(), false, object, false) || !object) {
			if (!detail::CreateObject(vm.get(), scriptName, object) || !object) {
				REX::WARN("Papyrus: could not create {} object", a_scriptName);
				return false;
			}
			vm->GetObjectBindPolicy().BindObject(object, handle);
		}

		const detail::ArgsFn args{ [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
			a_out = RE::BSScript::detail::PackVariables(a_args...);
			return true;
		} };
		return detail::DispatchMethodCall(vm.get(), object, RE::BSFixedString{ a_function }, args);
	}
}
