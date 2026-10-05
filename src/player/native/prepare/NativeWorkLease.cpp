#include "NativeWorkLease.hpp"

#include <atomic>
#include <cstdint>
#include <utility>

namespace mplayer::native {
namespace {

constexpr std::uint64_t preparationClaim = std::uint64_t{1} << 63;
std::atomic<std::uint64_t> workClaims{0};

}

NativeWorkLease::NativeWorkLease(NativeWorkKind kind) noexcept : kind_(kind) {}

NativeWorkLease::NativeWorkLease(NativeWorkLease&& other) noexcept
    : kind_(other.kind_), held_(std::exchange(other.held_, false)) {}

NativeWorkLease& NativeWorkLease::operator=(NativeWorkLease&& other) noexcept {
    if (this != &other) {
        release();
        kind_ = other.kind_;
        held_ = std::exchange(other.held_, false);
    }
    return *this;
}

NativeWorkLease::~NativeWorkLease() {
    release();
}

std::optional<NativeWorkLease> NativeWorkLease::acquire(NativeWorkKind kind) {
    auto current = workClaims.load(std::memory_order_relaxed);
    for (;;) {
        std::uint64_t desired;
        if (kind == NativeWorkKind::Preparation) {
            if (current != 0) return std::nullopt;
            desired = preparationClaim;
        } else {
            if (current >= preparationClaim - 1) return std::nullopt;
            desired = current + 1;
        }
        if (workClaims.compare_exchange_weak(current, desired,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
            return NativeWorkLease(kind);
        }
    }
}

void NativeWorkLease::release() noexcept {
    if (!std::exchange(held_, false)) return;
    workClaims.fetch_sub(kind_ == NativeWorkKind::Preparation ? preparationClaim : 1,
                         std::memory_order_release);
}

}
