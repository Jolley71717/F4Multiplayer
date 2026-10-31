#include "Runtime.h"

namespace Runtime
{
	namespace
	{
		// The loaded Address Library's table, read without the library's lookup (which fails the game on a missing ID).
		// F4SE's databases are format 0, an array of { id, offset } sorted by id; IDDB keeps it to itself (Offset2ID is
		// its friend), so this names its accessor through a class derived from it.
		struct Table : REL::IDDB
		{
			static std::span<const RuntimeSupport::Mapping> Get()
			{
				static_assert(sizeof(REL::IDDB::MAPPING) == sizeof(RuntimeSupport::Mapping));
				static_assert(offsetof(REL::IDDB::MAPPING, offset) == offsetof(RuntimeSupport::Mapping, offset));
				constexpr auto accessor = &Table::get_id2offset<REL::IDDB::MAPPING>;
				const auto     table = (REL::IDDB::GetSingleton()->*accessor)();
				return { reinterpret_cast<const RuntimeSupport::Mapping*>(table.data()), table.size() };
			}
		};

		std::mutex                  missingLock;
		std::set<std::uint64_t>     missingReported;  // IDs ReportMissing has logged
	}

	Kind Current()
	{
		return static_cast<Kind>(REX::FModule::GetRuntimeIndex());
	}

	std::string Describe()
	{
		const auto version = REX::FModule::GetExecutingModule().GetFileVersion();
		return std::format("{} {}.{}.{}", RuntimeSupport::Name(Current()), version.major(), version.minor(), version.patch());
	}

	bool Has(const REL::ID& a_id)
	{
		return TryAddress(a_id).has_value();
	}

	std::optional<std::uintptr_t> TryAddress(const REL::ID& a_id)
	{
		const auto offset = RuntimeSupport::Find(Table::Get(), a_id.id());
		if (!offset) {
			return std::nullopt;
		}
		return REX::FModule::GetExecutingModule().GetBaseAddress() + static_cast<std::uintptr_t>(*offset);
	}

	bool Feature::On() const
	{
		std::call_once(checked, [this] {
			const auto missing = std::ranges::find_if(ids, [](const REL::ID& a_id) { return !Has(a_id); });
			on = missing == ids.end();
			if (!on) {
				const auto count = std::ranges::count_if(ids, [](const REL::ID& a_id) { return !Has(a_id); });
				REX::WARN("{}: off on {}: {} of its {} engine addresses are not known here (the first: ID {})", name, Describe(), count, ids.size(),
					missing->id());
			}
		});
		return on;
	}

	void ReportMissing(std::uint64_t a_id)
	{
		std::scoped_lock l{ missingLock };
		if (missingReported.insert(a_id).second) {
			REX::ERROR("Runtime: ID {} has no address on {}; what needs it does nothing here", a_id, Describe());
		}
	}
}
