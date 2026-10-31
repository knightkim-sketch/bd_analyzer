// bda_ltm_parallel.cpp - thread pool behind bda_ltm_parallel_for() (see bda_ltm_parallel.h).

#include "bda_ltm_parallel.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Set on pool threads and while the calling thread runs chunks: nested loops run serially.
thread_local bool t_in_parallel = false;

// Sets t_in_parallel for a scope, restoring it even when a chunk throws.
struct InParallel {
	const bool saved = t_in_parallel;
	InParallel() { t_in_parallel = true; }
	~InParallel() { t_in_parallel = saved; }
};

unsigned configured_threads() {
	if (const char *env = std::getenv("BDA_LTM_THREADS")) {
		const long n = std::strtol(env, nullptr, 10);
		if (n >= 1)
			return (unsigned)std::min(n, 64L);
	}
	const unsigned hw = std::thread::hardware_concurrency();
	return std::max(1u, std::min(hw ? hw : 1u, 16u));
}

// One parallel loop. Every counter lives here, not in the pool: a worker that is still leaving an
// earlier job can only ever claim chunks of that job (all taken), never a chunk of the next one.
struct Job {
	const std::function<void(unsigned)> *chunk = nullptr; // valid while some chunk is unfinished
	unsigned chunks = 0;
	std::atomic<unsigned> next{0};
	std::mutex mutex;
	std::condition_variable done;
	unsigned pending = 0;
	std::exception_ptr error;

	// Run chunks until none is left to claim.
	void work() {
		for (;;) {
			const unsigned i = next.fetch_add(1);
			if (i >= chunks)
				return;
			std::exception_ptr e;
			try {
				(*chunk)(i);
			} catch (...) {
				e = std::current_exception();
			}
			std::lock_guard<std::mutex> lock(mutex);
			if (e && !error)
				error = e;
			if (--pending == 0)
				done.notify_all();
		}
	}
};

class Pool {
public:
	explicit Pool(unsigned threads) {
		// A thread that cannot be created (process thread limit) leaves the pool smaller.
		try {
			for (unsigned i = 1; i < threads; ++i)
				workers_.emplace_back([this] { serve(); });
		} catch (...) {
		}
	}

	~Pool() {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stop_ = true;
		}
		wake_.notify_all();
		for (auto &t : workers_)
			t.join();
	}

	unsigned threads() const { return (unsigned)workers_.size() + 1; }

	void run(unsigned chunks, const std::function<void(unsigned)> &chunk) {
		auto job = std::make_shared<Job>();
		job->chunk = &chunk;
		job->chunks = chunks;
		job->pending = chunks;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			job_ = job;
		}
		wake_.notify_all();

		{
			InParallel in_parallel;
			job->work();
		}
		{
			std::unique_lock<std::mutex> lock(job->mutex);
			job->done.wait(lock, [&] { return job->pending == 0; });
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (job_ == job)
				job_.reset();
		}
		if (job->error)
			std::rethrow_exception(job->error);
	}

private:
	void serve() {
		t_in_parallel = true;
		std::shared_ptr<Job> last;
		for (;;) {
			std::shared_ptr<Job> job;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				wake_.wait(lock, [&] { return stop_ || (job_ && job_ != last); });
				if (stop_)
					return;
				job = job_;
			}
			last = job;
			job->work();
		}
	}

	std::vector<std::thread> workers_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::shared_ptr<Job> job_; // the loop workers should join, if any
	bool stop_ = false;
};

Pool &pool() {
	static Pool p(configured_threads());
	return p;
}

// Several decoder handles (or threads of the host) may start loops at the same time; they take turns.
std::mutex g_run_mutex;

} // namespace

unsigned bda_ltm_parallel_threads() { return pool().threads(); }

void bda_ltm_parallel_for(unsigned count, unsigned min_chunk, const std::function<void(unsigned, unsigned)> &fn) {
	if (count == 0)
		return;
	const unsigned threads = t_in_parallel ? 1 : pool().threads();
	min_chunk = std::max(1u, min_chunk);
	// A few chunks per thread so uneven rows still balance.
	unsigned chunks = std::min(count / min_chunk, threads * 4);
	if (threads == 1 || chunks <= 1) {
		InParallel in_parallel;
		fn(0, count);
		return;
	}
	const unsigned size = (count + chunks - 1) / chunks;
	chunks = (count + size - 1) / size;
	const std::function<void(unsigned)> chunk = [&](unsigned i) { fn(i * size, std::min(count, (i + 1) * size)); };
	std::lock_guard<std::mutex> lock(g_run_mutex);
	pool().run(chunks, chunk);
}
