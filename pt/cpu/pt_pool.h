// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson
#pragma once

#include "pt_ns.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <immintrin.h>
#include <mutex>
#include <thread>
#include <vector>

namespace PT_NS {

// Runs "for i in [0, count)" across all cores; the caller takes part.
//
// A frame is a burst of several short runs, so a worker that has just
// finished one spins for a moment before going to sleep: waking a sleeping
// thread costs more than many of these runs take.
class Pool
{
public:
	Pool()
	{
		unsigned n = std::thread::hardware_concurrency();
		if (n < 1) n = 1;
		for (unsigned i = 1; i < n; i++)
			workers_.emplace_back([this, i] { Worker((int)i); });
	}

	~Pool()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			quit_ = true;
			generation_.fetch_add(1);
		}
		wake_.notify_all();
		for (std::thread &t : workers_)
			t.join();
	}

	int Threads() const { return (int)workers_.size() + 1; }

	// how many threads take part in a run, the caller included; 0 = all
	void SetLimit(int threads) { limit_.store(threads <= 0 ? 1 << 30 : threads); }

	void Run(int count, const std::function<void(int)> &job)
	{
		if (count <= 0)
			return;

		job_ = &job;
		count_ = count;
		next_.store(0);
		busy_.store((int)workers_.size());
		{
			// under the lock so a worker about to sleep cannot miss it
			std::lock_guard<std::mutex> lock(mutex_);
			generation_.fetch_add(1);
		}
		if (sleepers_.load() > 0)
			wake_.notify_all();

		Drain();

		// the others are at most one item from done
		while (busy_.load() > 0)
			_mm_pause();
		job_ = nullptr;
	}

private:
	void Drain()
	{
		for (;;)
		{
			const int i = next_.fetch_add(1);
			if (i >= count_)
				return;
			(*job_)(i);
		}
	}

	// true once there is a new generation; spins first, then sleeps
	bool WaitForWork(unsigned seen)
	{
		const auto give_up = std::chrono::steady_clock::now() + std::chrono::microseconds(300);
		for (int spins = 0; ; spins++)
		{
			if (generation_.load() != seen)
				return true;
			_mm_pause();
			if ((spins & 255) == 255 && std::chrono::steady_clock::now() > give_up)
				break;
		}

		std::unique_lock<std::mutex> lock(mutex_);
		sleepers_.fetch_add(1);
		wake_.wait(lock, [&] { return generation_.load() != seen; });
		sleepers_.fetch_sub(1);
		return true;
	}

	void Worker(int id)	// ids start at 1; the caller is thread 0
	{
		unsigned seen = 0;
		for (;;)
		{
			WaitForWork(seen);
			seen = generation_.load();
			if (quit_)
				return;
			if (id < limit_.load())
				Drain();
			busy_.fetch_sub(1);
		}
	}

	std::vector<std::thread>	workers_;
	std::mutex					mutex_;
	std::condition_variable		wake_;
	const std::function<void(int)> *job_ = nullptr;
	std::atomic<int>			next_{0};
	std::atomic<int>			busy_{0};
	std::atomic<int>			sleepers_{0};
	std::atomic<int>			limit_{1 << 30};
	std::atomic<unsigned>		generation_{0};
	int							count_ = 0;
	std::atomic<bool>			quit_{false};
};

} // namespace PT_NS
