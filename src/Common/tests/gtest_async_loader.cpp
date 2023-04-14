#include <gtest/gtest.h>

#include <barrier>
#include <chrono>
#include <mutex>
#include <vector>
#include <thread>
#include <pcg_random.hpp>

#include <base/types.h>
#include <base/sleep.h>
#include <Common/Exception.h>
#include <Common/AsyncLoader.h>
#include <Common/randomSeed.h>


using namespace DB;


namespace CurrentMetrics
{
    extern const Metric TablesLoaderThreads;
    extern const Metric TablesLoaderThreadsActive;
}

namespace DB::ErrorCodes
{
    extern const int ASYNC_LOAD_SCHEDULE_FAILED;
    extern const int ASYNC_LOAD_FAILED;
    extern const int ASYNC_LOAD_CANCELED;
    extern const int ASYNC_LOAD_DEPENDENCY_FAILED;
}

struct AsyncLoaderTest
{
    AsyncLoader loader;

    std::mutex rng_mutex;
    pcg64 rng{randomSeed()};

    explicit AsyncLoaderTest(size_t max_threads = 1)
        : loader(CurrentMetrics::TablesLoaderThreads, CurrentMetrics::TablesLoaderThreadsActive, max_threads)
    {}

    template <typename T>
    T randomInt(T from, T to)
    {
        std::uniform_int_distribution<T> distribution(from, to);
        std::scoped_lock lock(rng_mutex);
        return distribution(rng);
    }

    void randomSleepUs(UInt64 min_us, UInt64 max_us, int probability_percent)
    {
        if (randomInt(0, 99) < probability_percent)
            std::this_thread::sleep_for(std::chrono::microseconds(randomInt(min_us, max_us)));
    }

    template <typename JobFunc>
    LoadJobSet randomJobSet(int job_count, int dep_probability_percent, JobFunc job_func, std::string_view name_prefix = "job")
    {
        std::vector<LoadJobPtr> jobs;
        for (int j = 0; j < job_count; j++)
        {
            LoadJobSet deps;
            for (int d = 0; d < j; d++)
            {
                if (randomInt(0, 99) < dep_probability_percent)
                    deps.insert(jobs[d]);
            }
            jobs.push_back(makeLoadJob(std::move(deps), fmt::format("{}{}", name_prefix, j), job_func));
        }
        return {jobs.begin(), jobs.end()};
    }

    template <typename JobFunc>
    LoadJobSet chainJobSet(int job_count, JobFunc job_func, std::string_view name_prefix = "job")
    {
        std::vector<LoadJobPtr> jobs;
        jobs.push_back(makeLoadJob({}, fmt::format("{}{}", name_prefix, 0), job_func));
        for (int j = 1; j < job_count; j++)
            jobs.push_back(makeLoadJob({ jobs[j - 1] }, fmt::format("{}{}", name_prefix, j), job_func));
        return {jobs.begin(), jobs.end()};
    }
};

TEST(AsyncLoader, Smoke)
{
    AsyncLoaderTest t(2);

    static constexpr ssize_t low_priority = -1;

    std::atomic<size_t> jobs_done{0};
    std::atomic<size_t> low_priority_jobs_done{0};

    auto job_func = [&] (const LoadJob & self) {
        jobs_done++;
        if (self.priority == low_priority)
            low_priority_jobs_done++;
    };

    {
        auto job1 = makeLoadJob({}, "job1", job_func);
        auto job2 = makeLoadJob({ job1 }, "job2", job_func);
        auto task1 = t.loader.schedule({ job1, job2 });

        auto job3 = makeLoadJob({ job2 }, "job3", job_func);
        auto job4 = makeLoadJob({ job2 }, "job4", job_func);
        auto task2 = t.loader.schedule({ job3, job4 });
        auto job5 = makeLoadJob({ job3, job4 }, "job5", job_func);
        task2.merge(t.loader.schedule({ job5 }, low_priority));

        std::thread waiter_thread([=] { job5->wait(); });

        t.loader.start();

        job3->wait();
        t.loader.wait();
        job4->wait();

        waiter_thread.join();

        ASSERT_EQ(job1->status(), LoadStatus::SUCCESS);
        ASSERT_EQ(job2->status(), LoadStatus::SUCCESS);
    }

    ASSERT_EQ(jobs_done, 5);
    ASSERT_EQ(low_priority_jobs_done, 1);

    t.loader.stop();
}

