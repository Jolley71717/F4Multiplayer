#include "Log.h"

#include "LogRotation.h"
#include "Redact.h"

#include <ShlObj.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <mutex>

// wingdi.h defines ERROR, which collides with REX::ERROR.
#undef ERROR

namespace Log
{
	namespace
	{
		constexpr int KEEP = 3;  // F4Multiplayer.1.log to F4Multiplayer.3.log

		std::filesystem::path path;

		// Hands every line to the sinks CommonLib made (the file, the debugger) with Steam ids and
		// addresses scrubbed out.
		class ScrubSink final : public spdlog::sinks::base_sink<std::mutex>
		{
		public:
			explicit ScrubSink(std::vector<spdlog::sink_ptr> a_sinks) :
				sinks(std::move(a_sinks))
			{}

		protected:
			void sink_it_(const spdlog::details::log_msg& a_msg) override
			{
				const auto clean = Redact::Scrub(std::string_view{ a_msg.payload.data(), a_msg.payload.size() });
				auto       copy = a_msg;
				copy.payload = clean;
				for (const auto& sink : sinks) {
					if (sink->should_log(copy.level)) {
						sink->log(copy);
					}
				}
			}

			void flush_() override
			{
				for (const auto& sink : sinks) {
					sink->flush();
				}
			}

		private:
			std::vector<spdlog::sink_ptr> sinks;
		};
	}

	std::error_code RotateBeforeInit(std::string_view a_saveFolder)
	{
		// The same path CommonLib opens (F4SE::Init's InitLog): Documents/My Games/<save folder>/F4SE/<plugin>.log.
		PWSTR docs = nullptr;
		if (a_saveFolder.empty() || FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			return {};
		}
		std::filesystem::path folder{ docs };
		CoTaskMemFree(docs);
		const auto data = F4SE::PluginVersionData::GetSingleton();
		path = folder / "My Games" / a_saveFolder / "F4SE" / std::format("{}.log", data ? data->GetPluginName() : "F4Multiplayer"sv);
		return LogRotation::Rotate(path, KEEP);
	}

	void InstallScrubber()
	{
		const auto logger = spdlog::default_logger();
		if (!logger) {
			return;
		}
		auto scrub = std::make_shared<ScrubSink>(logger->sinks());
		logger->sinks() = { std::move(scrub) };
	}

	bool SetLevel(std::string_view a_level)
	{
		if (a_level != "info" && a_level != "debug") {
			return false;
		}
		spdlog::default_logger_raw()->set_level(a_level == "debug" ? spdlog::level::debug : spdlog::level::info);
		return true;
	}

	std::string_view Level()
	{
		const auto level = spdlog::default_logger_raw()->level();
		return level <= spdlog::level::debug ? "debug"sv : "info"sv;
	}

	void Flush()
	{
		spdlog::default_logger_raw()->flush();
	}

	const std::filesystem::path& Path()
	{
		return path;
	}
}
