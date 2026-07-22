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
        if (!thread_.joinable()) {
            thread_ = std::thread(&Worker::run, this);
            pthread_setname_np(thread_.native_handle(), "video");
        }
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
        AVPacket *packet = nullptr;
        ff::FrameQueue frame_queue;

        while (true) {
            // Wait for work or stop signal
            {
                std::unique_lock<std::mutex> lock(mtx_);
                if (pts != pts_clear) {
                    while (!frame_queue.empty()) {
                        video->video_frame_queue.push(frame_queue.front());
                        frame_queue.pop();
                    }
                }
                pts = pts_undefined;

                cv_.wait(lock, [this] {
                    return stop_ || (!video->video_packet_queue.empty() && video->video_frame_queue.size() < 2);
                });

                if (stop_) {
                    return;
                }

                packet = video->video_packet_queue.get();
                pts = 0;
            }

            // Do the work outside the lock (important for performance)
            frame_queue.clear();
            video->feed_video_frame(packet, [&](AVFrame *frame){
                auto new_frame = frame_queue.alloc();
                if (frame->hw_frames_ctx) {
                    new_frame->format = video->pixel_format;
                    av_hwframe_transfer_data(new_frame, frame, 0);
                    new_frame->pts = frame->pts;
                    new_frame->duration = frame->duration;
/*                    if (av_hwframe_map(new_frame, frame, AV_HWFRAME_MAP_READ) < 0) {
                        fprintf(stderr, "Mapping to DRM PRIME failed!\n");
                        av_frame_free(&new_frame);
                    }*/
                } else
                    av_frame_ref(new_frame, frame);
                frame_queue.push(new_frame);
            });
            av_packet_free(&packet);
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        video->video_packet_queue.clear();
        video->video_frame_queue.clear();
        pts = pts_clear;
    }

    auto count_video_packet() {
        std::lock_guard<std::mutex> lock(mtx_);
        return video->video_packet_queue.size();
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mtx_);
        return pts <= pts_undefined && video->video_frame_queue.empty() && video->video_packet_queue.empty();
    }

    double next_play_time() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!video->video_frame_queue.empty()) {
            return video->video_frame_queue.front()->pts * video->video_time_base;
        }
        if (pts > pts_undefined)
            return pts * video->video_time_base;
        return 0;
    }

    ff::VideoFile *video;

private:
    int64_t pts = pts_undefined;
    const int64_t pts_undefined = SDL_MIN_SINT64 + 99;
    const int64_t pts_clear = SDL_MIN_SINT64;
    AVBufferPool *my_pool = nullptr;
};
