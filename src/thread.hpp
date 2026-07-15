#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>

#include "ffmpeg.hpp"

class Worker {
public:
    Worker() {
        thread_ = std::thread(&Worker::run, this);
    }

    ~Worker() {
        stop();
    }

    // Enqueue work from another thread
    template <typename Func>
    void wake(Func func = nullptr) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (func)
                func();
        }
        cv_.notify_one();           // Wake up the worker
    }

protected:
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    virtual void run() = 0;

    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
};

class VideoConverter : Worker {
public:
    VideoConverter(ff::VideoFile *_video) : video(_video) {
    }

private:
    void run() {
        while (true) {
            AVFrame *frame = nullptr;
            AVFrame *converted_frame = nullptr;

            // Wait for work or stop signal
            {
                std::unique_lock<std::mutex> lock(mtx_);
                if (frame) {
                    video->video_frame_queue.recycle(frame);
                    frame = nullptr;
                }
                if (converted_frame) {
                    video->video_converted_queue.push(converted_frame);
                    converted_frame = nullptr;
                }

                cv_.wait(lock, [this] {
                    return stop_ || (!video->video_frame_queue.empty() && video->video_converted_queue.size() < 2);
                });

                if (stop_) {
                    return;
                }

                frame = video->video_frame_queue.get();
                if (frame)
                    converted_frame = video->video_converted_queue.alloc();
            }

            // Do the work outside the lock (important for performance)
            if (frame && converted_frame) {
                video->scale_video_frame(frame, converted_frame);
            }
        }
    }

    ff::VideoFile *video;
};
