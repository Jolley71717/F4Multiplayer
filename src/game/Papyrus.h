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
		constexpr std::size_t CREATE_STRUCT_SLOT = 0x18;          // CreateStruct(name, out)
		constexpr std::size_t DISPATCH_STATIC_SLOT = 0x2C;         // DispatchStaticCall(script, function, ...)
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

		inline bool CreateStruct(RE::BSScript::IVirtualMachine* a_vm, const RE::BSFixedString& a_name, RE::BSTSmartPointer<RE::BSScript::Struct>& a_out)
		{
			using fn_t = bool (*)(RE::BSScript::IVirtualMachine*, const RE::BSFixedString&, RE::BSTSmartPointer<RE::BSScript::Struct>&);
			return reinterpret_cast<fn_t>(Slot(a_vm, CREATE_STRUCT_SLOT))(a_vm, a_name, a_out);
		}

		inline bool DispatchMethodCall(RE::BSScript::IVirtualMachine* a_vm, const RE::BSTSmartPointer<RE::BSScript::Object>& a_self, const RE::BSFixedString& a_function, const ArgsFn& a_args)
		{
			using fn_t = bool (*)(RE::BSScript::IVirtualMachine*, const RE::BSTSmartPointer<RE::BSScript::Object>&, const RE::BSFixedString&, const ArgsFn&, const Callback&);
			const Callback none{};
			return reinterpret_cast<fn_t>(Slot(a_vm, DISPATCH_METHOD_OBJECT_SLOT))(a_vm, a_self, a_function, a_args, none);
		}

		inline bool DispatchStaticCall(RE::BSScript::IVirtualMachine* a_vm, const RE::BSFixedString& a_script, const RE::BSFixedString& a_function, const ArgsFn& a_args, const Callback& a_callback)
		{
			using fn_t = bool (*)(RE::BSScript::IVirtualMachine*, const RE::BSFixedString&, const RE::BSFixedString&, const ArgsFn&, const Callback&);
			return reinterpret_cast<fn_t>(Slot(a_vm, DISPATCH_STATIC_SLOT))(a_vm, a_script, a_function, a_args, a_callback);
		}
	}

	// The script object bound to a game object (T its script type, e.g. RE::Actor for actors), or null.
	// a_create: make a script object for the reference if none is bound (needed for references made
	// at runtime). Leave it off for calls that may hit many references: creating an object inside
	// the VM has crashed the game (BSScript VirtualMachine::CreateObject) on a mirrored NPC.
	template <class T>
	RE::BSTSmartPointer<RE::BSScript::Object> BoundObject(T* a_object, std::string_view a_scriptName, bool a_create)
	{
		const auto game = RE::GameVM::GetSingleton();
		const auto vm = game ? game->GetVM() : nullptr;
		if (!vm || !a_object) {
			return {};
		}

		const auto& handles = vm->GetObjectHandlePolicy();
		const auto  handle = handles.GetHandleForObject(RE::BSScript::GetVMTypeID<T>(), a_object);
		if (handle == handles.EmptyHandle()) {
			REX::WARN("Papyrus: no handle for a {}", a_scriptName);
			return {};
		}

		// Objects created at runtime may not have a script object bound yet.
		const RE::BSFixedString                   scriptName{ a_scriptName };
		RE::BSTSmartPointer<RE::BSScript::Object> object;
		if (!vm->FindBoundObject(handle, scriptName.c_str(), false, object, false) || !object) {
			if (!a_create) {
				return {};
			}
			if (!detail::CreateObject(vm.get(), scriptName, object) || !object) {
				REX::WARN("Papyrus: could not create {} object", a_scriptName);
				return {};
			}
			vm->GetObjectBindPolicy().BindObject(object, handle);
		}
		return object;
	}

	// A new struct of a script's (a_name as "Script#Struct"), its fields at their defaults; null if the VM has no such
	// struct (the script missing, say).
	inline RE::BSTSmartPointer<RE::BSScript::Struct> NewStruct(std::string_view a_name)
	{
		const auto game = RE::GameVM::GetSingleton();
		const auto vm = game ? game->GetVM() : nullptr;
		RE::BSTSmartPointer<RE::BSScript::Struct> out;
		if (!vm || !detail::CreateStruct(vm.get(), RE::BSFixedString{ a_name }, out)) {
			return {};
		}
		return out;
	}

	// A struct's field by name, or null if it has none by that name.
	inline RE::BSScript::Variable* Field(RE::BSScript::Struct& a_struct, std::string_view a_name)
	{
		if (!a_struct.type) {
			return nullptr;
		}
		const auto& names = a_struct.type->varNameIndexMap;
		const auto  it = names.find(RE::BSFixedString{ a_name });
		return it != names.end() && it->second < a_struct.type->variables.size() ? &a_struct.variables[it->second] : nullptr;
	}

	// Calls a script's global function (a plugin's native one, say) with the arguments a_args fills in. a_callback,
	// if any, is told the result, or that the call was cancelled, later on one of the VM's threads. False if the
	// call could not be queued.
	inline bool CallStatic(std::string_view a_script, std::string_view a_function, const std::function<void(RE::BSScrapArray<RE::BSScript::Variable>&)>& a_args, const detail::Callback& a_callback = {})
	{
		const auto game = RE::GameVM::GetSingleton();
		const auto vm = game ? game->GetVM() : nullptr;
		if (!vm) {
			return false;
		}
		const detail::ArgsFn args{ [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
			a_args(a_out);
			return true;
		} };
		return detail::DispatchStaticCall(vm.get(), RE::BSFixedString{ a_script }, RE::BSFixedString{ a_function }, args, a_callback);
	}

	// Calls a method on a script object (from BoundObject) with the arguments a_args fills in: script objects among them,
	// which the VM's own packing of a form would make through the wrong slot (see above). False if it could not be queued.
	inline bool CallOn(const RE::BSTSmartPointer<RE::BSScript::Object>& a_self, std::string_view a_function, const std::function<void(RE::BSScrapArray<RE::BSScript::Variable>&)>& a_args)
	{
		const auto game = RE::GameVM::GetSingleton();
		const auto vm = game ? game->GetVM() : nullptr;
		if (!vm || !a_self) {
			return false;
		}
		const detail::ArgsFn args{ [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
			a_args(a_out);
			return true;
		} };
		return detail::DispatchMethodCall(vm.get(), a_self, RE::BSFixedString{ a_function }, args);
	}

	// T must be the object's script type (e.g. RE::Actor for actors). a_create: see BoundObject.
	template <class T, class... Args>
	bool CallMethod(T* a_object, std::string_view a_scriptName, std::string_view a_function, bool a_create, Args... a_args)
	{
		const auto game = RE::GameVM::GetSingleton();
		const auto vm = game ? game->GetVM() : nullptr;
		const auto object = BoundObject(a_object, a_scriptName, a_create);
		if (!vm || !object) {
			return false;
		}

		const detail::ArgsFn args{ [&](RE::BSScrapArray<RE::BSScript::Variable>& a_out) {
			a_out = RE::BSScript::detail::PackVariables(a_args...);
			return true;
		} };
		return detail::DispatchMethodCall(vm.get(), object, RE::BSFixedString{ a_function }, args);
	}
}
