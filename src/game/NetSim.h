#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <format>
#include <random>
#include <string>

// Dev: a worse connection than the one we have, for friends' movement states only, so smoothing can be measured on a
// LAN rig as it is played over Steam's relays (dev command `netsim`). Each state is held back on its way in by a lag,
// plus a random jitter, plus now and then a spike (a relay route that hiccups); one held up holds up those behind it
// (a queue on the way, as a relay's is), and some are lost. Kept free of the game so it can be tested.
namespace NetSim
{
	using Clock = std::chrono::steady_clock;

	struct Settings
	{
		int   lagMs = 0;       // every state is this much later
		int   jitterMs = 0;    // plus up to this much more, at random
		float lossPercent = 0.0f;   // lost on the way
		int   spikeMs = 0;     // a spike holds a state up this much more,
		float spikePercent = 0.0f;  // this often
		bool  ordered = true;  // states come in the order they went (a held-up one holds up those behind it)

		[[nodiscard]] bool Active() const { return lagMs > 0 || jitterMs > 0 || lossPercent > 0.0f || (spikeMs > 0 && spikePercent > 0.0f); }

		[[nodiscard]] std::string Describe() const
		{
			if (!Active()) {
				return "off";
			}
			return std::format("{} ms lag, {} ms jitter, {:.1f}% loss, spikes of {} ms {:.1f}% of the time{}", lagMs, jitterMs, lossPercent, spikeMs, spikePercent, ordered ? "" : ", reordered");
		}
	};

	template <class T>
	class Line
	{
	public:
		explicit Line(unsigned a_seed = 1) :
			random(a_seed)
		{}

		Settings settings;

		// Takes an item that came at a_now; false when it is lost.
		bool Push(T a_item, Clock::time_point a_now)
		{
			std::uniform_real_distribution<float> unit{ 0.0f, 1.0f };
			if (settings.lossPercent > 0.0f && unit(random) * 100.0f < settings.lossPercent) {
				++lost;
				return false;
			}
			float ms = static_cast<float>(settings.lagMs) + unit(random) * static_cast<float>(settings.jitterMs);
			if (settings.spikeMs > 0 && settings.spikePercent > 0.0f && unit(random) * 100.0f < settings.spikePercent) {
				ms += static_cast<float>(settings.spikeMs);
				++spikes;
			}
			auto due = a_now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float, std::milli>(ms));
			if (settings.ordered && !queue.empty()) {
				due = (std::max)(due, queue.back().due);
			}
			queue.push_back({ due, std::move(a_item) });
			if (!settings.ordered) {
				std::stable_sort(queue.begin(), queue.end(), [](const Held& a_a, const Held& a_b) { return a_a.due < a_b.due; });
			}
			return true;
		}

		// Hands a_fn(item) each one due by a_now, in the order they come out.
		template <class Fn>
		void Release(Clock::time_point a_now, Fn&& a_fn)
		{
			while (!queue.empty() && queue.front().due <= a_now) {
				auto item = std::move(queue.front().item);
				queue.pop_front();
				a_fn(item);
			}
		}

		// Everything at once (netsim turned off).
		template <class Fn>
		void Flush(Fn&& a_fn)
		{
			while (!queue.empty()) {
				auto item = std::move(queue.front().item);
				queue.pop_front();
				a_fn(item);
			}
		}

		[[nodiscard]] std::size_t Waiting() const { return queue.size(); }

		std::uint64_t lost = 0;
		std::uint64_t spikes = 0;

	private:
		struct Held
		{
			Clock::time_point due;
			T                 item;
		};
		std::deque<Held> queue;
		std::mt19937     random;
	};
}
