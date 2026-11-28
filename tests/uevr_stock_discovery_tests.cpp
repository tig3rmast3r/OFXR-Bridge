#include "xrfg/uevr_stock_bootstrap_discovery.hpp"
#include "uevr_stock_code_fixture.hpp"
#include <cstdio>
#include <cstdlib>

using namespace xrfg::uevr_stock_discovery;
void check(bool ok,const char* message) {if(!ok){std::fprintf(stderr,"%s\n",message);std::exit(1);}}
template<class T> void put(std::vector<std::uint8_t>& image,unsigned at,const T& value) {std::memcpy(image.data()+at,&value,sizeof(T));}
std::vector<std::uint8_t> fixture() {
    // The published 01143 instruction tail, relocated into a PE fixture.
    // Unwind chaining and RIP-relative log discovery match the real image.
    std::vector<std::uint8_t> image(0x4000);
    IMAGE_DOS_HEADER dos{};dos.e_magic=IMAGE_DOS_SIGNATURE;dos.e_lfanew=0x80;put(image,0,dos);
    IMAGE_NT_HEADERS64 nt{};nt.Signature=IMAGE_NT_SIGNATURE;nt.FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.NumberOfSections=3;nt.FileHeader.SizeOfOptionalHeader=sizeof(nt.OptionalHeader);
    nt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt.OptionalHeader.SizeOfImage=unsigned(image.size());
    nt.OptionalHeader.NumberOfRvaAndSizes=16;nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION]={0x3000,24};put(image,0x80,nt);
    for(unsigned i=0;i<3;++i) {
        IMAGE_SECTION_HEADER s{};s.VirtualAddress=(i+1)*0x1000;s.Misc.VirtualSize=0x1000;
        s.Characteristics=IMAGE_SCN_MEM_READ|(i==0?IMAGE_SCN_MEM_EXECUTE:0);put(image,0x80+sizeof(nt)+i*sizeof(s),s);
    }
    std::fill(image.begin()+0x1100,image.begin()+0x11f7,0x90);
    image[0x1100]=0x48;image[0x1101]=0x8d;image[0x1102]=0x0d;put(image,0x1103,std::int32_t(0x2100-0x1107));
    std::memcpy(image.data()+0x2100,anchor,sizeof(anchor));
    const auto& tail=xrfg::uevr_stock_fixture::test_tail;
    std::memcpy(image.data()+0x1175,tail.data(),tail.size());
    put(image,0x3000,RUNTIME_FUNCTION{0x1100,0x1180,0x2200});
    put(image,0x300c,RUNTIME_FUNCTION{0x1180,0x11f7,0x2210});
    image[0x2200]=1;image[0x2210]=1|(4<<3);put(image,0x2214,RUNTIME_FUNCTION{0x1100,0x1180,0x2200});
    return image;
}
int main() {
    auto image=fixture();const auto sites=discover(image);
    check(sites && sites->entry==0x1100 && sites->count==0x11a8 && sites->gate==0x11b0 && sites->accepted==0x11dc,"published relocated tail not discovered");
    // Member layout is not a version key: both independent bool offsets and
    // the count offset can change while the instruction contract stays intact.
    auto changed=image;put(changed,0x11ab,std::int32_t(0x278));put(changed,0x11b4,std::int32_t(0x2a26));put(changed,0x11d3,std::int32_t(0x2a51));
    check(discover(changed).has_value(),"safe layout change rejected");
    for(unsigned at:{0x11b0u,0x11b9u,0x11c3u,0x11cdu,0x11d7u,0x11dau}) {
        auto bad=image;bad[at]^=1;check(!discover(bad),"changed branch/readiness/return admitted");
    }
    auto bad=image;bad[0x2100]='x';check(!discover(bad),"missing diagnostic anchor admitted");
    bad=image;put(bad,0x1103,std::int32_t(0x2101-0x1107));check(!discover(bad),"wrong RIP reference admitted");
    bad=image;bad[0x2210]=1;check(!discover(bad),"unrelated unwind root admitted");
    bad=image;put(bad,0x2214,RUNTIME_FUNCTION{0x1180,0x11f7,0x2210});check(!discover(bad),"cyclic unwind chain admitted");
    bad=image;bad[0x1176]=2;check(!discover(bad),"changed return-one fallback admitted");
    bad=image;bad[0x11e6]=0x90;check(!discover(bad),"changed accepted epilogue admitted");
    bad=image;bad[0x1100]=0xe9;put(bad,0x1101,std::int32_t(0x1500-0x1105));check(!discover(bad),"entry detour admitted");
    bad=image;put(bad,0x80+sizeof(IMAGE_NT_HEADERS64)+offsetof(IMAGE_SECTION_HEADER,Characteristics),DWORD(IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_WRITE));
    check(!discover(bad),"writable code admitted");
    // A duplicate anchored function must not choose whichever happens to scan first.
    bad=image;std::memcpy(bad.data()+0x1300,image.data()+0x1100,0xf7);put(bad,0x1303,std::int32_t(0x2100-0x1307));
    put(bad,0x3018,RUNTIME_FUNCTION{0x1300,0x13f7,0x2200});
    auto nt=IMAGE_NT_HEADERS64{};read(bad,0x80,nt);nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size=36;put(bad,0x80,nt);
    check(!discover(bad),"two matching functions were not rejected as ambiguous");
    for(unsigned length:{0u,64u,0x100u,0x3005u})check(!discover(std::span(image).first(length)),"truncated PE admitted");
    check(supports_sdk(2,34) && supports_sdk(2,39) && supports_sdk(2,41) && !supports_sdk(3,0) && !supports_sdk(2,33),"SDK ABI guard");
    std::puts("UEVR_DISCOVERY relocated official code, changing layouts, all readiness gates, bad returns, RIP anchor, chained unwind, ambiguity and malformed PE PASS");
}
