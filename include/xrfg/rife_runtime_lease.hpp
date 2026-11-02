#pragma once
#include <windows.h>
#include <filesystem>

namespace xrfg {
// Shared readers protect exactly the runtime loaded by the worker. The
// installer opens the same file exclusively before replacing that directory.
class RifeRuntimeReadLease {
    HANDLE handle_{};
public:
    RifeRuntimeReadLease()=default;
    RifeRuntimeReadLease(const RifeRuntimeReadLease&)=delete;
    RifeRuntimeReadLease& operator=(const RifeRuntimeReadLease&)=delete;
    ~RifeRuntimeReadLease(){if(handle_)CloseHandle(handle_);}
    bool acquire(const std::filesystem::path& root) noexcept {
        if(handle_)return true;
        std::error_code error;
        const auto canonical=std::filesystem::canonical(root,error);
        if(error)return false;
        const auto path=canonical.parent_path()/(L"."+canonical.filename().wstring()+L".lease");
        const auto handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
            OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)return false;
        handle_=handle;
        return true;
    }
};
}
