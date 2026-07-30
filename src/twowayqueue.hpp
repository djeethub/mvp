#pragma once

#include "readerwriterqueue.h"

template<typename T>
class TwowayQueue {
protected:
    moodycamel::ReaderWriterQueue<T> data_queue;
    moodycamel::ReaderWriterQueue<T> recycle_queue;

    virtual T create() const {
        return new std::remove_pointer_t<T>;
    }

    virtual void destroy(T t) const {
        delete t;
    }

public:
    explicit TwowayQueue(size_t data_size = 15, size_t recycle_size = 15) : data_queue(data_size), recycle_queue(recycle_size) {

    }

    ~TwowayQueue() {
        T t;
        while (data_queue.try_dequeue(t)) {
            destroy(t);
        }
        while (recycle_queue.try_dequeue(t)) {
            destroy(t);
        }
    }

    bool enqueue(T const& t) {
        return data_queue.enqueue(t);
    }

    bool dequeue(T& t) {
        return data_queue.try_dequeue(t);
    }

    bool pop() {
        return data_queue.pop();
    }

    T peek() {
        return *data_queue.peek();
    }

    bool recycle(T const& t) {
        return recycle_queue.enqueue(t);
    }

    T alloc() {
        T t;
        if (recycle_queue.try_dequeue(t))
            return t;
        return create();
    }

    void recycle_all() {
        T t;
        while (data_queue.try_dequeue(t)) {
            recycle(t);
        }
    }

    size_t size() {
        return data_queue.size_approx();
    }

    bool empty() {
        return size() == 0;
    }
};
