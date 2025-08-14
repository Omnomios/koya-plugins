/*
   Koya SDK Utility: threaded::queue

   What this is:
   - A minimal thread-safe queue used by various plugins to pass jobs and
     results between worker threads and the engine thread.

   Koya vs Application responsibilities:
   - This utility is SDK-ish and generic; it contains no QuickJS or plugin
     knowledge. Plugins (application) decide when to push/pop and how to
     integrate draining with the `update` hook.
*/
#pragma once

#include <queue>
#include <mutex>

namespace threaded {

template <typename T>
class queue {
public:
    void push(const T& value) {
        std::lock_guard<std::mutex> lock(this->mutex_);
        this->q_.push(value);
    }

    T frontPop() {
        std::lock_guard<std::mutex> lock(this->mutex_);
        T value = this->q_.front();
        this->q_.pop();
        return value;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(this->mutex_);
        return this->q_.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(this->mutex_);
        return this->q_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::queue<T> q_;
};

} // namespace threaded