TEST(AsyncLoader, CycleDetection)
{
    AsyncLoaderTest t;

    auto job_func = [&] (const LoadJob &) {};

    LoadJobPtr cycle_breaker; // To avoid memleak we introduce with a cycle

    try
    {
        std::vector<LoadJobPtr> jobs;
        jobs.push_back(makeLoadJob({}, "job0", job_func));
        jobs.push_back(makeLoadJob({ jobs[0] }, "job1", job_func));
        jobs.push_back(makeLoadJob({ jobs[0], jobs[1] }, "job2", job_func));
        jobs.push_back(makeLoadJob({ jobs[0], jobs[2] }, "job3", job_func));

        // Actually it is hard to construct a cycle, but suppose someone was able to succeed violating constness
        const_cast<LoadJobSet &>(jobs[1]->dependencies).insert(jobs[3]);
        cycle_breaker = jobs[1];

        // Add couple unrelated jobs
        jobs.push_back(makeLoadJob({ jobs[1] }, "job4", job_func));
        jobs.push_back(makeLoadJob({ jobs[4] }, "job5", job_func));
        jobs.push_back(makeLoadJob({ jobs[3] }, "job6", job_func));
        jobs.push_back(makeLoadJob({ jobs[1], jobs[2], jobs[3], jobs[4], jobs[5], jobs[6] }, "job7", job_func));

        // Also add another not connected jobs
        jobs.push_back(makeLoadJob({}, "job8", job_func));
        jobs.push_back(makeLoadJob({}, "job9", job_func));
        jobs.push_back(makeLoadJob({ jobs[9] }, "job10", job_func));

        auto task1 = t.loader.schedule({ jobs.begin(), jobs.end()});
        FAIL();
    }
    catch (Exception & e)
    {
        int present[] = { 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
        for (int i = 0; i < std::size(present); i++)
            ASSERT_EQ(e.message().find(fmt::format("job{}", i)) != String::npos, present[i]);
    }

    const_cast<LoadJobSet &>(cycle_breaker->dependencies).clear();
}

TEST(AsyncLoader, CancelPendingJob)
{
    AsyncLoaderTest t;

    auto job_func = [&] (const LoadJob &) {};

    auto job = makeLoadJob({}, "job", job_func);
    auto task = t.loader.schedule({ job });

    task.remove(); // this cancels pending the job (async loader was not started to execute it)

    ASSERT_EQ(job->status(), LoadStatus::FAILED);
    try
    {
        job->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_EQ(e.code(), ErrorCodes::ASYNC_LOAD_CANCELED);
    }
}

TEST(AsyncLoader, CancelPendingTask)
{
    AsyncLoaderTest t;

    auto job_func = [&] (const LoadJob &) {};

    auto job1 = makeLoadJob({}, "job1", job_func);
    auto job2 = makeLoadJob({ job1 }, "job2", job_func);
    auto task = t.loader.schedule({ job1, job2 });

    task.remove(); // this cancels both jobs (async loader was not started to execute it)

    ASSERT_EQ(job1->status(), LoadStatus::FAILED);
    ASSERT_EQ(job2->status(), LoadStatus::FAILED);

    try
    {
        job1->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_TRUE(e.code() == ErrorCodes::ASYNC_LOAD_CANCELED);
    }

    try
    {
        job2->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        // Result depend on non-deterministic cancel order
        ASSERT_TRUE(e.code() == ErrorCodes::ASYNC_LOAD_CANCELED || e.code() == ErrorCodes::ASYNC_LOAD_DEPENDENCY_FAILED);
    }
}

TEST(AsyncLoader, CancelPendingDependency)
{
    AsyncLoaderTest t;

    auto job_func = [&] (const LoadJob &) {};

    auto job1 = makeLoadJob({}, "job1", job_func);
    auto job2 = makeLoadJob({ job1 }, "job2", job_func);
    auto task1 = t.loader.schedule({ job1 });
    auto task2 = t.loader.schedule({ job2 });

    task1.remove(); // this cancels both jobs, due to dependency (async loader was not started to execute it)

    ASSERT_EQ(job1->status(), LoadStatus::FAILED);
    ASSERT_EQ(job2->status(), LoadStatus::FAILED);

    try
    {
        job1->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_TRUE(e.code() == ErrorCodes::ASYNC_LOAD_CANCELED);
    }

    try
    {
        job2->wait();
        FAIL();
    }
    catch (Exception & e)
    {
        ASSERT_TRUE(e.code() == ErrorCodes::ASYNC_LOAD_DEPENDENCY_FAILED);
    }
}

TEST(AsyncLoader, CancelExecutingJob)
{
    AsyncLoaderTest t;
    t.loader.start();

    std::barrier sync(2);

    auto job_func = [&] (const LoadJob &)
    {
        sync.arrive_and_wait(); // (A) sync with main thread
        sync.arrive_and_wait(); // (B) wait for waiter
        // signals (C)
    };

    auto job = makeLoadJob({}, "job", job_func);
    auto task = t.loader.schedule({ job });

    sync.arrive_and_wait(); // (A) wait for job to start executing
    std::thread canceler([&]
    {
        task.remove(); // waits for (C)
    });
    while (job->waiters_count() == 0)
        std::this_thread::yield();
    ASSERT_EQ(job->status(), LoadStatus::PENDING);
    sync.arrive_and_wait(); // (B) sync with job
    canceler.join();

    ASSERT_EQ(job->status(), LoadStatus::SUCCESS);
    job->wait();
}

TEST(AsyncLoader, CancelExecutingTask)
{
    AsyncLoaderTest t(16);
    t.loader.start();
    std::barrier sync(2);

    auto blocker_job_func = [&] (const LoadJob &)
    {
        sync.arrive_and_wait(); // (A) sync with main thread
        sync.arrive_and_wait(); // (B) wait for waiter
        // signals (C)
    };

    auto job_to_cancel_func = [&] (const LoadJob &)
    {
        FAIL(); // this job should be canceled
    };

    auto job_to_succeed_func = [&] (const LoadJob &)
    {
    };

    // Make several iterations to catch the race (if any)
    for (int iteration = 0; iteration < 10; iteration++) {
        std::vector<LoadJobPtr> task1_jobs;
        auto blocker_job = makeLoadJob({}, "blocker_job", blocker_job_func);
        task1_jobs.push_back(blocker_job);
        for (int i = 0; i < 100; i++)
            task1_jobs.push_back(makeLoadJob({ blocker_job }, "job_to_cancel", job_to_cancel_func));
        auto task1 = t.loader.schedule({ task1_jobs.begin(), task1_jobs.end() });
        auto job_to_succeed = makeLoadJob({ blocker_job }, "job_to_succeed", job_to_succeed_func);
        auto task2 = t.loader.schedule({ job_to_succeed });

        sync.arrive_and_wait(); // (A) wait for job to start executing
        std::thread canceler([&]
        {
            task1.remove(); // waits for (C)
        });
        while (blocker_job->waiters_count() == 0)
            std::this_thread::yield();
        ASSERT_EQ(blocker_job->status(), LoadStatus::PENDING);
        sync.arrive_and_wait(); // (B) sync with job
        canceler.join();
        t.loader.wait();

        ASSERT_EQ(blocker_job->status(), LoadStatus::SUCCESS);
        ASSERT_EQ(job_to_succeed->status(), LoadStatus::SUCCESS);
        for (const auto & job : task1_jobs)
        {
            if (job != blocker_job)
                ASSERT_EQ(job->status(), LoadStatus::FAILED);
        }
    }
}

TEST(AsyncLoader, RandomTasks)
{
    AsyncLoaderTest t(16);
    t.loader.start();

    auto job_func = [&] (const LoadJob &)
    {
        t.randomSleepUs(100, 500, 5);
    };

    std::vector<AsyncLoader::Task> tasks;
    for (int i = 0; i < 512; i++)
    {
        int job_count = t.randomInt(1, 32);
        tasks.push_back(t.loader.schedule(t.randomJobSet(job_count, 5, job_func)));
        t.randomSleepUs(100, 900, 20); // avg=100us
    }
}

TEST(AsyncLoader, TestConcurrency)
{
    AsyncLoaderTest t(10);
    t.loader.start();

    for (int concurrency = 1; concurrency <= 10; concurrency++)
    {
        std::barrier sync(concurrency);

        std::atomic<int> executing{0};
        auto job_func = [&] (const LoadJob &)
        {
            executing++;
            ASSERT_LE(executing, concurrency);
            sync.arrive_and_wait();
            executing--;
        };

        std::vector<AsyncLoader::Task> tasks;
        for (int i = 0; i < concurrency; i++)
            tasks.push_back(t.loader.schedule(t.chainJobSet(5, job_func)));
        t.loader.wait();
        ASSERT_EQ(executing, 0);
    }
}

TEST(AsyncLoader, TestOverload)
{
    AsyncLoaderTest t(3);
    t.loader.start();

    size_t max_threads = t.loader.getMaxThreads();
    std::atomic<int> executing{0};

    for (int concurrency = 4; concurrency <= 8; concurrency++)
    {
        auto job_func = [&] (const LoadJob &)
        {
            executing++;
            t.randomSleepUs(100, 200, 100);
            ASSERT_LE(executing, max_threads);
            executing--;
        };

        t.loader.stop();
        std::vector<AsyncLoader::Task> tasks;
        for (int i = 0; i < concurrency; i++)
            tasks.push_back(t.loader.schedule(t.chainJobSet(5, job_func)));
        t.loader.start();
        t.loader.wait();
        ASSERT_EQ(executing, 0);
    }
}
