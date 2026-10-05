#pragma once

#include <optional>

namespace mplayer::native {

enum class NativeWorkKind { Preparation, Inference };

class NativeWorkLease {
public:
    NativeWorkLease(const NativeWorkLease&) = delete;
    NativeWorkLease& operator=(const NativeWorkLease&) = delete;
    NativeWorkLease(NativeWorkLease&& other) noexcept;
    NativeWorkLease& operator=(NativeWorkLease&& other) noexcept;
    ~NativeWorkLease();

    static std::optional<NativeWorkLease> acquire(NativeWorkKind kind);

private:
    explicit NativeWorkLease(NativeWorkKind kind) noexcept;
    void release() noexcept;

    NativeWorkKind kind_;
    bool held_ = true;
};

}
