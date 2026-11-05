//
// C++ Interface: NindThreadPool
//
// Description: les threads d'une recherche : dejmarrejs ah la demande, gardejs jusqu'ah la fin de l'objet.
//
// Author: Claude <noreply@anthropic.com>
//
// Copyright: 2014-2017 LATEJCON. See LICENCE.md file that comes with this distribution
// This file is part of NIND (as "nouvelle indexation").
// NIND is free software: you can redistribute it and/or modify it under the terms of the
// GNU Less General Public License (LGPL) as published by the Free Software Foundation,
// (see <http://www.gnu.org/licenses/>), either version 3 of the License, or any later version.
// NIND is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Less General Public License for more details.
////////////////////////////////////////////////////////////
#ifndef NindThreadPool_H
#define NindThreadPool_H
////////////////////////////////////////////////////////////
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
////////////////////////////////////////////////////////////
namespace latecon {
    namespace nindex {
////////////////////////////////////////////////////////////
/**\brief Threads for the searches that use several: started when first needed, as many as the most
* a search asked for (its own thread aside), kept until the pool goes. Tasks of concurrent searches
* queue; a task never waits for another of the same pool. */
class NindThreadPool {
public:
    NindThreadPool(): m_mutex(), m_wake(), m_tasks(), m_threads(), m_stop(false) {}
    ~NindThreadPool() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        for (size_t t = 0; t < m_threads.size(); t++) m_threads[t].join();
    }
    /**\brief Runs work on a pool thread (at least threads of them)
    *\return a future giving its end (or exception) */
    std::future<void> run(const std::function<void()> &work, const size_t threads) {
        std::shared_ptr<std::packaged_task<void()> > task(new std::packaged_task<void()>(work));
        std::future<void> done = task->get_future();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            while (m_threads.size() < threads) m_threads.push_back(std::thread([this]() { loop(); }));
            m_tasks.push([task]() { (*task)(); });
        }
        m_wake.notify_one();
        return done;
    }
private:
    NindThreadPool(const NindThreadPool &);
    NindThreadPool &operator=(const NindThreadPool &);
    void loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this]() { return m_stop || !m_tasks.empty(); });
                if (m_tasks.empty()) return;            //(m_stop)
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            task();                     //(a packaged_task: its exception goes to its future)
        }
    }
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::queue<std::function<void()> > m_tasks;
    std::vector<std::thread> m_threads;
    bool m_stop;
};
////////////////////////////////////////////////////////////
    } // end namespace
} // end namespace
#endif
