#include "xrfg/luke_eye_canvas.hpp"
#include "xrfg/luke_guide_window.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

void check(bool value,const char* message) {if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
double ray(double a,double b,unsigned i,unsigned n){return std::tan(a)+(std::tan(b)-std::tan(a))*(i+.5)/n;}
int main() {
    using namespace xrfg;
    const std::array<float,4> left{-0.849466145F,0.837864161F,0.845249832F,-0.842268944F};
    const std::array<float,4> right{-0.849740982F,0.838139057F,0.842333913F,-0.845314801F};
    std::array<float,16> projection{};projection[0]=projection[5]=0.878936768F;
    projection[10]=-0.000001000000907F;projection[11]=0.999999940F;projection[14]=10.00000858F;
    for(bool reverse:{false,true}) {
        auto a=reverse?right:left,b=reverse?left:right;
        unsigned sw=reverse?3369:3367,sh=reverse?3368:3367,dw=reverse?3367:3369,dh=reverse?3367:3368;
        auto p=luke_eye_canvas(sw,sh,dw,dh,a,b,{0,0,sw,sh},{0,0,dw,dh});
        check(p.has_value()&&p->width==3369&&p->height==3368,"actual external pair rejected/unstable canvas");
        for(unsigned eye=0;eye<2;++eye) {
            auto f=eye?b:a,g=eye?p->destination_fov:p->source_fov;unsigned w=eye?dw:sw,h=eye?dh:sh;
            for(unsigned x=0;x<w;++x)check(std::abs(ray(f[0],f[1],x,w)-ray(g[0],g[1],x,p->width))<3e-7,"original horizontal ray moved");
            for(unsigned y=0;y<h;++y)check(std::abs(ray(f[2],f[3],y,h)-ray(g[2],g[3],y,p->height))<3e-7,"original vertical ray moved");
            auto window=luke_guide_window(projection,g[0],g[1],g[2],g[3],3408,3408,1704,1704,p->width,p->height);
            check(window&&window->raster_x&&window->raster_y&&window->width==1685&&window->height==1684,"alternating source changed native working grid/history extent");
        }
    }
    auto same=luke_eye_canvas(3367,3367,3367,3367,left,right);
    check(same&&same->source_fov==left&&same->destination_fov==right,"equal-size FOV changed");
    for(unsigned dx=0;dx<=4;++dx)for(unsigned dy=0;dy<=4;++dy) {
        unsigned w=7,h=5,cw=w+dx,ch=h+dy;auto copies=luke_canvas_copies(w,h,cw,ch);
        std::vector<unsigned> source(w*h),dest(cw*ch,99999);for(unsigned i=0;i<source.size();++i)source[i]=i;
        for(unsigned i=0;i<copies.count;++i){auto r=copies.regions[i];for(unsigned y=r.top;y<r.bottom;++y)for(unsigned x=r.left;x<r.right;++x)
            dest[(r.y+y-r.top)*cw+r.x+x-r.left]=source[y*w+x];}
        for(unsigned y=0;y<ch;++y)for(unsigned x=0;x<cw;++x)check(dest[y*cw+x]==source[std::min(y,h-1)*w+std::min(x,w-1)],"copy shifted real pixels or left undefined border");
    }
    check(!luke_eye_canvas(3367,3367,3372,3367,left,right),"large mismatch admitted");
    check(!luke_eye_canvas(3367,3367,3369,3368,left,right,{1,0,3366,3367}),"unknown offset subrect admitted");
    check(!luke_eye_canvas(3367,3367,3369,3368,left,right,{0,0,3366,3367}),"partial subrect admitted");
    auto bad=left;bad[0]=std::numeric_limits<float>::quiet_NaN();check(!luke_eye_canvas(3367,3367,3369,3368,bad,right),"NaN FOV admitted");
    bad=left;bad[0]=bad[1];check(!luke_eye_canvas(3367,3367,3369,3368,bad,right),"invalid FOV admitted");
    check(!luke_eye_canvas(0,3367,3369,3368,left,right),"zero extent admitted");
    check(luke_canvas_copies(5,5,4,5).count==0,"negative padding admitted");
    std::puts("PASS: actual two-eye geometry, per-pixel rays, stable guide grids, exact copies/borders, unchanged equal sizes and unsafe-input refusals");
}
