#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>

#include "ffmpeg.hpp"

class Worker {
public:
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

    void start() {
        if (!thread_.joinable())
            thread_ = std::thread(&Worker::run, this);
    }

    virtual void run() = 0;

    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
};

class VideoConverter : public Worker {
public:
    void run() {
        AVFrame *frame = nullptr;
        AVFrame *converted_frame = nullptr;

        while (true) {
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
                    converted_frame = video->video_converted_queue.alloc([this]{ return video->alloc_converted_frame();});
            }

            // Do the work outside the lock (important for performance)
            if (frame && converted_frame) {
                video->scale_video_frame(frame, converted_frame);
                converted_frame->pts = frame->pts;
                converted_frame->duration = frame->duration;
            }
        }
    }

    auto count_video_frame() {
        std::lock_guard<std::mutex> lock(mtx_);
        return video->video_frame_queue.size();
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mtx_);
        return video->video_frame_queue.empty() && video->video_converted_queue.empty();
    }

    double next_play_time(double play_time) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!video->video_converted_queue.empty()) {
            return video->video_converted_queue.front()->pts * video->video_time_base;
        }
        if (!video->video_frame_queue.empty()) {
            return video->video_frame_queue.front()->pts * video->video_time_base;
        }
        return 0;
    }

    ff::VideoFile *video;
};
