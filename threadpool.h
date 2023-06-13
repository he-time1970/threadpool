#ifndef THEADPOOL_H
#define THEADPOOL_H
#include <vector>
#include <thread>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <type_traits>
#include <map>
#include <chrono>

class ThreadPool
{
public:
    ThreadPool(int core_ths, int max_ths);
    ~ThreadPool();

    void shutdown()
    {
        std::unique_lock<std::mutex> lk(m_shutDownLock);
        m_isShutdown = true;
    }

    int getBusyNum()
    {
        int busy_num;
        {
            std::unique_lock<std::mutex> lk(m_wokerStateLock);
            busy_num = m_busyWorkers;
        }
        return busy_num;
    }

    int getIdleNum()
    {
        int idle_num;
        {
            std::unique_lock<std::mutex> lk(m_wokerStateLock);
            idle_num = m_idleWorkers;
        }
        return idle_num;
    }

    template <typename F, typename... ARGS>
    auto submit(F &&fn, ARGS &&...args) -> std::future<typename std::result_of<F(ARGS...)>::type>;

private:
    std::function<void()> createWorker();
    std::function<void()> createManager();
    bool isShutdown()
    {
        std::unique_lock<std::mutex> lk(m_shutDownLock);
        return m_isShutdown;
    }
    int needExitWorker()
    {
        int need_exit_num;
        {
            std::unique_lock<std::mutex> lk(this->m_wokerStateLock);
            need_exit_num = this->m_needExitWorkers;
        }
        return need_exit_num;
    }

private:
    int m_coreThs;
    int m_maxThs;

    std::mutex m_shutDownLock;
    bool m_isShutdown;

    std::mutex m_wokerStateLock;
    int m_busyWorkers;
    int m_idleWorkers;
    int m_needExitWorkers;

    std::thread m_managerTh;

    std::mutex m_queueLock;
    std::queue<std::function<void()>> m_tasks;
    std::condition_variable m_queueCond;
};

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lk(m_shutDownLock);
        m_isShutdown = true;
    }
}

std::function<void()> ThreadPool::createWorker()
{
    auto worker = [this]()
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(this->m_queueLock);
                m_queueCond.wait(lk, [this]()
                                 { return this->isShutdown() || !this->m_tasks.empty()\
                                 || this->needExitWorker()>0; });

                std::unique_lock<std::mutex> lk_state(this->m_wokerStateLock);
                while (this->m_needExitWorkers > 0)
                {
                    this->m_needExitWorkers--;
                    this->m_idleWorkers--;
                    return;
                }

                if ((this->isShutdown() && this->m_tasks.empty()))
                    return;

                task = this->m_tasks.front();
                this->m_tasks.pop();
            }

            {
                std::unique_lock<std::mutex> lk_state(this->m_wokerStateLock);
                this->m_busyWorkers++;
                this->m_idleWorkers--;
            }

            task();

            {
                std::unique_lock<std::mutex> lk_state(this->m_wokerStateLock);
                this->m_busyWorkers--;
                this->m_idleWorkers++;
            }
        }
    };

    return worker;
}

inline std::function<void()> ThreadPool::createManager()
{
    auto manager = [this]()
    {
        std::chrono::milliseconds wait_sec(100);
        while (!this->isShutdown())
        {
            std::this_thread::sleep_for(wait_sec);
            int task_num;
            {
                std::unique_lock<std::mutex> lk(this->m_queueLock);
                task_num = this->m_tasks.size();
            }
            int idle_workers_num;
            int total_workers;
            {
                std::unique_lock<std::mutex> lk(this->m_wokerStateLock);
                idle_workers_num = this->m_idleWorkers;
                total_workers = this->m_idleWorkers+this->m_busyWorkers;
            }

            // increase workers
            if (task_num > 2 * idle_workers_num)
            {
                for (int i = 0; i < task_num / 2 && total_workers < this->m_maxThs; i++)
                {
                    auto worker = this->createWorker();
                    std::thread th(worker);
                    th.detach();
                    std::unique_lock<std::mutex> lk(this->m_wokerStateLock);
                    this->m_idleWorkers++;
                    total_workers = this->m_idleWorkers + this->m_busyWorkers;
                }
            }

            // decrease workers
            if (task_num < 2 * idle_workers_num && idle_workers_num > this->m_coreThs)
            {
                std::unique_lock<std::mutex> lk(this->m_wokerStateLock);
                this->m_needExitWorkers = (idle_workers_num / 2) > this->m_coreThs ? (idle_workers_num / 2)
                                                                                   : idle_workers_num - this->m_coreThs;
                m_queueCond.notify_all();
            }
        }
    };
    return std::function<void()>(manager);
}

ThreadPool::ThreadPool(int core_ths, int max_ths) : m_coreThs(core_ths), m_maxThs(max_ths), m_isShutdown(false),m_busyWorkers(0), m_idleWorkers(0),m_needExitWorkers(0) 
{
    for (int i = 0; i < m_coreThs; i++)
    {
        auto worker = createWorker();
        std::thread th(worker);
        th.detach(); // 线程退出后自动释放所持有的资源
        {
            std::unique_lock<std::mutex> lk_state(this->m_wokerStateLock);
            this->m_idleWorkers++;
        }
    }

    auto manager = createManager();
    m_managerTh = std::move(std::thread(manager));
    m_managerTh.detach();
}

template <typename F, typename... ARGS>
auto ThreadPool::submit(F &&fn, ARGS &&...args) -> std::future<typename std::result_of<F(ARGS...)>::type>
{
    using return_type = typename std::result_of<F(ARGS...)>::type;

    auto task = std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(fn), std::forward<ARGS>(args)...));

    auto res = task->get_future();
    {
        std::unique_lock<std::mutex> lk(m_queueLock);
        m_tasks.emplace([task]()
                        { (*task)(); }); // 这里直接向队列里加入lambda表达式，以初始化function<void()>对象
    }
    m_queueCond.notify_all();
    return res;
}
#endif
