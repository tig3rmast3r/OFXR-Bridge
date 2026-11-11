#include "xrfg/luke_origin_dark_marker.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
using namespace xrfg;
unsigned checks{};
void require(bool b,const char* m){++checks;if(!b)throw std::runtime_error(m);}
struct Image {
    unsigned w=320,h=48;std::vector<std::byte> bytes;
    Image():bytes(w*h*4,std::byte{64}){}
    void value(unsigned x,unsigned y,unsigned v){for(unsigned c=0;c<3;++c)bytes[(y*w+x)*4+c]=std::byte(v);}
    void band(unsigned start=24,unsigned width=256,bool residual=true) {
        for(unsigned y=12;y<20;++y)for(unsigned x=0;x<width&&start+x<w;++x) {
            value(start+x,y,0);if(residual&&luke_origin_white(12345,x))bytes[(y*w+start+x)*4+2]=std::byte{1};
        }
    }
    LukeColorOriginImage view(){return {bytes.data(),DXGI_FORMAT_R8G8B8A8_UNORM,w,h,w*4};}
};
int main(int argc,char** argv){try {
    if(argc==6) {
        std::ifstream f(argv[1],std::ios::binary);std::vector<char>b((std::istreambuf_iterator<char>(f)),{});
        unsigned w=std::stoul(argv[2]),h=std::stoul(argv[3]),pitch=std::stoul(argv[4]);auto fmt=static_cast<DXGI_FORMAT>(std::stoul(argv[5]));
        require(b.size()>=std::uint64_t(h-1)*pitch+w*luke_origin_pixel_bytes(fmt),"truncated capture");
        auto d=classify_luke_dark_marker({reinterpret_cast<const std::byte*>(b.data()),fmt,w,h,pitch});
        std::cout<<"capture dark="<<d.recognized<<" rect="<<d.x<<','<<d.y<<','<<d.width<<','<<d.height<<'\n';
        return d.recognized?0:1;
    }
    Image a;a.band();auto d=classify_luke_dark_marker(a.view());
    require(d.recognized,"dim marker residual not recognized");require(d.x==24&&d.y==12&&d.width==256&&d.height==8,"wrong rectangle");
    Image black;std::fill(black.bytes.begin(),black.bytes.end(),std::byte{});require(!classify_luke_dark_marker(black.view()).recognized,"all-black frame recognized");
    Image plain;plain.band(24,256,false);require(!classify_luke_dark_marker(plain.view()).recognized,"unknown black rectangle guessed as marker");
    LukeColorOriginDecode known{12345,false,12,19,24,280};
    require(classify_luke_dark_marker(plain.view(),&known).recognized,"known complete marker footprint not recognized");
    known.first_column+=10;known.last_column+=10;require(!classify_luke_dark_marker(plain.view(),&known).recognized,"wrong known location trusted");
    Image clipped;clipped.band(150);require(!classify_luke_dark_marker(clipped.view()).recognized,"clipped marker triggers brightness");
    Image wide;wide.band(10,280);require(!classify_luke_dark_marker(wide.view()).recognized,"wrong footprint triggers brightness");
    Image corrupt;corrupt.band();for(unsigned y=12;y<20;++y)for(unsigned x=24;x<28;++x)corrupt.value(x,y,0);
    require(!classify_luke_dark_marker(corrupt.view()).recognized,"corrupt unknown sync triggers brightness");
    Image bright;bright.band();for(unsigned y=12;y<20;++y)for(unsigned x=24;x<280;++x)
        if(luke_origin_white(12345,x-24))bright.value(x,y,100);
    require(!classify_luke_dark_marker(bright.view()).recognized,"high-contrast bad CRC treated as black marker");
    Image covered;covered.band();for(unsigned y=12;y<20;++y)for(unsigned x=100;x<130;++x)covered.value(x,y,80);
    require(!classify_luke_dark_marker(covered.view()).recognized,"partial overlay treated as black marker");
    Image border;border.band();for(unsigned x=0;x<320;++x)for(unsigned y=0;y<12;++y)border.value(x,y,0);
    require(!classify_luke_dark_marker(border.view()).recognized,"black bar connected to marker accepted");
    auto invalid=a.view();invalid.row_pitch=1;require(!classify_luke_dark_marker(invalid).recognized,"invalid pitch accepted");
    invalid=a.view();invalid.format=DXGI_FORMAT_UNKNOWN;require(!classify_luke_dark_marker(invalid).recognized,"unknown format accepted");
    std::cout<<checks<<" dark-classification checks PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
