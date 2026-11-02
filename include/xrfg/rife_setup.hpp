#pragma once
#include "xrfg/standalone_launcher.hpp"
#include <windows.h>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace xrfg::rife_setup {
enum class Status { ready, missing, repair, unsupported, busy, error };
struct Result {
    Status status{Status::error};
    std::wstring message;
    std::filesystem::path root, log_path;
    std::wstring backend;
    std::uint64_t installed_bytes{}, required_free_bytes{};
};
inline bool offers_install(Status status) noexcept {
    return status == Status::missing || status == Status::repair;
}
inline DWORD expected_exit(Status status) noexcept {
    switch(status) {
    case Status::ready: return 0;
    case Status::missing: return 10;
    case Status::repair: return 11;
    case Status::unsupported: return 12;
    case Status::busy: return 13;
    default: return 1;
    }
}
inline std::wstring utf8_to_wide(std::string_view text) {
    if(text.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!count)return {};
    std::wstring out(count,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),count);
    return out;
}
// The helper contract is a flat UTF-8 JSON object. Reject malformed/duplicate
// fields and unknown statuses rather than turning a failed check into Install.
class JsonObject {
    std::string_view text_;std::size_t at_{};
    void whitespace(){while(at_<text_.size() && (text_[at_]==' '||text_[at_]=='\r'||text_[at_]=='\n'||text_[at_]=='\t'))++at_;}
    bool take(char c){whitespace();if(at_==text_.size()||text_[at_]!=c)return false;++at_;return true;}
    bool hex(unsigned& value) {
        value=0;if(text_.size()-at_<4)return false;
        for(unsigned i=0;i<4;++i){const char c=text_[at_++];unsigned digit;
            if(c>='0'&&c<='9')digit=c-'0';else if(c>='a'&&c<='f')digit=c-'a'+10;
            else if(c>='A'&&c<='F')digit=c-'A'+10;else return false;value=value*16+digit;}
        return true;
    }
    static void utf8(std::string& out,unsigned c) {
        if(c<0x80)out+=char(c);
        else if(c<0x800){out+=char(0xc0|(c>>6));out+=char(0x80|(c&63));}
        else if(c<0x10000){out+=char(0xe0|(c>>12));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
        else{out+=char(0xf0|(c>>18));out+=char(0x80|((c>>12)&63));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
    }
    bool string(std::string& out) {
        if(!take('"'))return false;
        while(at_<text_.size()) {
            const unsigned char c=text_[at_++];if(c=='"')return true;if(c<0x20)return false;
            if(c!='\\'){out+=char(c);continue;}
            if(at_==text_.size())return false;
            const char e=text_[at_++];
            switch(e){case '"':case '\\':case '/':out+=e;break;
            case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u':{unsigned code;if(!hex(code))return false;
                if(code>=0xd800&&code<=0xdbff){if(text_.size()-at_<2||text_.substr(at_,2)!="\\u")return false;at_+=2;unsigned low;
                    if(!hex(low)||low<0xdc00||low>0xdfff)return false;code=0x10000+((code-0xd800)<<10)+(low-0xdc00);}
                else if(code>=0xdc00&&code<=0xdfff)return false;
                if(code==0)return false;utf8(out,code);break;}
            default:return false;}
        }
        return false;
    }
    static bool bare(std::string_view value) {
        if(value=="true"||value=="false"||value=="null")return true;
        // This flat contract has only non-negative integer numeric fields.
        // Do not accept identifiers, signs, fractions or leading zeroes.
        if(value.empty()||(value.size()>1&&value.front()=='0'))return false;
        for(const char c:value)if(c<'0'||c>'9')return false;
        return true;
    }
public:
    struct Value { std::string text;bool quoted{}; };
    explicit JsonObject(std::string_view text):text_(text){if(text_.starts_with("\xef\xbb\xbf"))at_=3;}
    std::optional<std::map<std::string,Value>> parse() {
        std::map<std::string,Value> fields;if(!take('{'))return {};
        whitespace();if(take('}'))return fields;
        do {
            std::string key;Value value;if(!string(key)||!take(':'))return {};whitespace();
            if(at_<text_.size()&&text_[at_]=='"'){value.quoted=true;if(!string(value.text))return {};}
            else {const auto start=at_;while(at_<text_.size()&&text_[at_]!=','&&text_[at_]!='}'&&text_[at_]!=' '&&text_[at_]!='\r'&&text_[at_]!='\n'&&text_[at_]!='\t')++at_;
                const auto token=text_.substr(start,at_-start);if(!bare(token))return {};value.text=std::string(token);}
            if(!fields.emplace(std::move(key),std::move(value)).second)return {};
            whitespace();if(take('}')){whitespace();return at_==text_.size()?std::optional(fields):std::nullopt;}
        }while(take(','));
        return {};
    }
};
inline std::optional<Result> parse_result(std::string_view json,DWORD exit_code) {
    if(json.size()>64*1024)return {};
    auto fields=JsonObject(json).parse();if(!fields)return {};
    const auto field=[&](const char* key,bool quoted)->const std::string* {
        const auto found=fields->find(key);if(found==fields->end()||found->second.quoted!=quoted)return nullptr;
        return &found->second.text;
    };
    const auto schema=field("schema",false);if(!schema||*schema!="1")return {};
    const auto status=field("status",true),message=field("message",true),backend=field("backend",true),
        root=field("root",true),log=field("log_path",true);
    if(!status||!message||!backend||!root||!log)return {};
    Result result;const auto& name=*status;
    if(name=="ready")result.status=Status::ready;else if(name=="missing")result.status=Status::missing;
    else if(name=="repair")result.status=Status::repair;else if(name=="unsupported")result.status=Status::unsupported;
    else if(name=="busy")result.status=Status::busy;else if(name!="error")return {};
    if(expected_exit(result.status)!=exit_code)return {};
    result.message=utf8_to_wide(*message);result.backend=utf8_to_wide(*backend);
    result.root=std::filesystem::u8path(*root);result.log_path=std::filesystem::u8path(*log);
    if((!result.root.empty()&&!result.root.is_absolute())||(!result.log_path.empty()&&!result.log_path.is_absolute()))return {};
    const auto number=[&](const char* key,std::uint64_t& value){const auto text=field(key,false);
        if(!text||text->empty())return false;const auto parsed=std::from_chars(text->data(),text->data()+text->size(),value);
        return parsed.ec==std::errc{}&&parsed.ptr==text->data()+text->size();};
    if(!number("installed_bytes",result.installed_bytes)||!number("required_free_bytes",result.required_free_bytes))return {};
    return result;
}
inline std::wstring command_line(const std::filesystem::path& powershell,const std::filesystem::path& script,
    const std::filesystem::path& root,const std::filesystem::path& result,bool check) {
    auto command=standalone::quote_windows_argument(powershell.wstring())+
        L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+standalone::quote_windows_argument(script.wstring());
    if(check)command+=L" -Check";
    return command+L" -Root "+standalone::quote_windows_argument(root.wstring())+
        L" -ResultJson "+standalone::quote_windows_argument(result.wstring());
}
inline constexpr DWORD process_flags(bool install) noexcept {return install?CREATE_NEW_CONSOLE:CREATE_NO_WINDOW;}
inline constexpr ULONGLONG operation_timeout(bool install) noexcept {return install?2ULL*60*60*1000:180000;}
} // namespace xrfg::rife_setup
