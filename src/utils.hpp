#pragma once

template <typename ResourceID, typename DestroyFunc>
class ResourceHandle {
public:
    ResourceHandle(ResourceID id = {}, DestroyFunc destroy = DestroyFunc{})
        : id_(id), destroy_(destroy) {}

    ~ResourceHandle() {
        reset();
    }

    // Non-copyable, but movable
    ResourceHandle(const ResourceHandle&) = delete;
    ResourceHandle& operator=(const ResourceHandle&) = delete;

    ResourceHandle(ResourceHandle&& other) noexcept
        : id_(other.id_), destroy_(std::move(other.destroy_)) {
        other.id_ = {};
    }

    ResourceHandle& operator=(ResourceHandle&& other) noexcept {
        if (this != &other) {
            reset();
            id_ = other.id_;
            destroy_ = std::move(other.destroy_);
            other.id_ = {};
        }
        return *this;
    }

    ResourceID get() const { return id_; }
    explicit operator bool() const { return id_ != ResourceID{}; }

    void reset(ResourceID newId = {}) {
        if (id_ != ResourceID{}) {
            (void)destroy_(id_); // ignore return value
        }
        id_ = newId;
    }

private:
    ResourceID id_;
    DestroyFunc destroy_;
};
