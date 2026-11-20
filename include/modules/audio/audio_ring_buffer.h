#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>

namespace paxg {
namespace audio {

/**
 * @brief Bounded, non-blocking circular FIFO ring buffer for PCM16 audio samples.
 *
 * Designed for real-time audio sample buffering without dynamic memory allocation.
 * Guarantees zero heap allocation, bounded memory footprint, and deterministic bounds.
 *
 * @tparam Capacity Maximum number of int16_t samples the buffer can hold.
 */
template <size_t Capacity = 1024>
class PcmRingBuffer {
    static_assert(Capacity > 0, "Capacity must be greater than zero");

public:
    PcmRingBuffer() : head_(0), tail_(0), count_(0) {
        std::memset(buffer_, 0, sizeof(buffer_));
    }

    /**
     * @brief Reset buffer to empty state.
     */
    void clear() {
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

    /**
     * @brief Check if buffer is empty.
     */
    bool isEmpty() const {
        return count_ == 0;
    }

    /**
     * @brief Check if buffer is full.
     */
    bool isFull() const {
        return count_ == Capacity;
    }

    /**
     * @brief Number of samples currently stored in buffer.
     */
    size_t available() const {
        return count_;
    }

    /**
     * @brief Number of samples that can be written before buffer is full.
     */
    size_t freeSpace() const {
        return Capacity - count_;
    }

    /**
     * @brief Maximum capacity of buffer in samples.
     */
    constexpr size_t capacity() const {
        return Capacity;
    }

    /**
     * @brief Push a single sample into buffer. Drops sample if buffer is full.
     * @param sample PCM16 audio sample.
     * @return true if pushed, false if buffer was full (sample dropped).
     */
    bool push(int16_t sample) {
        if (isFull()) {
            return false;
        }
        buffer_[head_] = sample;
        head_ = (head_ + 1) % Capacity;
        count_++;
        return true;
    }

    /**
     * @brief Push a single sample, overwriting the oldest sample if buffer is full.
     * @param sample PCM16 audio sample.
     */
    void pushOverwrite(int16_t sample) {
        buffer_[head_] = sample;
        head_ = (head_ + 1) % Capacity;
        if (isFull()) {
            tail_ = (tail_ + 1) % Capacity;
        } else {
            count_++;
        }
    }

    /**
     * @brief Push multiple samples. Drops any samples that do not fit.
     * @param data Pointer to input PCM16 samples.
     * @param numSamples Number of samples to write.
     * @return Actual number of samples pushed.
     */
    size_t push(const int16_t* data, size_t numSamples) {
        if (data == nullptr || numSamples == 0) {
            return 0;
        }
        size_t written = 0;
        while (written < numSamples && !isFull()) {
            buffer_[head_] = data[written];
            head_ = (head_ + 1) % Capacity;
            count_++;
            written++;
        }
        return written;
    }

    /**
     * @brief Push multiple samples, overwriting oldest samples if buffer exceeds capacity.
     * @param data Pointer to input PCM16 samples.
     * @param numSamples Number of samples to write.
     * @return Number of samples written (equal to numSamples).
     */
    size_t pushOverwrite(const int16_t* data, size_t numSamples) {
        if (data == nullptr || numSamples == 0) {
            return 0;
        }
        for (size_t i = 0; i < numSamples; ++i) {
            pushOverwrite(data[i]);
        }
        return numSamples;
    }

    /**
     * @brief Read and remove samples from buffer into destination array.
     * @param dest Destination buffer.
     * @param numSamples Maximum number of samples to pop.
     * @return Actual number of samples read.
     */
    size_t pop(int16_t* dest, size_t numSamples) {
        if (dest == nullptr || numSamples == 0 || isEmpty()) {
            return 0;
        }
        const size_t toRead = std::min(numSamples, count_);
        for (size_t i = 0; i < toRead; ++i) {
            dest[i] = buffer_[tail_];
            tail_ = (tail_ + 1) % Capacity;
        }
        count_ -= toRead;
        return toRead;
    }

    /**
     * @brief Peek samples from buffer without removing them.
     * @param dest Destination buffer.
     * @param numSamples Maximum number of samples to peek.
     * @param offset Offset from the oldest sample (tail).
     * @return Actual number of samples peeked.
     */
    size_t peek(int16_t* dest, size_t numSamples, size_t offset = 0) const {
        if (dest == nullptr || numSamples == 0 || offset >= count_) {
            return 0;
        }
        const size_t availableFromOffset = count_ - offset;
        const size_t toRead = std::min(numSamples, availableFromOffset);
        size_t idx = (tail_ + offset) % Capacity;
        for (size_t i = 0; i < toRead; ++i) {
            dest[i] = buffer_[idx];
            idx = (idx + 1) % Capacity;
        }
        return toRead;
    }

private:
    int16_t buffer_[Capacity];
    size_t head_;
    size_t tail_;
    size_t count_;
};

}  // namespace audio
}  // namespace paxg
