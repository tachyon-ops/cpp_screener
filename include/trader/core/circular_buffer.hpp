#pragma once
#include <vector>
#include <stdexcept>

namespace trader {
namespace core {

template <typename T>
class CircularBuffer {
public:
    explicit CircularBuffer(size_t capacity) : capacity_(capacity), head_(0), tail_(0), size_(0) {
        buffer_.resize(capacity);
    }

    void push_back(const T& item) {
        if (capacity_ == 0) return;
        buffer_[tail_] = item;
        tail_ = (tail_ + 1) % capacity_;
        if (size_ == capacity_) {
            head_ = (head_ + 1) % capacity_;
        } else {
            size_++;
        }
    }

    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }

    const T& operator[](size_t idx) const {
        if (idx >= size_) throw std::out_of_range("CircularBuffer index out of range");
        return buffer_[(head_ + idx) % capacity_];
    }

    T& operator[](size_t idx) {
        if (idx >= size_) throw std::out_of_range("CircularBuffer index out of range");
        return buffer_[(head_ + idx) % capacity_];
    }

    const T& back() const {
        if (size_ == 0) throw std::runtime_error("CircularBuffer is empty");
        return buffer_[(tail_ + capacity_ - 1) % capacity_];
    }

    T& back() {
        if (size_ == 0) throw std::runtime_error("CircularBuffer is empty");
        return buffer_[(tail_ + capacity_ - 1) % capacity_];
    }

    void clear() {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
    }

private:
    std::vector<T> buffer_;
    size_t capacity_;
    size_t head_;
    size_t tail_;
    size_t size_;
};

} // namespace core
} // namespace trader
