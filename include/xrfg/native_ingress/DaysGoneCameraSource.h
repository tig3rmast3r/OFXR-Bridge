#pragma once
#include "xrfg/daysgone_camera.hpp"
#include <windows.h>
#include <safetyhook.hpp>
#include <atomic>
#include <climits>
#include <mutex>
#include <new>
#include <span>

// Native UE4.11/Bend view capture. The verified layouts and RHI allocator
// contract are adapted from Bustanity's MIT Days Gone DLSS implementation;
// see licenses/DaysGoneDLSS-MIT.txt. No modified addon is loaded or required.
namespace xrfg::daysgone_camera_source {
using Address=std::uintptr_t;
struct Layout {
    std::size_t size,rect,unscaled,jitter,cut,aa,state,projection;
};
inline constexpr Layout layout113{0x2d50,0xb28,0xb38,0x10f0,0x1755,0x1760,0x1ce0,0xb60};
inline constexpr std::array<unsigned char,14> visibility_prefix{
    0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55};
inline constexpr std::array<unsigned char,24> allocate_prefix{
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,
    0x74,0x24,0x18,0x48,0x89,0x7c,0x24,0x20,0x41,0x56,0x48,0x83};
struct Record {daysgone_camera::Packet packet;float view_jitter[2]{};bool valid{},cut{};
    daysgone_camera::Rejection rejection{daysgone_camera::Rejection::clock};};
struct Slot {Record current,previous;bool consumed{true};};
inline thread_local Slot executing;
using Allocate=void*(*)(Address,std::int32_t);
struct Command {Command* next{};void(*execute)(Address,Command*){};Record record;};
struct Statistics {unsigned status{};std::uint64_t queued{},executed{},taken{},rejected{};};
namespace detail {
inline std::atomic_uint status{};
inline std::atomic_uint64_t sequence{},queued{},executed{},taken{},rejected{};
template<class T>bool read(Address address,T& value) noexcept {
    SIZE_T copied{};return address&&ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),
        &value,sizeof(value),&copied)&&copied==sizeof(value);
}
inline bool bytes(Address address,void* output,std::size_t count) noexcept {
    SIZE_T copied{};return address&&ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),
        output,count,&copied)&&copied==count;
}
template<class T>T field(std::span<const unsigned char> view,std::size_t offset) noexcept {
    T value{};if(offset<=view.size()&&sizeof(T)<=view.size()-offset)std::memcpy(&value,view.data()+offset,sizeof(T));return value;
}
inline bool writable(Address address,std::size_t count) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if(!address||!VirtualQuery(reinterpret_cast<const void*>(address),&info,sizeof(info))||
        info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto access=info.Protect&0xff;
    if(access!=PAGE_READWRITE&&access!=PAGE_WRITECOPY&&access!=PAGE_EXECUTE_READWRITE&&access!=PAGE_EXECUTE_WRITECOPY)return false;
    return address>=reinterpret_cast<Address>(info.BaseAddress)&&count<=info.RegionSize-(address-reinterpret_cast<Address>(info.BaseAddress));
}
inline bool camera_matches_view(const Record& record,const std::array<float,16>& expected) noexcept {
    const auto& c=record.packet.camera;
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col) {
        double product{};for(unsigned k=0;k<4;++k)product+=double(c.translated_view[row*4+k])*c.projection[k*4+col];
        const auto wanted=expected[row*4+col];
        if(!std::isfinite(product)||!std::isfinite(wanted)||std::abs(product-wanted)>1e-3*std::max(1.,std::abs(double(wanted))))return false;
    }
    return true;
}
inline void execute(Address,Command* command) noexcept {
    executing.previous=executing.current;executing.current=command->record;
    executing.current.packet.thread=GetCurrentThreadId();executing.consumed=false;++executed;
}
inline bool enqueue(Address list,Allocate allocate,const Record& record) noexcept {
    bool active{};Address link{};std::uint32_t count{};
    if(!allocate||!read(list+0x10,active)||active||!read(list+8,link)||!read(list+0x14,count)||count>10'000'000||
        !writable(list+8,sizeof(Address))||!writable(list+0x14,sizeof(count))||!writable(link,sizeof(Address)))return false;
    const auto memory=list+0x30;std::int32_t pages{};Address data{};void* storage{};
    if(!read(memory+0x28,pages)||pages<0||pages>4096)return false;
    if(pages) {
        if(!read(memory+0x20,data))return false;if(!data)data=memory;
        const auto page=data+Address(pages-1)*16;std::array<std::int32_t,2> bounds{};Address buffer{};
        if(!read(page,bounds)||!read(page+8,buffer)||bounds[0]<0||bounds[1]<bounds[0]||bounds[0]>INT_MAX-7)return false;
        const auto offset=(bounds[0]+7)&~7;
        if(bounds[1]-offset>=std::int32_t(sizeof(Command))&&writable(page,sizeof(std::int32_t))&&
            writable(buffer+offset,sizeof(Command))) {
            *reinterpret_cast<std::int32_t*>(page)=offset+std::int32_t(sizeof(Command));
            storage=reinterpret_cast<void*>(buffer+offset);
        }
    }
    if(!storage)storage=allocate(memory,std::int32_t(sizeof(Command)));
    if(!writable(reinterpret_cast<Address>(storage),sizeof(Command)))return false;
    auto* command=new(storage)Command{nullptr,&execute,record};
    *reinterpret_cast<std::uint32_t*>(list+0x14)=count+1;
    *reinterpret_cast<Command**>(link)=command;
    *reinterpret_cast<Command***>(list+8)=&command->next;++queued;return true;
}
inline bool capture(Address renderer,Address list,Allocate allocate,const Layout& layout=layout113) noexcept {
    Address views{};std::int32_t count{};
    if(!read(renderer+0x80,views)||!read(renderer+0x88,count)||!views||count<1||count>4||layout.size>0x3000)return false;
    Record record;unsigned selected{};
    std::array<unsigned char,0x3000> storage{};
    for(std::int32_t i=0;i<count;++i) {
        if(!bytes(views+Address(i)*layout.size,storage.data(),layout.size))return false;
        const auto view=std::span<const unsigned char>(storage.data(),layout.size);
        if(field<std::uint8_t>(view,layout.aa)!=3||!field<Address>(view,layout.state))continue;
        ++selected;
        if(selected!=1)continue;
        auto& p=record.packet;p.game_build=113;p.sequence=++sequence;
        const auto rect=field<std::array<std::int32_t,4>>(view,layout.rect);
        const auto output=field<std::array<std::int32_t,4>>(view,layout.unscaled);
        const auto size=[](const auto& r){return r[0]>=0&&r[1]>=0&&r[2]>r[0]&&r[3]>r[1]&&r[2]<=32768&&r[3]<=32768;};
        if(!size(rect)||!size(output)){record.rejection=daysgone_camera::Rejection::extent;continue;}
        p.render_width=rect[2]-rect[0];p.render_height=rect[3]-rect[1];
        p.output_width=output[2]-output[0];p.output_height=output[3]-output[1];
        const auto jitter=field<std::array<float,2>>(view,layout.jitter);
        std::copy(jitter.begin(),jitter.end(),record.view_jitter);
        record.cut=field<std::uint8_t>(view,layout.cut)!=0;
        const auto matrices=layout.projection;
        const auto projection=field<std::array<float,16>>(view,matrices);
        const auto translated=field<std::array<float,16>>(view,matrices+0x80);
        const auto origin=field<std::array<float,3>>(view,matrices+0x160);
        const auto projection_jitter=field<std::array<float,2>>(view,matrices+0x174);
        std::copy(projection.begin(),projection.end(),p.camera.projection);
        std::copy(translated.begin(),translated.end(),p.camera.translated_view);
        std::copy(origin.begin(),origin.end(),p.camera.origin);
        std::copy(projection_jitter.begin(),projection_jitter.end(),p.camera.projection_jitter);
        const auto family=field<Address>(view,0);
        record.valid=read(family+0x48,p.frame)&&p.frame&&daysgone_camera::finite(record.view_jitter,2)&&
            camera_matches_view(record,field<std::array<float,16>>(view,matrices+0xc0));
        record.rejection=record.valid?daysgone_camera::Rejection::none:daysgone_camera::Rejection::projection;
    }
    if(!selected)return false;
    if(selected!=1){record.valid=false;record.rejection=daysgone_camera::Rejection::identity;} // Never choose the newest of several views.
    return enqueue(list,allocate,record);
}
using Setup=void(*)(Address,Address);
struct Hooks {std::mutex mutex;bool attempted{};SafetyHookInline hook;Setup original{};Allocate allocate{};};
inline Hooks& hooks(){static auto* h=new Hooks;return *h;}
inline void setup(Address renderer,Address list) {
    hooks().original(renderer,list);
    try{(void)capture(renderer,list,hooks().allocate);}catch(...){++rejected;}
}
// Check the original file's PE identity and code, even if an existing injector
// has already hooked the entry. Only a verified addon detour may be chained.
inline bool disk_prefix(Address rva,std::span<const unsigned char> expected) noexcept {
    wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768))return false;
    const auto file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    struct Close {HANDLE h;~Close(){CloseHandle(h);}} close{file};
    std::array<unsigned char,4096> headers{};DWORD got{};
    if(!ReadFile(file,headers.data(),DWORD(headers.size()),&got,nullptr)||got<sizeof(IMAGE_NT_HEADERS64))return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(headers.data());
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||std::size_t(dos->e_lfanew)>got-sizeof(IMAGE_NT_HEADERS64))return false;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(headers.data()+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.TimeDateStamp!=0x68754d46||nt->OptionalHeader.SizeOfImage!=0x93f0000||
        nt->FileHeader.NumberOfSections>64)return false;
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    if(reinterpret_cast<const unsigned char*>(sections+nt->FileHeader.NumberOfSections)>headers.data()+got)return false;
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        const auto& s=sections[i];
        if(rva<s.VirtualAddress||rva-s.VirtualAddress>s.SizeOfRawData||expected.size()>s.SizeOfRawData-(rva-s.VirtualAddress))continue;
        LARGE_INTEGER offset{};offset.QuadPart=s.PointerToRawData+rva-s.VirtualAddress;
        std::array<unsigned char,32> code{};
        return expected.size()<=code.size()&&SetFilePointerEx(file,offset,nullptr,FILE_BEGIN)&&
            ReadFile(file,code.data(),DWORD(expected.size()),&got,nullptr)&&got==expected.size()&&
            std::equal(expected.begin(),expected.end(),code.begin());
    }
    return false;
}
inline bool addon_detour(Address address) noexcept {
    const auto addon=GetModuleHandleW(L"DaysGoneDLSS.addon64");if(!addon)return false;
    Address visited[4]{};
    for(unsigned depth=0;depth<4;++depth) {
        for(unsigned i=0;i<depth;++i)if(visited[i]==address)return false;
        visited[depth]=address;
        HMODULE owner{};
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address),&owner)&&owner==addon)return true;
        std::array<unsigned char,6> code{};if(!bytes(address,code.data(),code.size()))return false;
        std::int32_t relative{};
        if(code[0]==0xe9){std::memcpy(&relative,code.data()+1,4);address=Address(std::intptr_t(address+5)+relative);}
        else if(code[0]==0xff&&code[1]==0x25) {
            std::memcpy(&relative,code.data()+2,4);
            if(!read(Address(std::intptr_t(address+6)+relative),address))return false;
        }else return false;
    }
    return false;
}
}
inline Statistics statistics() noexcept {return {detail::status.load(),detail::queued.load(),detail::executed.load(),detail::taken.load(),detail::rejected.load()};}
inline void poll(bool luke) noexcept {
    static const bool selected=[] {wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
        const auto* base=wcsrchr(path,L'\\');return _wcsicmp(base?base+1:path,L"DaysGone.exe")==0;}();
    if(!selected||!luke)return;
    if(detail::status.load(std::memory_order_acquire))return;
    try {
        auto& h=detail::hooks();std::scoped_lock lock(h.mutex);if(h.attempted)return;
        h.attempted=true;detail::status=1;
        const auto base=reinterpret_cast<Address>(GetModuleHandleW(nullptr));
        IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
        if(!detail::read(base,dos)||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<=0||dos.e_lfanew>4096||
            !detail::read(base+dos.e_lfanew,nt)||nt.Signature!=IMAGE_NT_SIGNATURE||
            nt.FileHeader.TimeDateStamp!=0x68754d46||nt.OptionalHeader.SizeOfImage!=0x93f0000)return;
        if(!detail::disk_prefix(0x20e6920,visibility_prefix)||!detail::disk_prefix(0x19c0ea0,allocate_prefix))return;
        detail::status=2;
        std::array<unsigned char,24> code{};
        if(!detail::bytes(base+0x19c0ea0,code.data(),allocate_prefix.size())||code!=allocate_prefix)return;
        if(!detail::bytes(base+0x20e6920,code.data(),visibility_prefix.size()))return;
        if(!std::equal(visibility_prefix.begin(),visibility_prefix.end(),code.begin())&&!detail::addon_detour(base+0x20e6920)) {
            detail::status=3;return;
        }
        HMODULE pinned{};
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&poll),&pinned))return;
        h.allocate=reinterpret_cast<Allocate>(base+0x19c0ea0);
        h.hook=safetyhook::create_inline(reinterpret_cast<void*>(base+0x20e6920),reinterpret_cast<void*>(&detail::setup),
            SafetyHookInline::StartDisabled);
        if(!h.hook){detail::status=4;return;}
        h.original=h.hook.original<detail::Setup>();
        if(!h.hook.enable()){h.hook={};h.original=nullptr;detail::status=4;return;}
        detail::status=7;
    }catch(...){detail::status=9;}
}
inline bool take(const daysgone_camera::Binding& binding,std::uint64_t last_sequence,
    daysgone_camera::Packet& packet,daysgone_camera::Rejection& reason) noexcept {
    auto& slot=executing;reason=daysgone_camera::Rejection::clock;
    if(slot.consumed||!slot.current.valid){if(!slot.consumed)reason=slot.current.rejection;++detail::rejected;return false;}
    slot.consumed=true;
    packet=slot.current.packet;
    if(packet.thread!=binding.thread||packet.sequence<=last_sequence){++detail::rejected;return false;}
    if(std::abs(slot.current.view_jitter[0])!=std::abs(binding.jitter_x)||
        std::abs(slot.current.view_jitter[1])!=std::abs(binding.jitter_y)) {
        reason=daysgone_camera::Rejection::jitter;++detail::rejected;return false;
    }
    packet.context=binding.context;packet.feature=binding.feature;packet.color=binding.color;packet.output=binding.output;
    packet.depth=binding.depth;packet.motion=binding.motion;packet.jitter_x=binding.jitter_x;packet.jitter_y=binding.jitter_y;
    const auto& previous=slot.previous;
    if(!slot.current.cut&&previous.valid&&previous.packet.sequence+1==packet.sequence&&previous.packet.frame+1==packet.frame) {
        packet.flags=1;packet.previous_sequence=previous.packet.sequence;packet.previous_frame=previous.packet.frame;
        packet.previous_camera=previous.packet.camera;
    }
    ++detail::taken;return true;
}
}
