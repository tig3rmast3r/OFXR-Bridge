#include "xrfg/luke_gta_eye_reference.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
using M=std::array<double,16>;
void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
M identity(){M m{};m[0]=m[5]=m[10]=m[15]=1;return m;}
M product(M a,M b){M m{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)for(unsigned k=0;k<4;++k)m[r*4+c]+=a[r*4+k]*b[k*4+c];return m;}
M root(double t){auto m=identity();const double a=.015*t,c=std::cos(a),s=std::sin(a);m[0]=m[10]=c;m[2]=s;m[8]=-s;m[7]=.02*t;return m;}
M reference(unsigned eye,bool split){auto m=identity();if(!split)return m;const double a=eye?.025:-.025,c=std::cos(a),s=std::sin(a);m[5]=m[10]=c;m[6]=-s;m[9]=s;m[3]=eye?.03:-.03;m[11]=eye?-.02:.02;return m;}
xrfg::Quaternion head(double t){return {0,float(std::sin(.02*t*.5)),0,float(std::cos(.02*t*.5))};}
M head_matrix(double t,unsigned eye){auto m=identity();const double a=.02*t,c=std::cos(a),s=std::sin(a);m[0]=m[10]=c;m[2]=s;m[8]=-s;if(!eye){m[3]=-.064*c;m[11]=.064*s;}return m;}
M native(double t,unsigned eye,bool split){return product(product(root(t),reference(eye,split)),head_matrix(t,eye));}
xrfg::LukeGtaEyeCamera input(double t,unsigned eye,bool split){xrfg::LukeGtaEyeCamera v;const auto m=native(t,eye,split);for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)v.view_to_world[c*4+r]=float(m[r*4+c]);v.xr_orientation=head(t);v.eye=eye;return v;}
}
int main(){try {
 for(unsigned eye=0;eye<2;++eye)for(const double peer_time:{1.,1.4}) {
  const double current=peer_time==1?2:2.7;
  auto c=input(current,eye,true),s=input(0,eye,true),p=input(peer_time,1-eye,true);
  const auto original=c.view_to_world;
  const auto result=xrfg::luke_gta_peer_eye_reference(c,s,p,.064F,float((current-peer_time)/current));
  check(bool(result),"alternating native-eye reference not detected");
  const auto expected=product(product(root(current),reference(1-eye,true)),head_matrix(peer_time,1-eye));
  double error{};for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col)error=std::max(error,std::abs(double((*result)[col*4+r])-expected[r*4+col]));
  check(error<2e-6,"peer reference / constant rigid gameplay motion is not preserved");
  check(c.view_to_world==original,"real source camera was mutated");
  // V700 transfers the source-eye reference; this independent scene oracle
  // must distinguish it from the destination eye's native reference.
  const auto old=product(product(root(current),reference(eye,true)),head_matrix(peer_time,1-eye));
  double old_error{};for(unsigned i=0;i<16;++i)old_error=std::max(old_error,std::abs(old[i]-expected[i]));
  check(old_error>.03,"negative oracle cannot distinguish the old shared-reference target");
  c=input(current,eye,false);s=input(0,eye,false);p=input(peer_time,1-eye,false);
  check(!xrfg::luke_gta_peer_eye_reference(c,s,p,.064F,.5F),"ordinary monotonic scene must retain V700 target");
  c=input(current,eye,true);s=input(0,eye,true);p=input(peer_time,1-eye,true);
  auto bad=c;bad.view_to_world[0]=std::numeric_limits<float>::quiet_NaN();
  check(!xrfg::luke_gta_peer_eye_reference(bad,s,p,.064F,.5F),"nonfinite camera admitted");
  bad=c;bad.view_to_world[0]*=-1;bad.view_to_world[1]*=-1;bad.view_to_world[2]*=-1;
  check(!xrfg::luke_gta_peer_eye_reference(bad,s,p,.064F,.5F),"reflected camera admitted");
  bad=s;bad.eye=1-eye;check(!xrfg::luke_gta_peer_eye_reference(c,bad,p,.064F,.5F),"cross-eye same history admitted");
  check(!xrfg::luke_gta_peer_eye_reference(c,s,p,0,.5F),"zero baseline admitted");
  check(!xrfg::luke_gta_peer_eye_reference(c,s,p,.064F,0),"zero interval admitted");
  check(!xrfg::luke_gta_peer_eye_reference(c,s,p,.064F,1),"unbounded interval admitted");
  // Alternating physical eye/cant or a genuine head reversal is not the
  // monotonic-head / alternating-native-reference signature from the dumps.
  auto reversed=p;reversed.xr_orientation=head(-peer_time);
  check(!xrfg::luke_gta_peer_eye_reference(c,s,reversed,.064F,.5F),"physical head reversal admitted");
 }
 std::cout<<"GTA native eye references, both eyes, independent rigid-motion oracle, uneven intervals and V700 monotonic fallback PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
