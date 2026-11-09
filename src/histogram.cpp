#include "histogram.h"

#include <cmath>
#include <cstring>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace {

// v != 0. _BitScanReverse64 does not exist on 32-bit MSVC.
int highestBit(std::uint64_t v) {
#ifdef _MSC_VER
    unsigned long index;
    if (_BitScanReverse(&index, static_cast<unsigned long>(v >> 32))) {
        return static_cast<int>(index) + 32;
    }
    _BitScanReverse(&index, static_cast<unsigned long>(v));
    return static_cast<int>(index);
#else
    return 63 - __builtin_clzll(v);
#endif
}

} // namespace

int Histogram::indexOf(std::uint64_t ns) {
    if (ns < static_cast<std::uint64_t>(kSub)) return static_cast<int>(ns);
    const int msb = highestBit(ns);
    if (msb >= kMaxBits) return kBuckets - 1;
    const int shift = msb - kSubBits;
    return (shift + 1) * kSub + static_cast<int>((ns >> shift) - kSub);
}

std::uint64_t Histogram::bucketLow(int index) {
    const int exponent = index / kSub;
    const std::uint64_t mantissa = static_cast<std::uint64_t>(index % kSub);
    if (exponent == 0) return mantissa;
    return (static_cast<std::uint64_t>(kSub) + mantissa) << (exponent - 1);
}

std::uint64_t Histogram::bucketHigh(int index) {
    const int exponent = index / kSub;
    if (exponent == 0) return bucketLow(index) + 1;
    return bucketLow(index) + (1ull << (exponent - 1));
}

void Histogram::clear() {
    std::memset(buckets_, 0, sizeof(buckets_));
    count_ = 0;
    sum_ = 0;
    sumSquares_ = 0.0;
    min_ = UINT64_MAX;
    max_ = 0;
}

void Histogram::merge(const Histogram& other) {
    if (other.count_ == 0) return;
    for (int i = 0; i < kBuckets; ++i) buckets_[i] += other.buckets_[i];
    count_ += other.count_;
    sum_ += other.sum_;
    sumSquares_ += other.sumSquares_;
    if (other.min_ < min_) min_ = other.min_;
    if (other.max_ > max_) max_ = other.max_;
}

double Histogram::mean() const {
    return count_ != 0 ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0;
}

double Histogram::stddev() const {
    if (count_ < 2) return 0.0;
    const double m = mean();
    const double variance = sumSquares_ / static_cast<double>(count_) - m * m;
    return variance > 0.0 ? std::sqrt(variance) : 0.0;
}

double Histogram::percentile(double p) const {
    if (count_ == 0) return 0.0;
    if (p <= 0.0) return static_cast<double>(min());
    if (p >= 100.0) return static_cast<double>(max_);
    const double rank = p / 100.0 * static_cast<double>(count_);
    double below = 0.0;
    for (int i = 0; i < kBuckets; ++i) {
        const double here = static_cast<double>(buckets_[i]);
        if (here == 0.0) continue;
        if (below + here >= rank) {
            const double low = static_cast<double>(bucketLow(i));
            const double high = static_cast<double>(bucketHigh(i));
            double value = low + (rank - below) / here * (high - low);
            // The true extremes are known exactly; never report past them.
            if (value < static_cast<double>(min_)) value = static_cast<double>(min_);
            if (value > static_cast<double>(max_)) value = static_cast<double>(max_);
            return value;
        }
        below += here;
    }
    return static_cast<double>(max_);
}

double Histogram::countAbove(std::uint64_t ns) const {
    if (count_ == 0 || ns >= max_) return 0.0;
    double total = 0.0;
    for (int i = kBuckets - 1; i >= 0; --i) {
        if (buckets_[i] == 0) continue;
        const std::uint64_t low = bucketLow(i);
        const std::uint64_t high = bucketHigh(i);
        if (low > ns) {
            total += static_cast<double>(buckets_[i]);
        } else {
            if (high > ns + 1) {
                total += static_cast<double>(buckets_[i]) *
                    static_cast<double>(high - ns - 1) / static_cast<double>(high - low);
            }
            break;
        }
    }
    return total;
}
