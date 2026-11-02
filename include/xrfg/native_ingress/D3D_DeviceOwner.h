#pragma once
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <mutex>

namespace d3d_owner {
// Reserved for this short, serialized ownership challenge; never frame pixels.
inline constexpr GUID receipt_guid{
    0xf1d70763, 0xc534, 0x4b89, {0xa8, 0x43, 0x64, 0x42, 0x9c, 0x97, 0x18, 0x5b}};
struct Receipt {
    std::array<HRESULT, 10> steps{E_PENDING,E_PENDING,E_PENDING,E_PENDING,E_PENDING,
        E_PENDING,E_PENDING,E_PENDING,E_PENDING,E_PENDING};
    std::uint32_t mask{};
    bool verified{}, canonical{}, fresh{};
};
// A private-data round trip proves shared ID3D12Object backing state, NOT that
// an unsupported proxy has become a native interface. Caller chooses one
// authoritative allocation device and keeps the real queue/submission checks.
template<typename Device>
class Cache {
    struct Entry {
        Microsoft::WRL::ComPtr<IUnknown> left, right;
        Receipt receipt;
    };
    std::array<Entry, 8> entries_{};
    std::size_t count_{};
    std::mutex mutex_;
    struct Challenge {
        std::uint64_t cache{}, sequence{}, left{}, right{}, phase{};
        bool operator==(const Challenge&) const = default;
    };
public:
    bool same(Device* left, Device* right, Receipt* receipt = nullptr) {
        Receipt result;
        const auto finish = [&] { if (receipt) *receipt = result; return result.verified; };
        if (!left || !right) return finish();
        Microsoft::WRL::ComPtr<IUnknown> a, b;
        if (FAILED(left->QueryInterface(IID_PPV_ARGS(a.GetAddressOf()))) ||
            FAILED(right->QueryInterface(IID_PPV_ARGS(b.GetAddressOf())))) return finish();
        if (a.Get() == b.Get()) {
            result.verified = result.canonical = true;
            return finish();
        }
        const auto al = left->GetAdapterLuid(), bl = right->GetAdapterLuid();
        if (al.LowPart != bl.LowPart || al.HighPart != bl.HighPart) return finish();
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& entry = entries_[i];
            if ((entry.left.Get() == a.Get() && entry.right.Get() == b.Get()) ||
                (entry.left.Get() == b.Get() && entry.right.Get() == a.Get())) {
                result = entry.receipt;
                result.fresh = false;
                return finish();
            }
        }
        if (count_ == entries_.size()) return finish(); // Bounded; never guess.
        auto& entry = entries_[count_++];
        entry.left = a; entry.right = b; // Pin identities: no pointer reuse.
        result.fresh = true;
        UINT size{};
        result.steps[0] = left->GetPrivateData(receipt_guid, &size, nullptr);
        size = 0;
        result.steps[1] = right->GetPrivateData(receipt_guid, &size, nullptr);
        if (result.steps[0] == DXGI_ERROR_NOT_FOUND) result.mask |= 16;
        if (result.steps[1] == DXGI_ERROR_NOT_FOUND) result.mask |= 32;
        // Never overwrite or delete an existing value under our reserved key.
        if (result.mask == 48) {
            Challenge sent{reinterpret_cast<std::uintptr_t>(this), count_,
                reinterpret_cast<std::uintptr_t>(a.Get()),
                reinterpret_cast<std::uintptr_t>(b.Get()), 1};
            Challenge received{};
            result.steps[2] = left->SetPrivateData(receipt_guid, sizeof(sent), &sent);
            size = sizeof(received);
            if (SUCCEEDED(result.steps[2]))
                result.steps[3] = right->GetPrivateData(receipt_guid, &size, &received);
            if (SUCCEEDED(result.steps[3]) && size == sizeof(sent) && received == sent)
                result.mask |= 1;
            if (result.mask & 1) {
                sent.phase = 2;
                result.steps[4] = right->SetPrivateData(receipt_guid, sizeof(sent), &sent);
                received = {}; size = sizeof(received);
                if (SUCCEEDED(result.steps[4]))
                    result.steps[5] = left->GetPrivateData(receipt_guid, &size, &received);
                if (SUCCEEDED(result.steps[5]) && size == sizeof(sent) && received == sent)
                    result.mask |= 2;
            }
            result.steps[6] = left->SetPrivateData(receipt_guid, 0, nullptr);
            result.steps[7] = right->SetPrivateData(receipt_guid, 0, nullptr);
            size = 0; result.steps[8] = left->GetPrivateData(receipt_guid, &size, nullptr);
            size = 0; result.steps[9] = right->GetPrivateData(receipt_guid, &size, nullptr);
            if ((SUCCEEDED(result.steps[6]) || result.steps[6] == DXGI_ERROR_NOT_FOUND) &&
                result.steps[8] == DXGI_ERROR_NOT_FOUND) result.mask |= 4;
            if ((SUCCEEDED(result.steps[7]) || result.steps[7] == DXGI_ERROR_NOT_FOUND) &&
                result.steps[9] == DXGI_ERROR_NOT_FOUND) result.mask |= 8;
            result.verified = result.mask == 63;
        }
        entry.receipt = result;
        return finish();
    }
};
inline Cache<ID3D12Device>& cache() {
    static auto& instance = *new Cache<ID3D12Device>; // pinned for the process, never released at detach
    return instance;
}
inline bool same(ID3D12Device* left, ID3D12Device* right, Receipt* receipt = nullptr) {
    return cache().same(left, right, receipt);
}
} // namespace d3d_owner
