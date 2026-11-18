#pragma once

#include <atomic>

// The audio thread writes the peak, the UI timer reads it. Lock-free.
class LevelMeterSource
{
public:
    // Audio thread: raises the stored peak if `peak` is larger.
    void pushPeak (float peak) noexcept
    {
        auto current = value.load (std::memory_order_relaxed);

        while (peak > current
               && ! value.compare_exchange_weak (current, peak, std::memory_order_relaxed))
        {
        }
    }

    // UI timer: returns the peak since the last call and resets it to 0.
    float readAndReset() noexcept { return value.exchange (0.0f, std::memory_order_relaxed); }

private:
    std::atomic<float> value { 0.0f };
};
