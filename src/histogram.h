#pragma once

#include <cstdint>

// Log-linear histogram of nanosecond durations. Exact below 16 ns, then 16
// buckets per power of two, so no bucket is wider than 1/16 of its lower
// bound. Values from 2^40 ns (~18 minutes) up share the last bucket.
class Histogram {
public:
    static constexpr int kSubBits = 4;
    static constexpr int kSub = 1 << kSubBits;
    static constexpr int kMaxBits = 40;
    static constexpr int kBuckets = (kMaxBits - kSubBits + 1) * kSub;

    Histogram() { clear(); }

    void record(std::uint64_t ns) {
        ++buckets_[indexOf(ns)];
        ++count_;
        sum_ += ns;
        sumSquares_ += static_cast<double>(ns) * static_cast<double>(ns);
        if (ns < min_) min_ = ns;
        if (ns > max_) max_ = ns;
    }

    void merge(const Histogram& other);
    void clear();

    std::uint64_t count() const { return count_; }
    std::uint64_t sum() const { return sum_; }
    std::uint64_t min() const { return count_ != 0 ? min_ : 0; }
    std::uint64_t max() const { return max_; }
    double mean() const;
    double stddev() const;

    // p in [0, 100], interpolated linearly inside the bucket it lands in.
    double percentile(double p) const;

    // Values above ns, interpolating the bucket that straddles it.
    double countAbove(std::uint64_t ns) const;

    std::uint64_t bucket(int index) const { return buckets_[index]; }
    static std::uint64_t bucketLow(int index);
    static std::uint64_t bucketHigh(int index);   // exclusive

private:
    static int indexOf(std::uint64_t ns);

    std::uint64_t buckets_[kBuckets];
    std::uint64_t count_;
    std::uint64_t sum_;
    double sumSquares_;
    std::uint64_t min_;
    std::uint64_t max_;
};
