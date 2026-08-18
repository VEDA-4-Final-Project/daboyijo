#ifndef THREAD_SAFE_QUEUE_H
#define THREAD_SAFE_QUEUE_H 

#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>

template <typename T>

class ThreadSafeQueue {
public:
    ThreadSafeQueue() = default;
    ~ThreadSafeQueue() = default;

    void push(T value) {
        // mutex on
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(value));

        m_condVar.notify_one();
    }

    void waitAndPop(T& value) {
        std::unique_lock<std::mutex> lock(m_mutex);

        m_condVar.wait(lock, [this] {return !m_queue.empty();});

        value = std::move(m_queue.front());
        m_queue.pop();
    }

    bool tryPop(T& value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if(m_queue.empty()) {
            return false;
    }

    value = std::move(m_queue.front());
    m_queue.pop();
    return true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }

        
private:
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_condVar;
};



#endif //THREAD_SAFE_QUEUE_H
