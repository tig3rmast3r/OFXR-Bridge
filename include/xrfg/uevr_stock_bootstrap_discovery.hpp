#pragma once
#include <windows.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace xrfg::uevr_stock_discovery {
// Discovery runs once on CPU. RVAs and C++ member offsets are learned from
// code, never used to dereference private UEVR objects. Unknown shapes fail closed.
inline constexpr char anchor[]="get desired number of views hook called!";
// The callback/get_mod_value prefix used here is present in stable 1.05
// (SDK 2.34). SDK 2 appends fields; a different major remains incompatible.
inline bool supports_sdk(int major,int minor) noexcept {return major==2 && minor>=34;}
struct Sites {std::uint32_t entry{},end{},count{},gate{},accepted{};};
struct Instruction {
    std::uint32_t rva{};
    ZydisDecodedInstruction decoded{};
    std::array<ZydisDecodedOperand,ZYDIS_MAX_OPERAND_COUNT> operands{};
};
inline bool range(std::span<const std::uint8_t> bytes,std::size_t start,std::size_t size) {
    return start<=bytes.size() && size<=bytes.size()-start;
}
template<class T> inline bool read(std::span<const std::uint8_t> bytes,std::size_t start,T& out) {
    if(!range(bytes,start,sizeof(T)))return false;
    std::memcpy(&out,bytes.data()+start,sizeof(T));return true;
}
inline ZydisRegister root(ZydisRegister r) {return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64,r);}
inline bool nonvolatile(ZydisRegister r) {
    return r==ZYDIS_REGISTER_RBX || r==ZYDIS_REGISTER_RBP || r==ZYDIS_REGISTER_RSI || r==ZYDIS_REGISTER_RDI ||
        (r>=ZYDIS_REGISTER_R12 && r<=ZYDIS_REGISTER_R15);
}
inline bool reg(const ZydisDecodedOperand& o,ZydisRegister r) {return o.type==ZYDIS_OPERAND_TYPE_REGISTER && o.reg.value==r;}
inline bool imm(const ZydisDecodedOperand& o,std::uint64_t v) {return o.type==ZYDIS_OPERAND_TYPE_IMMEDIATE && !o.imm.is_relative && o.imm.value.u==v;}
inline bool member(const ZydisDecodedOperand& o,unsigned size,ZydisRegister base) {
    return o.type==ZYDIS_OPERAND_TYPE_MEMORY && o.size==size && o.mem.base==base &&
        o.mem.index==ZYDIS_REGISTER_NONE && o.mem.disp.has_displacement && o.mem.disp.value>0 && o.mem.disp.value<0x10000;
}
inline std::optional<std::uint32_t> target(const Instruction& i) {
    if(i.operands[0].type!=ZYDIS_OPERAND_TYPE_IMMEDIATE || !i.operands[0].imm.is_relative)return {};
    const auto rva=std::int64_t(i.rva)+i.decoded.length+i.operands[0].imm.value.s;
    if(rva<0 || rva>UINT32_MAX)return {};return std::uint32_t(rva);
}
inline std::vector<Instruction> decode(std::span<const std::uint8_t> image,std::uint32_t begin,std::uint32_t end) {
    std::vector<Instruction> out;
    if(begin>=end || end-begin>4096 || !range(image,begin,end-begin))return out;
    ZydisDecoder decoder;ZydisDecoderInit(&decoder,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64);
    for(auto pos=begin;pos<end;) {
        Instruction i;i.rva=pos;
        if(!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder,image.data()+pos,end-pos,&i.decoded,i.operands.data())))return {};
        out.push_back(i);pos+=i.decoded.length;
    }
    return out;
}
inline std::optional<std::uint32_t> epilogue(const std::vector<Instruction>& code,std::size_t n);
inline bool zero_register(const std::vector<Instruction>& code,std::size_t before,ZydisRegister wanted) {
    // A nonvolatile zero established in the entry basic block survives calls.
    // Any other write on ANY intervening path invalidates that proof.
    wanted=root(wanted);if(!nonvolatile(wanted))return false;
    bool established=false,entry=true;
    for(std::size_t n=0;n<before;++n) {
        const auto& i=code[n];
        if(i.decoded.meta.category==ZYDIS_CATEGORY_COND_BR || i.decoded.meta.category==ZYDIS_CATEGORY_UNCOND_BR ||
            i.decoded.meta.category==ZYDIS_CATEGORY_CALL)entry=false;
        for(unsigned p=0;p<i.decoded.operand_count;++p) {
            const auto& o=i.operands[p];
            if(o.type!=ZYDIS_OPERAND_TYPE_REGISTER || root(o.reg.value)!=wanted || !(o.actions&ZYDIS_OPERAND_ACTION_MASK_WRITE))continue;
            if(entry && i.decoded.mnemonic==ZYDIS_MNEMONIC_XOR && i.operands[0].size>=32 &&
                i.operands[1].type==ZYDIS_OPERAND_TYPE_REGISTER && i.operands[0].reg.value==i.operands[1].reg.value)established=true;
            // Restores on a terminating return path cannot reach the gate.
            else if(!epilogue(code,n))return false;
        }
    }
    return established;
}
inline std::optional<std::uint32_t> epilogue(const std::vector<Instruction>& code,std::size_t n) {
    if(n>=code.size())return {};
    for(unsigned steps=0;steps<10 && n<code.size();++steps,++n) {
        const auto& i=code[n];const auto& a=i.operands[0];const auto& b=i.operands[1];
        if(i.decoded.mnemonic==ZYDIS_MNEMONIC_RET && i.decoded.operand_count_visible==0)return i.rva+i.decoded.length;
        if(i.decoded.mnemonic==ZYDIS_MNEMONIC_POP && a.type==ZYDIS_OPERAND_TYPE_REGISTER && nonvolatile(a.reg.value))continue;
        if(i.decoded.mnemonic==ZYDIS_MNEMONIC_ADD && reg(a,ZYDIS_REGISTER_RSP) && b.type==ZYDIS_OPERAND_TYPE_IMMEDIATE && b.imm.value.u>0 && b.imm.value.u<0x10000)continue;
        if(i.decoded.mnemonic==ZYDIS_MNEMONIC_MOV && a.type==ZYDIS_OPERAND_TYPE_REGISTER && nonvolatile(a.reg.value) &&
            b.type==ZYDIS_OPERAND_TYPE_MEMORY && b.mem.base==ZYDIS_REGISTER_RSP && b.mem.index==ZYDIS_REGISTER_NONE)continue;
        return {};
    }
    return {};
}
inline std::optional<Sites> validate(std::span<const std::uint8_t> image,std::uint32_t begin,std::uint32_t end) {
    const auto code=decode(image,begin,end);std::optional<Sites> found;
    // Reject entry detours and malformed internal branches. Calls keep their
    // original external destinations; no function entry hook is installed.
    for(const auto& i:code)if(i.decoded.meta.category==ZYDIS_CATEGORY_COND_BR || i.decoded.meta.category==ZYDIS_CATEGORY_UNCOND_BR) {
        const auto to=target(i);
        if(!to || !std::any_of(code.begin(),code.end(),[&](const auto& dst){return dst.rva==*to;}))return {};
    }
    for(std::size_t n=1;n+13<code.size();++n) {
        const auto& count=code[n];
        if(count.operands[0].type!=ZYDIS_OPERAND_TYPE_MEMORY)continue;
        const auto base=count.operands[0].mem.base;
        if(count.decoded.mnemonic!=ZYDIS_MNEMONIC_CMP || !member(count.operands[0],64,base) ||
            !imm(count.operands[1],2) || (base!=ZYDIS_REGISTER_RCX && base!=ZYDIS_REGISTER_RDX))continue;
        const auto& branch=code[n+1];const auto fail=target(branch);
        if(branch.decoded.mnemonic!=ZYDIS_MNEMONIC_JNB || !fail || code[n-1].decoded.mnemonic!=ZYDIS_MNEMONIC_JNZ || target(code[n-1])!=fail)continue;
        bool guards=true;std::array<std::int64_t,3> offsets{};
        for(unsigned g=0;g<3;++g) {
            const auto& cmp=code[n+2+g*2];const auto& jump=code[n+3+g*2];
            const auto& value=cmp.operands[1];
            const bool zero=imm(value,0) || (value.type==ZYDIS_OPERAND_TYPE_REGISTER && zero_register(code,n+2+g*2,value.reg.value));
            if(cmp.decoded.mnemonic!=ZYDIS_MNEMONIC_CMP || !member(cmp.operands[0],g?64:8,base) || !zero ||
                jump.decoded.mnemonic!=ZYDIS_MNEMONIC_JZ || target(jump)!=fail) {guards=false;break;}
            offsets[g]=cmp.operands[0].mem.disp.value;
        }
        if(!guards || offsets[2]!=offsets[1]+8 || offsets[0]==offsets[1] || offsets[0]==offsets[2] ||
            count.operands[0].mem.disp.value==offsets[1] || count.operands[0].mem.disp.value==offsets[2])continue;
        std::size_t last=n+8;
        const bool eax=code[last].decoded.mnemonic==ZYDIS_MNEMONIC_XOR && reg(code[last].operands[0],ZYDIS_REGISTER_EAX) && reg(code[last].operands[1],ZYDIS_REGISTER_EAX);
        if(eax)++last;
        const auto& ext=code[last++];const auto& set=code[last++];const auto& plus=code[last++];
        if(ext.decoded.mnemonic!=ZYDIS_MNEMONIC_CMP || !member(ext.operands[0],8,base) ||
            std::find(offsets.begin(),offsets.end(),ext.operands[0].mem.disp.value)!=offsets.end() ||
            ext.operands[0].mem.disp.value==count.operands[0].mem.disp.value || set.decoded.mnemonic!=ZYDIS_MNEMONIC_SETNZ)continue;
        if(eax) {
            if(!reg(ext.operands[1],ZYDIS_REGISTER_AL) || !reg(set.operands[0],ZYDIS_REGISTER_AL) ||
                plus.decoded.mnemonic!=ZYDIS_MNEMONIC_INC || !reg(plus.operands[0],ZYDIS_REGISTER_EAX))continue;
        } else {
            const auto z=ext.operands[1];
            if(z.type!=ZYDIS_OPERAND_TYPE_REGISTER || !zero_register(code,n+8,z.reg.value) || !reg(set.operands[0],z.reg.value) ||
                plus.decoded.mnemonic!=ZYDIS_MNEMONIC_LEA || !reg(plus.operands[0],ZYDIS_REGISTER_EAX) ||
                plus.operands[1].type!=ZYDIS_OPERAND_TYPE_MEMORY || root(plus.operands[1].mem.base)!=root(z.reg.value) ||
                plus.operands[1].mem.index!=ZYDIS_REGISTER_NONE || plus.operands[1].mem.disp.value!=1)continue;
        }
        const auto finish=epilogue(code,last);if(!finish)continue;
        const auto failure=std::find_if(code.begin(),code.end(),[&](const auto& i){return i.rva==*fail;});
        if(failure==code.end() || failure->decoded.mnemonic!=ZYDIS_MNEMONIC_MOV || !reg(failure->operands[0],ZYDIS_REGISTER_EAX) || !imm(failure->operands[1],1))continue;
        const auto fail_next=std::size_t(failure-code.begin())+1;
        const auto fail_end=epilogue(code,fail_next);
        if(!fail_end || *finish-code[last].rva!=*fail_end-code[fail_next].rva ||
            std::memcmp(image.data()+code[last].rva,image.data()+code[fail_next].rva,*finish-code[last].rva))continue;
        if(found)return {}; // multiple plausible count gates are ambiguous
        found=Sites{begin,end,count.rva,branch.rva,code[last].rva};
    }
    return found;
}

