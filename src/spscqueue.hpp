#include <atomic>
#include <vector>
#include <optional>
#include <new> // For hardware_destructive_interference_size

// Establish a reliable cache-line size at compile time
#ifdef __cpp_lib_hardware_interference_size
    using std::hardware_destructive_interference_size;
#else
    // 64 bytes is the standard cache line size for most modern CPUs
    constexpr size_t hardware_destructive_interference_size = 64;
#endif

template <typename T>
class SPSCQueue {
private:
    std::vector<T> buffer;
    const size_t capacity;
    
    // Align indices to prevent false sharing between reader and writer threads
    alignas(hardware_destructive_interference_size) std::atomic<size_t> head{0};
    alignas(hardware_destructive_interference_size) std::atomic<size_t> tail{0};

public:
    explicit SPSCQueue(size_t cap) : capacity(cap + 1) {
        buffer.resize(capacity);
    }

    // Push an item (Called by Producer thread only)
    bool enqueue(T&& item) { // Change from const T& to T&&
        const size_t current_tail = tail.load(std::memory_order_relaxed);
        const size_t next_tail = (current_tail + 1) % capacity;
        
        if (next_tail == head.load(std::memory_order_acquire)) {
            return false; 
        }
        
        // Transfer ownership of the unique_ptr via std::move
        buffer[current_tail] = std::move(item); 
        
        tail.store(next_tail, std::memory_order_release);
        return true;
    }

    // Pop an item (Called by Consumer thread only)
    std::optional<T> dequeue() {
        const size_t current_head = head.load(std::memory_order_relaxed);
        
        // If head equals tail, the queue is empty
        if (current_head == tail.load(std::memory_order_acquire)) {
            return std::nullopt; 
        }
        
        T item = std::move(buffer[current_head]);
        head.store((current_head + 1) % capacity, std::memory_order_release);
        return item;
    }

    void clear() {
        while (dequeue()) {
        }
    }
};