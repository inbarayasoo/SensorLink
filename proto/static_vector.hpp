#pragma once

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <utility>

namespace proto {

// A vector with a compile-time capacity and no dynamic allocation. The elements
// live inside the object itself, so a static_vector can be a member of a type
// that must never touch the heap (for example a frame buffer on the MCU).
//
// T is expected to be trivially copyable; unused slots are default-constructed.
template <typename T, std::size_t Capacity>
class static_vector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T*;
    using const_iterator = const T*;

    constexpr static_vector() noexcept = default;

    constexpr static_vector(std::initializer_list<T> init) {
        assert(init.size() <= Capacity && "static_vector: initializer list too long");
        for (const T& value : init) {
            data_[size_++] = value;
        }
    }

    static constexpr size_type capacity() noexcept { return Capacity; }
    constexpr size_type size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr bool full() const noexcept { return size_ == Capacity; }

    constexpr void clear() noexcept { size_ = 0; }

    // Returns false and does nothing when the vector is already full.
    constexpr bool push_back(const T& value) {
        if (full()) {
            return false;
        }
        data_[size_++] = value;
        return true;
    }

    constexpr bool push_back(T&& value) {
        if (full()) {
            return false;
        }
        data_[size_++] = std::move(value);
        return true;
    }

    constexpr void pop_back() {
        assert(!empty() && "static_vector: pop_back on empty vector");
        --size_;
    }

    constexpr T& operator[](size_type index) {
        assert(index < size_ && "static_vector: index out of range");
        return data_[index];
    }
    constexpr const T& operator[](size_type index) const {
        assert(index < size_ && "static_vector: index out of range");
        return data_[index];
    }

    constexpr T& front() { return (*this)[0]; }
    constexpr const T& front() const { return (*this)[0]; }
    constexpr T& back() { return (*this)[size_ - 1]; }
    constexpr const T& back() const { return (*this)[size_ - 1]; }

    constexpr T* data() noexcept { return data_; }
    constexpr const T* data() const noexcept { return data_; }

    constexpr iterator begin() noexcept { return data_; }
    constexpr iterator end() noexcept { return data_ + size_; }
    constexpr const_iterator begin() const noexcept { return data_; }
    constexpr const_iterator end() const noexcept { return data_ + size_; }
    constexpr const_iterator cbegin() const noexcept { return data_; }
    constexpr const_iterator cend() const noexcept { return data_ + size_; }

private:
    T data_[Capacity] = {};
    size_type size_ = 0;
};

}  // namespace proto
