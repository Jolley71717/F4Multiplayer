#pragma once

#include "RuntimeSupport.h"

// The runtime the plugin is in, and the engine's addresses on it. Every Address Library ID of our own is written
// REL::ID{ og, ng, ae }: one number per runtime, in that order, 0 where it is not known yet on that runtime. An ID of
// ours is never resolved by the library's own lookup (REL::ID::address(), REL::Relocation), which ends the game with a
// message box when this runtime has no value: always through Runtime::TryAddress, Runtime::Relocation or a feature's
// Runtime::Feature check. A feature with a value missing turns itself off, with one line in the log.
namespace Runtime
{
	using RuntimeSupport::Kind;

	// This game's runtime. Only called once the plugin is loaded (F4SE loads it on a supported runtime only).
	[[nodiscard]] Kind Current();

	// "old-gen 1.10.163" or "Anniversary 1.11.240", for the log.
	[[nodiscard]] std::string Describe();

	// This runtime has a value for a_id, and the installed Address Library knows it.
	[[nodiscard]] bool Has(const REL::ID& a_id);

	// The address of a_id on this runtime, or none (never the library's fatal lookup).
	[[nodiscard]] std::optional<std::uintptr_t> TryAddress(const REL::ID& a_id);

	// A part of the plugin that needs a set of engine addresses. On() says whether this runtime has every one of them;
	// the first time it hasn't, one line says what is off and the first ID missing. Every part asks before it runs.
	class Feature
	{
	public:
		Feature(std::string_view a_name, std::initializer_list<REL::ID> a_ids) :
			name(a_name),
			ids(a_ids)
		{}

		[[nodiscard]] bool On() const;

	private:
		std::string_view     name;
		std::vector<REL::ID> ids;
		mutable std::once_flag checked;
		mutable bool           on = false;
	};

	// Says, once per ID, that ID a_id was used on a runtime without it (by a part that should have asked its Feature
	// first). Used by Relocation below.
	void ReportMissing(std::uint64_t a_id);

	namespace detail
	{
		// What a Relocation without its address stands for: for a function, one that does nothing and returns zero; for
		// a global, zeroed memory. Never reached by a feature that asked its Feature first.
		template <class T>
		struct Neutral
		{
			static std::uintptr_t Address()
			{
				alignas(64) static std::array<std::byte, 0x400> zero{};
				return reinterpret_cast<std::uintptr_t>(zero.data());
			}
		};

		template <class R, class... A>
		struct Neutral<R (*)(A...)>
		{
			static R Call(A...)
			{
				if constexpr (!std::is_void_v<R>) {
					return R{};
				}
			}

			static std::uintptr_t Address() { return reinterpret_cast<std::uintptr_t>(&Call); }
		};
	}

	// REL::Relocation for one of our IDs: the same address on a runtime that has it, and on one that hasn't, a neutral
	// stand-in (see detail::Neutral) instead of the library's fatal lookup, and one line in the log once it is used (a
	// part off through its Feature never uses it, so only the Feature's line is written). T is a function pointer or a
	// pointer to a global, as with REL::Relocation.
	template <class T>
		requires std::is_pointer_v<T>
	class Relocation
	{
	public:
		explicit Relocation(const REL::ID& a_id)
		{
			if (const auto address = TryAddress(a_id)) {
				value = *address;
			} else {
				missing = a_id.id();
				value = detail::Neutral<T>::Address();
			}
		}

		[[nodiscard]] T get() const
		{
			Used();
			return reinterpret_cast<T>(value);
		}

		[[nodiscard]] std::uintptr_t address() const
		{
			Used();
			return value;
		}

		[[nodiscard]] decltype(auto) operator*() const
			requires(!std::is_function_v<std::remove_pointer_t<T>>)
		{
			return *get();
		}

		[[nodiscard]] T operator->() const
			requires(!std::is_function_v<std::remove_pointer_t<T>>)
		{
			return get();
		}

		template <class... Args>
		decltype(auto) operator()(Args&&... a_args) const
			requires std::is_function_v<std::remove_pointer_t<T>>
		{
			return get()(std::forward<Args>(a_args)...);
		}

	private:
		void Used() const
		{
			if (missing != 0) {
				ReportMissing(missing);
			}
		}

		std::uintptr_t value = 0;
		std::uint64_t  missing = 0;  // the ID, on a runtime without it
	};
}