inline std::optional<Sites> discover(std::span<const std::uint8_t> image) {
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
    if(!read(image,0,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || !read(image,dos.e_lfanew,nt) ||
        nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage!=image.size() ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections>96 ||
        nt.FileHeader.SizeOfOptionalHeader<sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_EXCEPTION)return {};
    std::vector<IMAGE_SECTION_HEADER> sections;
    const auto table=std::size_t(dos.e_lfanew)+24+nt.FileHeader.SizeOfOptionalHeader;
    for(unsigned n=0;n<nt.FileHeader.NumberOfSections;++n) {
        IMAGE_SECTION_HEADER s{};
        if(!read(image,table+n*sizeof(s),s) || !range(image,s.VirtualAddress,s.Misc.VirtualSize))return {};
        sections.push_back(s);
    }
    const auto executable=[&](std::uint32_t a,std::uint32_t b) {
        return std::any_of(sections.begin(),sections.end(),[&](const auto& s){return (s.Characteristics&IMAGE_SCN_MEM_EXECUTE) &&
            !(s.Characteristics&IMAGE_SCN_MEM_WRITE) && a>=s.VirtualAddress && b>a && b<=std::uint64_t(s.VirtualAddress)+s.Misc.VirtualSize;});
    };
    std::vector<std::uint32_t> anchors;
    for(const auto& s:sections)if((s.Characteristics&IMAGE_SCN_MEM_READ) && !(s.Characteristics&(IMAGE_SCN_MEM_WRITE|IMAGE_SCN_MEM_EXECUTE))) {
        for(std::size_t n=s.VirtualAddress;n+sizeof(anchor)<=std::size_t(s.VirtualAddress)+s.Misc.VirtualSize;++n)
            if(image[n]==anchor[0] && !std::memcmp(image.data()+n,anchor,sizeof(anchor)))anchors.push_back(std::uint32_t(n));
    }
    if(anchors.empty())return {};
    const auto directory=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if(!range(image,directory.VirtualAddress,directory.Size) || directory.Size%sizeof(RUNTIME_FUNCTION))return {};
    struct Part {std::uint32_t begin,end,root;};std::vector<Part> parts;
    for(unsigned offset=0;offset<directory.Size;offset+=sizeof(RUNTIME_FUNCTION)) {
        RUNTIME_FUNCTION f{};if(!read(image,directory.VirtualAddress+offset,f) || !executable(f.BeginAddress,f.EndAddress))return {};
        const auto begin=f.BeginAddress,end=f.EndAddress;bool valid=false;
        for(unsigned depth=0;depth<8;++depth) {
            std::array<std::uint8_t,4> unwind{};if(!read(image,f.UnwindData,unwind) || (unwind[0]&7)!=1)break;
            if(!(unwind[0]>>3&4)){valid=true;break;}
            const auto next=std::size_t(f.UnwindData)+4+((unsigned(unwind[2])+1)&~1u)*2;
            RUNTIME_FUNCTION parent{};if(!read(image,next,parent) || parent.BeginAddress>=f.BeginAddress || !executable(parent.BeginAddress,parent.EndAddress))break;
            f=parent;
        }
        // Other functions (for example CRT v2 unwind records) are unrelated.
        // Keep their boundaries searchable, but never admit them as candidates.
        if(!parts.empty() && begin<parts.back().end)return {};
        parts.push_back({begin,end,valid?f.BeginAddress:0});
    }
    std::vector<std::uint32_t> candidates;
    for(const auto& s:sections)if(executable(s.VirtualAddress,s.VirtualAddress+s.Misc.VirtualSize)) {
        for(auto at=s.VirtualAddress;std::uint64_t(at)+7<=std::uint64_t(s.VirtualAddress)+s.Misc.VirtualSize;++at) {
            // RIP-relative LEA to the diagnostic is only a candidate; decode
            // it at its real instruction boundary and prove the function below.
            if((image[at]&0xf8)!=0x48 || image[at+1]!=0x8d || (image[at+2]&0xc7)!=5)continue;
            std::int32_t disp{};read(image,at+3,disp);const auto to=std::int64_t(at)+7+disp;
            if(std::find(anchors.begin(),anchors.end(),to)==anchors.end())continue;
            const auto part=std::upper_bound(parts.begin(),parts.end(),at,[](auto rva,const auto& p){return rva<p.begin;});
            if(part==parts.begin())continue;
            const auto& p=*(part-1);if(at>=p.end || !p.root)continue;
            auto end=p.end;for(const auto& other:parts)if(other.root==p.root)end=std::max(end,other.end);
            if(end-p.root>4096 || !executable(p.root,end))continue;
            const auto code=decode(image,p.root,end);
            if(std::any_of(code.begin(),code.end(),[&](const auto& i){return i.rva==at && i.decoded.mnemonic==ZYDIS_MNEMONIC_LEA;}))candidates.push_back(p.root);
        }
    }
    std::sort(candidates.begin(),candidates.end());candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
    std::optional<Sites> result;
    for(const auto begin:candidates) {
        auto end=begin;for(const auto& p:parts)if(p.root==begin)end=std::max(end,p.end);
        if(const auto sites=validate(image,begin,end)) {if(result)return {};result=sites;}
    }
    return result;
}
} // namespace xrfg::uevr_stock_discovery
