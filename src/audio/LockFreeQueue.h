#pragma once

#include <juce_core/juce_core.h>

#include <array>

// Single-producer / single-consumer lock-free queue built on juce::AbstractFifo.
// Holds exactly Capacity items. AbstractFifo can only use (size - 1) slots, so the
// storage is Capacity + 1 long. push/pop never allocate, free or lock, so either
// side may be the audio thread.
template <typename T, int Capacity>
class LockFreeQueue
{
public:
    static_assert (Capacity > 0, "LockFreeQueue needs a positive capacity");

    // Producer side. Returns false (and copies nothing) when the queue is full.
    bool push (const T& item)
    {
        const auto scope = fifo.write (1);

        if (scope.blockSize1 > 0)
            storage[(size_t) scope.startIndex1] = item;
        else if (scope.blockSize2 > 0)
            storage[(size_t) scope.startIndex2] = item;
        else
            return false;

        return true;
    }

    // Consumer side. Returns false (and leaves item untouched) when the queue is empty.
    bool pop (T& item)
    {
        const auto scope = fifo.read (1);

        if (scope.blockSize1 > 0)
            item = storage[(size_t) scope.startIndex1];
        else if (scope.blockSize2 > 0)
            item = storage[(size_t) scope.startIndex2];
        else
            return false;

        return true;
    }

    int getNumReady() const noexcept   { return fifo.getNumReady(); }
    static constexpr int capacity() noexcept   { return Capacity; }

private:
    juce::AbstractFifo fifo { Capacity + 1 };
    std::array<T, (size_t) Capacity + 1> storage {};
};
