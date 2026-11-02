// Only compiled into the native-ingress test executable; never shipping DLL.
struct DispatchParameters final:NVSDK_NGX_Parameter {
 bool present{true};int flags{NVSDK_NGX_DLSS_Feature_Flags_MVLowRes};
 int reset{};
 float scale_x{1},scale_y{1},jitter_x{},jitter_y{},frame_time{16.6667F};
 unsigned width{1280},height{720},out_width{2560},out_height{1440};
 unsigned render_width{},render_height{};
 ID3D12Resource *output{},*motion{},*depth{},*input_color{};
 void Set(const char*, unsigned long long ) override {}
 NVSDK_NGX_Result Get(const char* key, unsigned long long* value) const override {
 return NVSDK_NGX_Result_FAIL_InvalidParameter;}
 void Set(const char*, float ) override {}
 NVSDK_NGX_Result Get(const char* key, float* value) const override {
 if(!std::strcmp(key,NVSDK_NGX_Parameter_MV_Scale_X))*value=scale_x;
 else if(!std::strcmp(key,NVSDK_NGX_Parameter_MV_Scale_Y))*value=scale_y;
 else if(!std::strcmp(key,NVSDK_NGX_Parameter_Jitter_Offset_X))*value=jitter_x;
 else if(!std::strcmp(key,NVSDK_NGX_Parameter_Jitter_Offset_Y))*value=jitter_y;
 else if(!std::strcmp(key,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec))*value=frame_time;
 else return NVSDK_NGX_Result_FAIL_InvalidParameter;
 return NVSDK_NGX_Result_Success;}
 void Set(const char*, double ) override {}
 NVSDK_NGX_Result Get(const char* key, double* value) const override {
 return NVSDK_NGX_Result_FAIL_InvalidParameter;}
 void Set(const char*, unsigned int ) override {}
 NVSDK_NGX_Result Get(const char* key, unsigned int* value) const override {
 if(render_width&&!std::strcmp(key,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width)){*value=render_width;return NVSDK_NGX_Result_Success;}
 if(render_height&&!std::strcmp(key,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height)){*value=render_height;return NVSDK_NGX_Result_Success;}
 if(present){if(!std::strcmp(key,NVSDK_NGX_Parameter_Width))*value=width;else if(!std::strcmp(key,NVSDK_NGX_Parameter_Height))*value=height;else if(!std::strcmp(key,NVSDK_NGX_Parameter_OutWidth))*value=out_width;else if(!std::strcmp(key,NVSDK_NGX_Parameter_OutHeight))*value=out_height;else return NVSDK_NGX_Result_FAIL_InvalidParameter;return NVSDK_NGX_Result_Success;}
 return NVSDK_NGX_Result_FAIL_InvalidParameter;}
 void Set(const char*, int ) override {}
 NVSDK_NGX_Result Get(const char* key, int* value) const override {
 if(!std::strcmp(key,NVSDK_NGX_Parameter_Reset)){*value=reset;return NVSDK_NGX_Result_Success;}
 if(present&&!std::strcmp(key,NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags)){*value=flags;return NVSDK_NGX_Result_Success;}
 return NVSDK_NGX_Result_FAIL_InvalidParameter;}
 void Set(const char*, ID3D11Resource * ) override {}
 NVSDK_NGX_Result Get(const char* key, ID3D11Resource ** value) const override {
 return NVSDK_NGX_Result_FAIL_InvalidParameter;}
 void Set(const char*, ID3D12Resource * ) override {}
 NVSDK_NGX_Result Get(const char* key, ID3D12Resource ** value) const override {
 if(!std::strcmp(key,NVSDK_NGX_Parameter_Output)&&output){*value=output;return NVSDK_NGX_Result_Success;}
 if(!std::strcmp(key,NVSDK_NGX_Parameter_MotionVectors)&&motion){*value=motion;return NVSDK_NGX_Result_Success;}
 if(!std::strcmp(key,NVSDK_NGX_Parameter_Depth)&&depth){*value=depth;return NVSDK_NGX_Result_Success;}
 if(!std::strcmp(key,NVSDK_NGX_Parameter_Color)&&input_color){*value=input_color;return NVSDK_NGX_Result_Success;}
 return NVSDK_NGX_Result_FAIL_InvalidParameter;}
 void Set(const char*, void * ) override {}
 NVSDK_NGX_Result Get(const char* key, void ** value) const override {
 return NVSDK_NGX_Result_FAIL_InvalidParameter;}
 void Reset() override {present=false;}
};
std::uint64_t nested_dispatch_entry{}, native_dispatch_entry{}, nested_dispatch_exit{};
unsigned nested_native_slot{};
std::uint64_t native_dispatch_production{};
bool native_dispatch_fail{};
bool nested_reverse_dimensions{};
bool nested_forward_alias{};
const NVSDK_NGX_Handle* nested_alias{};
ID3D12GraphicsCommandList* nested_list_override{};
Feature native_dispatch_metadata{};
unsigned actual_family_stage_mask{};
void (*native_parameter_observer)(const NVSDK_NGX_Parameter*){};
const NVSDK_NGX_Handle* direct_creation_handle{};
NVSDK_NGX_Result direct_creation_original(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter* parameters,NVSDK_NGX_Handle** output) {
    if(!output)return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *output=const_cast<NVSDK_NGX_Handle*>(direct_creation_handle);
    if(parameters)parameters->Reset();
    return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result forwarding_generic_original(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params,void* callback) {
    return nested_native_slot==0?evaluate<0>(list,handle,params,callback):evaluate<1>(list,handle,params,callback);
}
NVSDK_NGX_Result nested_native_original(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,void*) {
    actual_family_stage_mask|=1u;
    native_dispatch_entry=native_ingress::upscaler_observation().evaluations;
    {std::scoped_lock lock(state().mutex);
        native_dispatch_production=state().native_production_sequences.at(handle);
        native_dispatch_metadata=state().features.at(handle);}
    if(native_parameter_observer)native_parameter_observer(parameters);
    return native_dispatch_fail?NVSDK_NGX_Result_Fail:NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result nested_generic_original(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* params,void* callback) {
    nested_dispatch_entry=native_ingress::upscaler_observation().evaluations;
    if(nested_reverse_dimensions) {
        auto& map=*static_cast<DispatchParameters*>(const_cast<NVSDK_NGX_Parameter*>(params));
        map.present=true;
        std::swap(map.width,map.out_width);std::swap(map.height,map.out_height);
    }
    const auto* native_handle=nested_alias?nested_alias:handle;
    auto* native_list=nested_list_override?nested_list_override:list;
    const auto result=nested_forward_alias?evaluate<2>(native_list,native_handle,params,callback):
        nested_native_slot==0?evaluate<0>(native_list,native_handle,params,callback):
            evaluate<1>(native_list,native_handle,params,callback);
    // This is where an injector can copy/submit the native output, before the
    // surrounding generic API returns. Observations must already be native.
    nested_dispatch_exit=native_ingress::upscaler_observation().evaluations;
    return result;
}
int run_luke_ngx_dispatch_test(const wchar_t* path, bool engine = false) {
 const auto require=[](bool b,const char* message){if(!b)throw std::runtime_error(message);};
 try {
  test_kcd_production_identity=true;
  HMODULE fixture=LoadLibraryW(path);require(fixture!=nullptr,"fixture load");
  native_ingress::refresh_luke_detection(); // mirror the production XR/session entry
  if(engine)require(!luke_dispatch_module_valid(fixture),"engine misidentified as REAL VR");
  else {
   require(luke_dispatch_module_valid(fixture),"REAL VR own NGX dispatch not recognized");
   require(luke_dispatch_module()==fixture,"custom filename REAL VR marker lookup");
  }
  require(!luke_dispatch_module_valid(GetModuleHandleW(L"kernel32.dll")),"unmarked unrelated module admitted");
  // Cache the actual callable entry points before hook installation.
  const auto make=reinterpret_cast<Create>(GetProcAddress(fixture,"NVSDK_NGX_D3D12_CreateFeature"));
  const auto eval=reinterpret_cast<Evaluate>(GetProcAddress(fixture,"NVSDK_NGX_D3D12_EvaluateFeature"));
  const auto retire=reinterpret_cast<Release>(GetProcAddress(fixture,"NVSDK_NGX_D3D12_ReleaseFeature"));
  DispatchParameters baseline;NVSDK_NGX_Handle* baseline_handle{};
  const auto baseline_count=native_ingress::upscaler_observation().evaluations;
  require(NVSDK_NGX_SUCCEED(make(nullptr,NVSDK_NGX_Feature_SuperSampling,&baseline,&baseline_handle)) &&
      NVSDK_NGX_SUCCEED(eval(nullptr,baseline_handle,&baseline,nullptr)),"baseline original cached dispatch");
  require(native_ingress::upscaler_observation().evaluations==baseline_count,
      "fixture did not reproduce unobserved REAL VR original dispatch");
  require(NVSDK_NGX_SUCCEED(retire(baseline_handle)),"baseline retirement");
  if(engine)install_dispatch<4>(fixture);else install_dispatch<3>(fixture);
  const auto slot=engine?4u:3u;
  require(state().modules[slot].retained!=nullptr&&!state().modules[slot].typed_implementation,"generic dispatch hook/type");
  DispatchParameters p;NVSDK_NGX_Handle* h{};
  require(NVSDK_NGX_SUCCEED(make(nullptr,NVSDK_NGX_Feature_SuperSampling,&p,&h)),"original create result");
  require(!p.present,"original parameter mutation suppressed");
  {std::scoped_lock lock(state().mutex);const auto& f=state().features.at(h);
   require(f.flags_known&&f.flags==p.flags&&f.width==1280&&f.out_height==1440&&f.creation_observed&&
       f.creation_source_identity==f.identity,"real creation contract lost after injector mutation");}
  const auto before=native_ingress::upscaler_observation().evaluations;
  require(NVSDK_NGX_SUCCEED(eval(nullptr,h,&p,nullptr)),"cached original evaluate result");
  require(native_ingress::upscaler_observation().evaluations==before+1,"cached REAL VR evaluation bypassed capture");
  require(state().native_production_sequences.at(h)==1,"generic-only SR did not allocate exact production");
  // No command list or guide resources exist here: these failed capture
  // attempts are still real SR productions and must not be compacted away.
  require(NVSDK_NGX_SUCCEED(eval(nullptr,h,&p,nullptr)) && state().native_production_sequences.at(h)==2,
      "failed guide capture reused production identity");
  const auto old_identity=state().features.at(h).identity;
  p.present=true;
  require(NVSDK_NGX_SUCCEED(make(nullptr,NVSDK_NGX_Feature_SuperSampling,&p,&h)) &&
      state().features.at(h).identity!=old_identity && !state().native_production_sequences.contains(h),
      "reused handle inherited previous native feature generation");
  require(NVSDK_NGX_SUCCEED(eval(nullptr,h,&p,nullptr)) && state().native_production_sequences.at(h)==1,
      "replacement feature did not begin its own production sequence");
  require(NVSDK_NGX_SUCCEED(retire(h))&&!state().features.contains(h),"native handle retirement");
  require(!state().native_production_sequences.contains(h),"native production stream survived feature release");
  p.present=true;require(NVSDK_NGX_SUCCEED(make(nullptr,NVSDK_NGX_Feature_FrameGeneration,&p,&h)),"FG creation");
  require(NVSDK_NGX_SUCCEED(eval(nullptr,h,&p,nullptr)),"FG passthrough");
  require(native_ingress::upscaler_observation().evaluations==before+3 && !state().native_production_sequences.contains(h),"generic FG guessed as SR");
  require(NVSDK_NGX_SUCCEED(retire(h)),"FG retirement");
  // A missed creation at a generic injector must not guess feature type.
  require(NVSDK_NGX_SUCCEED(eval(nullptr,h,&p,nullptr)),"unknown generic passthrough");
  require(native_ingress::upscaler_observation().evaluations==before+3 && !state().native_production_sequences.contains(h),"unknown generic guessed as SR");
  // C-callback evaluation has a separate public entry and may bypass the legacy body.
  const auto eval_c=reinterpret_cast<Evaluate>(GetProcAddress(fixture,"NVSDK_NGX_D3D12_EvaluateFeature_C"));
  require(eval_c!=nullptr,"C callback export");
  p.present=true;require(NVSDK_NGX_SUCCEED(make(nullptr,NVSDK_NGX_Feature_SuperSampling,&p,&h)),"C SR creation");
  const auto before_c=native_ingress::upscaler_observation().evaluations;
  const auto last_c=reinterpret_cast<void*(*)()>(GetProcAddress(fixture,"FixtureLastCCallback"));
  auto* callback=reinterpret_cast<void*>(std::uintptr_t{0x77});
  require(last_c&&NVSDK_NGX_SUCCEED(eval_c(nullptr,h,&p,callback)),"C callback original result");
  require(last_c()==callback,"C callback argument changed");
  require(native_ingress::upscaler_observation().evaluations==before_c+1,"public C evaluation bypassed capture");
  require(state().native_production_sequences.at(h)==1,"public C SR production missing");
  require(NVSDK_NGX_SUCCEED(retire(h)),"C retirement");
  p.present=true;require(NVSDK_NGX_SUCCEED(make(nullptr,NVSDK_NGX_Feature_FrameGeneration,&p,&h)),"C FG create");
  require(NVSDK_NGX_SUCCEED(eval_c(nullptr,h,&p,callback)),"C FG passthrough");
  require(native_ingress::upscaler_observation().evaluations==before_c+1,"C FG guessed as SR");
  require(NVSDK_NGX_SUCCEED(retire(h)),"C FG retirement");
  require(NVSDK_NGX_SUCCEED(eval_c(nullptr,h,&p,callback)),"C unknown passthrough");
  require(native_ingress::upscaler_observation().evaluations==before_c+1,"C unknown guessed as SR");
  for(unsigned native_slot=0;native_slot<2;++native_slot) {
   const auto type=native_slot==0?NVSDK_NGX_Feature_SuperSampling:NVSDK_NGX_Feature_RayReconstruction;
   p.present=true;require(NVSDK_NGX_SUCCEED(make(nullptr,type,&p,&h)),"nested SR/RR create");
   const auto saved_generic=state().modules[slot].evaluate;
   const auto saved_native=state().modules[native_slot].evaluate;
   const auto saved_typed=state().modules[native_slot].typed_implementation;
   nested_native_slot=native_slot;
   state().modules[slot].evaluate=&nested_generic_original;
   state().modules[native_slot].evaluate=&nested_native_original;
   state().modules[native_slot].typed_implementation=true;
   const auto before_nested=native_ingress::upscaler_observation().evaluations;
   require(NVSDK_NGX_SUCCEED(eval(nullptr,h,&p,callback)),"nested cached original result");
   require(nested_dispatch_entry==before_nested,"guide capture still occurs at outer injector boundary");
   require(native_dispatch_entry==before_nested+1 && nested_dispatch_exit==before_nested+1,
       "native SR/RR observation unavailable before injector copies output or duplicated");
   require(native_dispatch_production==1 && state().native_production_sequences.at(h)==1,
       "nested generic/native dispatch allocated more than one SR production");
   native_dispatch_fail=true;
   require(NVSDK_NGX_FAILED(eval(nullptr,h,&p,callback)) && native_dispatch_production==2 &&
       state().native_production_sequences.at(h)==2,"failed native SR result or production identity lost");
   native_dispatch_fail=false;
   require(NVSDK_NGX_SUCCEED(eval(nullptr,h,&p,callback)) && native_dispatch_production==3 &&
       state().native_production_sequences.at(h)==3,"native failure compacted out of production order");
   state().modules[slot].evaluate=saved_generic;
   state().modules[native_slot].evaluate=saved_native;
   state().modules[native_slot].typed_implementation=saved_typed;
   require(NVSDK_NGX_SUCCEED(retire(h)),"nested SR/RR retirement");
  }

  // Cached public dispatch replaces the opaque handle and reverses the
  // creation-only dimension keys before the actual native SR/RR callback.
  // Use real command-list identities: metadata cannot join another list.
  ComPtr<IDXGIFactory6> factory;ComPtr<IDXGIAdapter> warp;
  ComPtr<ID3D12Device> device;
  require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&
      SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))&&
      SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),
      "metadata invocation WARP device");
  std::array<ComPtr<ID3D12CommandAllocator>,2> allocators;
  std::array<ComPtr<ID3D12GraphicsCommandList>,2> lists;
  for(unsigned index=0;index<lists.size();++index)
   require(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators[index])))&&
       SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[index].Get(),nullptr,
           IID_PPV_ARGS(&lists[index]))),"metadata invocation command lists");
  for(unsigned native_slot=0;native_slot<2;++native_slot) {
   const auto type=native_slot==0?NVSDK_NGX_Feature_SuperSampling:NVSDK_NGX_Feature_RayReconstruction;
   alignas(16) std::array<unsigned char,16> alias_storage{},wrong_list_storage{},direct_storage{};
   const auto* alias=reinterpret_cast<const NVSDK_NGX_Handle*>(alias_storage.data());
   const auto* wrong_list_alias=reinterpret_cast<const NVSDK_NGX_Handle*>(wrong_list_storage.data());
   const auto* direct_handle=reinterpret_cast<const NVSDK_NGX_Handle*>(direct_storage.data());
   p.present=true;p.width=1672;p.height=1368;p.out_width=3344;p.out_height=2736;
   require(NVSDK_NGX_SUCCEED(make(lists[0].Get(),type,&p,&h)),"alias public creation");
   const auto created=state().features.at(h);
   const auto saved_generic=state().modules[slot].evaluate;
   const auto saved_native=state().modules[native_slot].evaluate;
   const auto saved_typed=state().modules[native_slot].typed_implementation;
   const auto saved_forwarder=state().modules[2].evaluate;
   const auto saved_forwarder_typed=state().modules[2].typed_implementation;
   state().modules[slot].evaluate=&nested_generic_original;
   state().modules[native_slot].evaluate=&nested_native_original;
   state().modules[native_slot].typed_implementation=true;
   state().modules[2].evaluate=&forwarding_generic_original;
   state().modules[2].typed_implementation=false;
   nested_native_slot=native_slot;nested_alias=alias;nested_reverse_dimensions=true;
   // Recover the raw native handle before any enclosing creation receipt.
   // Later adopting a proven receipt cannot reuse this unproved generation.
   p.present=true;p.width=3344;p.height=2736;p.out_width=1672;p.out_height=1368;
   require(NVSDK_NGX_SUCCEED(native_slot==0?evaluate<0>(lists[0].Get(),alias,&p,callback):
       evaluate<1>(lists[0].Get(),alias,&p,callback)),"unobserved native alias recovery");
   require(!native_dispatch_metadata.creation_observed&&native_dispatch_metadata.creation_source_identity==0&&
       native_dispatch_production==1,"late native evaluation fabricated an observed creation");
   const auto unproved_identity=native_dispatch_metadata.identity;
   p.width=1672;p.height=1368;p.out_width=3344;p.out_height=2736;
   const auto before_alias=native_ingress::upscaler_observation().evaluations;
   require(NVSDK_NGX_SUCCEED(eval(lists[0].Get(),h,&p,callback)),"cached native alias result");
   require(p.width==3344&&p.height==2736&&p.out_width==1672&&p.out_height==1368,
       "original callback did not invert live creation-only keys");
   require(native_dispatch_entry==before_alias+1&&nested_dispatch_exit==before_alias+1,
       "native alias observation lost its boundary or duplicated");
   require(native_dispatch_metadata.creation_observed&&native_dispatch_metadata.width==1672&&
       native_dispatch_metadata.height==1368&&native_dispatch_metadata.out_width==3344&&
       native_dispatch_metadata.out_height==2736&&native_dispatch_metadata.identity!=created.identity&&
       native_dispatch_metadata.creation_source_identity==created.identity&&
       native_dispatch_metadata.identity!=unproved_identity,
       "typed alias trusted reversed evaluation dimensions instead of exact public creation");
   require(native_dispatch_production==1&&state().native_production_sequences.at(alias)==1,
       "public/native alias allocated wrong production identity");
   const auto first_alias_identity=native_dispatch_metadata.identity;

   // A proven new public creation can reuse a raw native alias. Refresh its
   // contract and generation rather than freezing the first preset forever.
   p.present=true;p.width=1559;p.height=1705;p.out_width=3056;p.out_height=3344;
   require(NVSDK_NGX_SUCCEED(make(lists[0].Get(),type,&p,&h)),"replacement public creation");
   const auto replacement=state().features.at(h);
   nested_forward_alias=true; // public A -> known inherited generic B -> typed B
   require(NVSDK_NGX_SUCCEED(eval(lists[0].Get(),h,&p,callback)),"replacement native alias result");
   require(native_dispatch_metadata.creation_observed&&native_dispatch_metadata.width==1559&&
       native_dispatch_metadata.out_width==3056&&native_dispatch_metadata.out_height==3344&&
       native_dispatch_metadata.creation_source_identity==replacement.identity&&
       native_dispatch_metadata.identity!=first_alias_identity&&native_dispatch_production==1,
       "reused native alias retained the previous creation generation/preset");
   nested_forward_alias=false;

   // A nested native call on another real list cannot inherit that receipt.
   nested_alias=wrong_list_alias;nested_list_override=lists[1].Get();
   p.present=true;p.width=1672;p.height=1368;p.out_width=3344;p.out_height=2736;
   require(NVSDK_NGX_SUCCEED(eval(lists[0].Get(),h,&p,callback)),"different native list result");
   require(!native_dispatch_metadata.creation_observed&&!native_dispatch_metadata.width&&
       !native_dispatch_metadata.out_width&&native_dispatch_metadata.flags_known&&
       ngx_display_dimension(native_dispatch_metadata.out_width,&p,NVSDK_NGX_Parameter_OutWidth,
           3344,native_dispatch_metadata.creation_observed)==3344,
       "different-list native handle inherited or promoted an unproved display contract");
   // An independently observed native creation is not an alias of outer A.
   const auto saved_direct_create=state().modules[native_slot].create;
   state().modules[native_slot].create=&direct_creation_original;direct_creation_handle=direct_handle;
   p.present=true;p.width=2048;p.height=1080;p.out_width=4096;p.out_height=2160;
   NVSDK_NGX_Handle* created_direct{};
   require(NVSDK_NGX_SUCCEED(native_slot==0?create<0>(lists[0].Get(),type,&p,&created_direct):
       create<1>(lists[0].Get(),type,&p,&created_direct))&&created_direct==direct_handle,
       "independent native production creation");
   state().modules[native_slot].create=saved_direct_create;direct_creation_handle=nullptr;
   const auto direct=state().features.at(direct_handle);
   nested_alias=direct_handle;nested_list_override=nullptr;nested_forward_alias=true;
   p.present=true;p.width=1672;p.height=1368;p.out_width=3344;p.out_height=2736;
   require(NVSDK_NGX_SUCCEED(eval(lists[0].Get(),h,&p,callback))&&
       native_dispatch_metadata.creation_source_identity==direct.identity&&
       native_dispatch_metadata.identity==direct.identity&&native_dispatch_metadata.out_width==4096&&
       native_dispatch_metadata.out_height==2160,"outer receipt overwrote independently observed native creation");
   nested_forward_alias=false;
   nested_alias=nullptr;nested_list_override=nullptr;nested_reverse_dimensions=false;
   state().modules[slot].evaluate=saved_generic;
   state().modules[native_slot].evaluate=saved_native;
   state().modules[native_slot].typed_implementation=saved_typed;
   state().modules[2].evaluate=saved_forwarder;
   state().modules[2].typed_implementation=saved_forwarder_typed;
   require(NVSDK_NGX_SUCCEED(retire(h)),"alias public retirement");
   {std::scoped_lock lock(state().mutex);
       state().features.erase(alias);state().features.erase(wrong_list_alias);state().features.erase(direct_handle);
       state().native_production_sequences.erase(alias);state().native_production_sequences.erase(wrong_list_alias);
       state().native_production_sequences.erase(direct_handle);}
  }
  for(auto& list:lists)require(SUCCEEDED(list->Close()),"close metadata invocation list");
  std::cout<<"Cached SR/RR aliases: original creation through exact native-list invocation, reversed keys/preset reuse isolation PASS\n";
  std::cout<<"Nested generic/native SR and RR: exact one native observation before injector copy PASS\n";
  std::cout<<"KCD production identity: generic/native ownership, failed captures/native calls, feature isolation PASS\n";
  std::cout<<"REAL VR cached NGX dispatch: original create/evaluate/release, pre-mutation SR metadata, FG/unknown isolation PASS\n";
  // Hooked fixture is pinned until process exit, matching shipping hook lifetime.
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

bool test_dead_space_rejected_camera{};
struct ActualNgxCaptureContext {
    Evaluate evaluate{};
    ID3D12GraphicsCommandList* list{};
    ID3D12CommandQueue* queue{};
    const NVSDK_NGX_Handle* handle{};
    DispatchParameters* parameters{};
    std::shared_ptr<const DlssMotionVectorFrame> frame;
    bool expect_capture{true};
    Create create{};
    Release release{};
    bool recycle_on_release{};
    bool logical_family{};
    unsigned direct_native_identity{};
    bool scene_camera{};
    NVSDK_NGX_Feature feature{NVSDK_NGX_Feature_SuperSampling};
    bool reject_family{},expect_original_failure{};
    ID3D12RootSignature* injector_signature{};
    D3D12_GPU_VIRTUAL_ADDRESS injector_camera{};
    ID3D12Resource *public_output{},*public_motion{},*public_depth{};
    ID3D12Resource *injector_output{},*injector_motion{},*injector_depth{},*composited_color{};
    ComPtr<ID3D12Resource> before_public_readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT before_public_footprint{};
    UINT64 before_public_bytes{};
};
ActualNgxCaptureContext* actual_ngx_capture_context{};
const NVSDK_NGX_Handle* actual_family_alias{};
const NVSDK_NGX_Handle* actual_family_direct{};
Evaluate actual_realvr_evaluate{};
bool actual_family_use_alias{};
enum class OutlawsNegative { none,foreign_list,foreign_device,wrong_type,second_child,native_failure,outer_failure,recursive_public,recreated_public,no_child,changed_contract };
OutlawsNegative actual_outlaws_negative{};
enum class KcdNegative { none,second_child,native_failure,outer_failure };
KcdNegative actual_kcd_negative{};
ID3D12GraphicsCommandList* actual_outlaws_other_list{};
const NVSDK_NGX_Handle* actual_outlaws_sr_handle{};
void actual_kcd_native_parameter_observer(const NVSDK_NGX_Parameter* parameters) {
    const auto& c=*actual_ngx_capture_context;
    const auto& p=*c.parameters;
    // Outlaws deliberately poisons native scalars to prove its public snapshot;
    // KCD uses its distinct typed-native contract. Keep both strict oracles.
    const bool kcd=c.scene_camera;
    if(parameters!=c.parameters||p.motion!=((!kcd||c.expect_capture)?c.injector_motion:nullptr)||p.depth!=c.injector_depth||
        p.output!=c.injector_output||p.input_color!=c.composited_color||p.reset!=(kcd?0:1)||
        p.scale_x!=(kcd?1.25F:91.F)||p.scale_y!=(kcd?.75F:92.F)||
        p.jitter_x!=(kcd?-.375F:9.F)||p.jitter_y!=(kcd?.25F:-8.F)||p.frame_time!=(kcd?19.F:333.F))
        throw std::runtime_error("KCD public transaction changed the original injector's modified native parameters");
}
void actual_fixture_copy(ID3D12GraphicsCommandList* list,ID3D12Resource* destination,
    D3D12_RESOURCE_STATES destination_state,ID3D12Resource* source,D3D12_RESOURCE_STATES source_state) {
    const auto transition=[](ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=resource;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=before;barrier.Transition.StateAfter=after;return barrier;
    };
    const std::array before{transition(destination,destination_state,D3D12_RESOURCE_STATE_COPY_DEST),
        transition(source,source_state,D3D12_RESOURCE_STATE_COPY_SOURCE)};
    // A source already in COPY_SOURCE needs no duplicate same-state barrier.
    list->ResourceBarrier(source_state==D3D12_RESOURCE_STATE_COPY_SOURCE?1:2,before.data());
    list->CopyResource(destination,source);
    const std::array after{transition(destination,D3D12_RESOURCE_STATE_COPY_DEST,destination_state),
        transition(source,D3D12_RESOURCE_STATE_COPY_SOURCE,source_state)};
    list->ResourceBarrier(source_state==D3D12_RESOURCE_STATE_COPY_SOURCE?1:2,after.data());
}
NVSDK_NGX_Result actual_crimson_public_forwarder(ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter* parameters,void* callback) {
    const auto& c=*actual_ngx_capture_context;
    // In-place guide overwrite before native SR; pointer retention alone fails.
    actual_fixture_copy(list,c.public_motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        c.injector_motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    actual_fixture_copy(list,c.public_depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        c.injector_depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(test_gta_enhanced_production_identity) {
        auto& modified=*static_cast<DispatchParameters*>(const_cast<NVSDK_NGX_Parameter*>(parameters));
        modified.motion=c.injector_motion;modified.depth=c.injector_depth;
        modified.output=c.injector_output;
    }
    const auto result=evaluate<0>(list,handle,parameters,callback);
    if(test_gta_enhanced_production_identity&&NVSDK_NGX_SUCCEED(result)) {
        // GTA's actual public fixture delegates directly to the typed body.
        // Inspect its native target here, before the wrapper's final composite.
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={c.injector_output,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=c.injector_output;
        from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=c.before_public_readback.Get();
        to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=c.before_public_footprint;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
    }
    if(NVSDK_NGX_SUCCEED(result))actual_fixture_copy(list,c.public_output,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,c.composited_color,D3D12_RESOURCE_STATE_COPY_SOURCE);
    return result;
}
// REAL VR's NGX hook with FixDLSS on: it hands the typed SR call its own
// MotionVectors in place of the game's (pointer replaced, not overwritten).
NVSDK_NGX_Result actual_unlisted_public_forwarder(ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Handle* handle,const NVSDK_NGX_Parameter* parameters,void* callback) {
    auto& dispatch=*const_cast<DispatchParameters*>(static_cast<const DispatchParameters*>(parameters));
    auto* game_motion=dispatch.motion;
    if(test_unlisted_motion_swap&&game_motion)dispatch.motion=actual_ngx_capture_context->injector_motion;
    const auto result=evaluate<0>(list,handle,parameters,callback);
    dispatch.motion=game_motion;
    return result;
}
NVSDK_NGX_Result actual_outlaws_native_original(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,void*) {
    actual_family_stage_mask|=1u;
    if(test_outlaws_production_identity&&actual_ngx_capture_context&&
        get<ID3D12Resource*>(parameters,NVSDK_NGX_Parameter_Output)!=actual_ngx_capture_context->injector_output)
        throw std::runtime_error("Outlaws fixture lost the original wrapper's private native Output");
    if(test_outlaws_production_identity&&actual_ngx_capture_context&&list==actual_ngx_capture_context->list) {
        const auto& c=*actual_ngx_capture_context;
        actual_kcd_native_parameter_observer(parameters);
        actual_fixture_copy(list,c.injector_output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            c.composited_color,D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    std::scoped_lock lock(state().mutex);
    const auto sequence=state().native_production_sequences.find(handle);
    native_dispatch_production=sequence==state().native_production_sequences.end()?0:sequence->second;
    native_dispatch_metadata=state().features.at(handle);
    return native_dispatch_fail||actual_outlaws_negative==OutlawsNegative::native_failure?
        NVSDK_NGX_Result_Fail:NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result actual_outlaws_public_forwarder(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* public_handle,
    const NVSDK_NGX_Parameter* parameters,void* callback) {
    actual_family_stage_mask|=1u<<3;
    if(actual_outlaws_negative==OutlawsNegative::recursive_public) {
        actual_outlaws_negative=OutlawsNegative::none;
        return actual_realvr_evaluate(list,public_handle,parameters,callback);
    }
    const auto& c=*actual_ngx_capture_context;
    auto& modified=*static_cast<DispatchParameters*>(const_cast<NVSDK_NGX_Parameter*>(parameters));
    // In-place overwrite and replacement are distinct operations. The original
    // game GPU bits must have been protected before either reaches the native
    // SR/RR entry, while that entry still receives these unchanged Luke inputs.
    actual_fixture_copy(list,c.public_motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        c.injector_motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    actual_fixture_copy(list,c.public_depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        c.injector_depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    modified.motion=c.injector_motion;modified.depth=c.injector_depth;modified.output=c.injector_output;
    modified.input_color=c.composited_color;modified.reset=1;modified.scale_x=91;modified.scale_y=92;
    modified.jitter_x=9;modified.jitter_y=-8;modified.frame_time=333;
    if(actual_outlaws_negative==OutlawsNegative::no_child)return NVSDK_NGX_Result_Success;
    if(actual_outlaws_negative==OutlawsNegative::wrong_type)
        return evaluate<0>(list,actual_outlaws_sr_handle,parameters,callback);
    auto* child_list=actual_outlaws_negative==OutlawsNegative::foreign_list||
        actual_outlaws_negative==OutlawsNegative::foreign_device?actual_outlaws_other_list:list;
    const auto* native_handle=actual_family_use_alias?actual_family_alias:actual_family_direct;
    if(actual_outlaws_negative==OutlawsNegative::changed_contract) {
        modified.flags^=NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
        direct_creation_handle=native_handle;NVSDK_NGX_Handle* recreated{};
        if(NVSDK_NGX_FAILED(create<1>(list,NVSDK_NGX_Feature_RayReconstruction,&modified,&recreated))||
            recreated!=native_handle)return NVSDK_NGX_Result_Fail;
        modified.present=true;
    }
    auto result=evaluate<1>(child_list,native_handle,parameters,callback);
    if(actual_outlaws_negative==OutlawsNegative::second_child)
        result=evaluate<1>(list,native_handle,parameters,callback);
    if(actual_outlaws_negative==OutlawsNegative::recreated_public) {
        const auto saved=state().modules[3].create;
        state().modules[3].create=&direct_creation_original;
        direct_creation_handle=public_handle;NVSDK_NGX_Handle* replacement{};
        result=create<3>(list,NVSDK_NGX_Feature_RayReconstruction,&modified,&replacement);
        state().modules[3].create=saved;modified.present=true;
        if(NVSDK_NGX_FAILED(result)||replacement!=public_handle)return NVSDK_NGX_Result_Fail;
    }
    if(NVSDK_NGX_SUCCEED(result)) {
        if(actual_outlaws_negative==OutlawsNegative::none) {
            // Snapshot the PRIVATE GPU color between typed completion and
            // public wrapper completion, before the composite overwrites it.
            // Its readback must contain no provisional origin marker.
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={c.injector_output,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
            list->ResourceBarrier(1,&barrier);
            D3D12_TEXTURE_COPY_LOCATION source{},destination{};
            source.pResource=c.injector_output;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.pResource=c.before_public_readback.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint=c.before_public_footprint;
            list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
        }
        // Post-native composition overwrites any premature private marker and
        // copybacks into the caller target. Output deliberately stays private.
        actual_fixture_copy(list,c.injector_output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            c.composited_color,D3D12_RESOURCE_STATE_COPY_SOURCE);
        actual_fixture_copy(list,c.public_output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            c.injector_output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    return actual_outlaws_negative==OutlawsNegative::outer_failure?NVSDK_NGX_Result_Fail:result;
}
NVSDK_NGX_Result actual_family_forwarder(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,void* callback) {
    actual_family_stage_mask|=1u<<3;
    return evaluate<0>(list,handle,parameters,callback);
}
NVSDK_NGX_Result actual_family_core(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter* parameters,void* callback) {
    actual_family_stage_mask|=1u<<2;
    if(test_kcd_scene_camera) {
        const auto& c=*actual_ngx_capture_context;
        // This original injector callback replaces the game's root state
        // before entering its cached native SR implementation.
        list->SetGraphicsRootSignature(c.injector_signature);
        list->SetGraphicsRootConstantBufferView(1,c.injector_camera);
        // The real wrapper changes not just the camera bindings but its native
        // input map. It also overwrites the ORIGINAL game guides on the same
        // GPU list: saving resource pointers until native entry is insufficient.
        actual_fixture_copy(list,c.public_motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            c.injector_motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        actual_fixture_copy(list,c.public_depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            c.injector_depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto& modified=*static_cast<DispatchParameters*>(const_cast<NVSDK_NGX_Parameter*>(parameters));
        modified.motion=c.expect_capture?c.injector_motion:nullptr;modified.depth=c.injector_depth;modified.output=c.injector_output;
        modified.input_color=c.composited_color;
        modified.reset=0;modified.scale_x=1.25F;modified.scale_y=.75F;
        modified.jitter_x=-.375F;modified.jitter_y=.25F;modified.frame_time=19;
    }
    auto result=actual_realvr_evaluate(list,actual_family_use_alias?actual_family_alias:actual_family_direct,parameters,callback);
    if(test_kcd_scene_camera&&actual_kcd_negative==KcdNegative::second_child)
        result=actual_realvr_evaluate(list,actual_family_use_alias?actual_family_alias:actual_family_direct,parameters,callback);
    if(test_kcd_scene_camera&&NVSDK_NGX_SUCCEED(result)) {
        const auto& c=*actual_ngx_capture_context;
        // Finish the original post-SR composite before publishing its caller
        // color. This deliberately wipes a marker written prematurely into
        // the native injector output, then makes the caller's final image.
        actual_fixture_copy(list,c.injector_output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            c.composited_color,D3D12_RESOURCE_STATE_COPY_SOURCE);
        actual_fixture_copy(list,c.public_output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            c.injector_output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    return test_kcd_scene_camera&&actual_kcd_negative==KcdNegative::outer_failure?NVSDK_NGX_Result_Fail:result;
}
NVSDK_NGX_Result actual_family_engine(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,void* callback) {
    actual_family_stage_mask|=1u<<4;
    return evaluate<2>(list,handle,parameters,callback);
}
NVSDK_NGX_Result actual_recycling_release(NVSDK_NGX_Handle* handle) {
    auto& c=*actual_ngx_capture_context;
    // The original public wrapper releases its native implementation, then a
    // driver callback creates/evaluates a new generation at the same address
    // before the outer intercepted release has returned.
    auto result=release<2>(handle);
    if(NVSDK_NGX_FAILED(result))return result;
    c.parameters->present=true;
    NVSDK_NGX_Handle* reused{};
    result=c.create(c.list,c.feature,c.parameters,&reused);
    if(NVSDK_NGX_FAILED(result)||reused!=handle)return NVSDK_NGX_Result_Fail;
    c.parameters->present=true;
    return c.evaluate(c.list,reused,c.parameters,nullptr);
}
void actual_ngx_capture_observer(void* list,std::uint32_t feature,std::uint32_t frame,
    std::uint32_t viewport,const void*) {
    auto& c=*actual_ngx_capture_context;
    const auto camera=native_ingress::streamline_camera::current();
    if(test_dead_space_rejected_camera) {
        if(camera.valid || camera.rejection!=8 || camera.right!=std::array<float,3>{2,0,0})
            throw std::runtime_error("rejected camera basis was normalized or replaced");
        const auto before=dlss_motion_vector_statistics().published;
        if(NVSDK_NGX_FAILED(c.evaluate(c.list,c.handle,c.parameters,nullptr)) || dlss_motion_vector_statistics().published!=before)
            throw std::runtime_error("rejected camera changed original SR or published a guide");
        DlssMotionVectorPublication guide{};guide.stream=17;
        const auto diagnostic=dead_space_rejected_camera_contract(camera,guide);
        if(std::string_view(diagnostic.stage)!="dead_space_rejected_camera" || diagnostic.publication!=0 ||
            diagnostic.mode!=-8 || diagnostic.camera_right!=camera.right || diagnostic.view_to_clip!=camera.projection)
            throw std::runtime_error("F8 rejected camera diagnostic lost exact values or claimed AFW admission");
        c.frame.reset();return;
    }
    if(list!=c.list||feature!=sl1::eFeatureDLSS||!camera.valid||camera.frame!=frame||camera.viewport!=viewport)
        throw std::runtime_error("actual NGX capture lost its original Streamline production camera");
    const auto before=dlss_motion_vector_statistics().published;
    const auto previous_public_lifetime=state().outlaws_public_lifetimes.find(c.handle);
    const auto previous_public_stream=previous_public_lifetime==state().outlaws_public_lifetimes.end()?0:
        kKcdPublicStreamNamespace|previous_public_lifetime->second->identity;
    actual_family_stage_mask=0;
    const auto result=c.recycle_on_release?c.release(const_cast<NVSDK_NGX_Handle*>(c.handle)):
        c.evaluate(c.list,c.handle,c.parameters,nullptr);
    if(c.reject_family) {
        if(NVSDK_NGX_FAILED(result)!=c.expect_original_failure)
            throw std::runtime_error("Outlaws public rejection changed the original callback result");
        std::uint64_t stream{};
        if(c.scene_camera)stream=kKcdPublicStreamNamespace|state().features.at(c.handle).identity;
        else {
            const auto lifetime=state().outlaws_public_lifetimes.find(c.handle);
            if(lifetime==state().outlaws_public_lifetimes.end()&&!previous_public_stream)
                throw std::runtime_error("Outlaws rejected scope lost its exact public lifetime");
            stream=lifetime==state().outlaws_public_lifetimes.end()?previous_public_stream:
                kKcdPublicStreamNamespace|lifetime->second->identity;
        }
        if(dlss_motion_vector_statistics().published>before+1)
            throw std::runtime_error("rejected public scope borrowed or compacted native children");
        const auto published=dlss_motion_vector_statistics().published;
        if(published>before&&resolve_dlss_guide_publication(stream,published,c.queue))
            throw std::runtime_error("Outlaws mismatched/multiple/failed public child retained admitted guide history");
        c.frame.reset();return;
    }
    if(NVSDK_NGX_FAILED(result))
        throw std::runtime_error("actual NGX capture changed original evaluation result");
    const auto expected_stages=c.feature==NVSDK_NGX_Feature_RayReconstruction?((1u<<3)|1u):
        ((1u<<4)|(1u<<2)|(1u<<3)|1u);
    if(c.logical_family&&(actual_family_stage_mask!=expected_stages||
        (!actual_family_use_alias&&(!native_dispatch_metadata.creation_observed||
            native_dispatch_metadata.identity!=c.direct_native_identity||
            native_dispatch_metadata.creation_source_identity!=c.direct_native_identity||
            native_dispatch_metadata.width!=7||native_dispatch_metadata.out_width!=512))))
        throw std::runtime_error("full engine/core/REALVR/native chain lost direct native creation geometry");
    if(!c.expect_capture) {
        if(dlss_motion_vector_statistics().published!=before)
            throw std::runtime_error("missing native guides unexpectedly published a production");
        c.frame.reset();return;
    }
    const auto guides=resolve_dlss_motion_vectors(1,c.queue);
    if(dlss_motion_vector_statistics().published!=before+1||!guides||!guides->eyes[0])
        throw std::runtime_error("actual NGX capture did not publish exact current guides");
    c.frame=guides->eyes[0];
    if(c.frame->camera_frame_token!=frame||!c.frame->camera_data_valid||!c.frame->camera_matrices_valid)
        throw std::runtime_error("actual NGX publication borrowed another production camera");
}
using SelectLukeExecutionQueue=HRESULT(*)(bool,ID3D12Device*,ID3D12CommandQueue*,ComPtr<ID3D12CommandQueue>&);
bool test_dead_space_diagnostics{};
int run_actual_ngx_capture_pipeline(const wchar_t* realvr,const wchar_t* streamline,bool logical_family,
    bool scene_camera=false,SelectLukeExecutionQueue selector=nullptr,bool outlaws=false,bool dead_space=false,bool gta=false) {
    const auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    const auto check=[&](HRESULT result,const char* message){require(SUCCEEDED(result),message);};
    const bool original_public_guides=test_crimson_native_motion||gta;
    const bool final_public_color=original_public_guides||gta;
    try {
        const auto vr_module=LoadLibraryW(realvr),sl_module=LoadLibraryW(streamline);
        native_ingress::refresh_luke_detection(); // mirror the production XR/session entry
        require(vr_module&&sl_module&&native_ingress::luke_detected(),"actual NGX capture fixtures");
        const auto observer=reinterpret_cast<void(*)(decltype(&actual_ngx_capture_observer))>(
            GetProcAddress(sl_module,"TestObserver"));
        const auto set_constants=reinterpret_cast<decltype(&sl1::slSetConstants)>(GetProcAddress(sl_module,"slSetConstants"));
        const auto evaluate_sl=reinterpret_cast<decltype(&sl1::slEvaluateFeature)>(GetProcAddress(sl_module,"slEvaluateFeature"));
        require(observer&&set_constants&&evaluate_sl,"actual NGX capture original Streamline exports");
        observer(&actual_ngx_capture_observer);native_ingress::poll_streamline();
        require(native_ingress::streamline_camera::hooked(),"actual NGX capture camera ABI hook");
        // Cache the exact exported entries and create the opaque Outlaws public
        // lifetime before interception. Its original creation contract is no
        // longer available when OFXR first sees a public evaluation.
        const auto create_feature=reinterpret_cast<Create>(GetProcAddress(vr_module,"NVSDK_NGX_D3D12_CreateFeature"));
        const auto evaluate_feature=reinterpret_cast<Evaluate>(GetProcAddress(vr_module,"NVSDK_NGX_D3D12_EvaluateFeature"));
        const auto release_feature=reinterpret_cast<Release>(GetProcAddress(vr_module,"NVSDK_NGX_D3D12_ReleaseFeature"));
        require(create_feature&&evaluate_feature&&release_feature,"actual NGX capture original cached NGX exports");
        NVSDK_NGX_Handle* late_public_handle{};
        if(outlaws) {
            DispatchParameters initial;
            require(NVSDK_NGX_SUCCEED(create_feature(nullptr,NVSDK_NGX_Feature_RayReconstruction,&initial,&late_public_handle))&&
                late_public_handle&&!state().features.contains(late_public_handle),
                "Outlaws fixture did not create its public RR lifetime before interception");
        }
        test_kcd_production_identity=logical_family&&!outlaws;
        test_outlaws_production_identity=outlaws;
        test_dead_space_production_identity=dead_space;
        test_gta_enhanced_production_identity=gta;
        test_kcd_scene_camera=scene_camera;
        if(outlaws||dead_space||gta) {
            // Exercise the shipping title selector and marked-module lookup,
            // including cached public exports. A forced test-only slot install
            // would hide the original Outlaws dispatcher coverage failure.
            install_feature_hooks();
            require(state().modules[3].retained==vr_module&&!state().modules[3].typed_implementation,
                "selected title's marked cached NGX dispatcher was not installed by the actual title policy");
        } else install_dispatch<3>(vr_module);
        const auto feature=outlaws?NVSDK_NGX_Feature_RayReconstruction:NVSDK_NGX_Feature_SuperSampling;
        const bool scene_bindings=scene_camera||outlaws;
        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"actual capture factory");
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"actual capture WARP");
        check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"actual capture device");
        if(scene_bindings)require(luke_scene_camera::install(device.Get()),"scene root hooks");
        D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
        check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"actual capture queue");
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"actual capture fence");
        HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(done!=nullptr,"actual capture event");
        UINT64 fence_value{};
        const auto wait=[&] {
            check(queue->Signal(fence.Get(),++fence_value),"actual capture submit fence");
            check(fence->SetEventOnCompletion(fence_value,done),"actual capture notify");
            require(WaitForSingleObject(done,10000)==WAIT_OBJECT_0,"actual capture GPU timeout");
        };
        const auto make_list=[&](ComPtr<ID3D12CommandAllocator>& allocator,ComPtr<ID3D12GraphicsCommandList>& list) {
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"actual capture allocator");
            check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"actual capture list");
        };
        // Only the real Execute of an observed producer list establishes the
        // Luke context. Queue creation alone and layer-private queues cannot.
        native_ingress::configure_queue(queue.Get());
        ComPtr<ID3D12CommandAllocator> bootstrap_allocator;ComPtr<ID3D12GraphicsCommandList> bootstrap_list;
        make_list(bootstrap_allocator,bootstrap_list);native_ingress::observe_luke_production(bootstrap_list.Get());
        if(dead_space&&test_dead_space_diagnostics) {
            DispatchParameters parameters;
            {std::scoped_lock lock(state().mutex);state().queue=queue;state().active=false;}
            dead_space_capture_report_at=0;
            require(!capture(bootstrap_list.Get(),nullptr,&parameters,0,0),"inactive diagnostics created guides");
            const auto stamp=dead_space_capture_report_at.load();
            require(stamp!=0,"F9 diagnostic attempt not sampled");
            require(!capture(bootstrap_list.Get(),nullptr,&parameters,0,0)&&dead_space_capture_report_at==stamp,
                "F9 diagnostics bypassed the one-second limit");
            state().active=true;dead_space_capture_report_at=0;
            require(!capture(bootstrap_list.Get(),nullptr,&parameters,0,0),"unsubmitted producer admitted by diagnostics");
            const auto context=native_ingress::luke_context_diagnostics();
            require(!context.admitted&&context.execute_hook&&context.pending_lists==1&&context.submissions==0,
                "diagnostic snapshot fabricated a producer or consumed its pending list");
        }
        check(bootstrap_list->Close(),"actual capture bootstrap close");
        ID3D12CommandList* bootstrap[]{bootstrap_list.Get()};queue->ExecuteCommandLists(1,bootstrap);wait();
        ComPtr<ID3D12Device> submitted_owner;ComPtr<ID3D12CommandQueue> submitted_queue;
        require(native_ingress::query_luke_context(submitted_owner.GetAddressOf(),submitted_queue.GetAddressOf())&&
            d3d_owner::same(submitted_owner.Get(),device.Get())&&same(submitted_queue.Get(),queue.Get()),
            "actual capture did not retain its original executed native producer");
        ComPtr<ID3D12CommandQueue> execution,foreign_queue;ComPtr<ID3D12Device> foreign_device;
        ComPtr<ID3D12CommandAllocator> private_bootstrap_allocator;
        ComPtr<ID3D12GraphicsCommandList> private_bootstrap_list;ComPtr<ID3D12Fence> private_bootstrap_complete;
        if(selector) {
            check(selector(true,submitted_owner.Get(),submitted_queue.Get(),execution),"actual private execution selector");
            ComPtr<ID3D12Device> execution_owner;check(execution->GetDevice(IID_PPV_ARGS(&execution_owner)),"private execution owner");
            require(execution&&!same(execution.Get(),queue.Get())&&d3d_owner::same(execution_owner.Get(),device.Get()),
                "actual private selector changed owner or reused the native producer queue");
            make_list(private_bootstrap_allocator,private_bootstrap_list);
            check(private_bootstrap_list->Close(),"private unrelated bootstrap close");
            ID3D12CommandList* unrelated[]{private_bootstrap_list.Get()};execution->ExecuteCommandLists(1,unrelated);
            check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&private_bootstrap_complete)),"private bootstrap receipt");
            check(execution->Signal(private_bootstrap_complete.Get(),1),"private bootstrap completion receipt");
            // Keep both original recording objects until their OWN queue has
            // completed. The original producer's separate fence cannot retire
            // this asynchronous, deliberately unobserved private submission.
            require(native_ingress::query_luke_context(submitted_owner.GetAddressOf(),submitted_queue.GetAddressOf())&&
                same(submitted_queue.Get(),queue.Get()),"private submission stole original producer admission");
            check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&foreign_device)),"foreign actual hardware owner");
            require(!d3d_owner::same(foreign_device.Get(),device.Get()),"foreign hardware owner equals WARP");
            check(foreign_device->CreateCommandQueue(&qd,IID_PPV_ARGS(&foreign_queue)),"foreign actual consumer queue");
        }
        if(final_public_color||dead_space)check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&execution)),
            "Crimson independent snapshot read queue");
        auto* consumer=execution?execution.Get():queue.Get();

        const auto texture=[&](unsigned width,unsigned height,DXGI_FORMAT format,
            D3D12_RESOURCE_STATES initial,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width=width;desc.Height=height;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;
            desc.Format=format;desc.Flags=flags;ComPtr<ID3D12Resource> resource;
            check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,initial,nullptr,IID_PPV_ARGS(&resource)),"actual capture texture");
            return resource;
        };
        auto color=texture(512,32,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto motion=texture(7,3,dead_space?DXGI_FORMAT_R16G16_TYPELESS:DXGI_FORMAT_R32G32_FLOAT,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto depth=texture(7,3,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ComPtr<ID3D12Resource> injector_motion,injector_depth,injector_output,composited_color;
        if(scene_camera||outlaws||final_public_color||test_unlisted_typed_dispatch) {
            injector_motion=texture(7,3,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            injector_depth=texture(7,3,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            injector_output=texture(512,32,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            composited_color=texture(512,32,DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        DispatchParameters parameters;parameters.width=7;parameters.height=3;
        parameters.out_width=512;parameters.out_height=32;
        if(outlaws){parameters.render_width=7;parameters.render_height=3;}
        parameters.flags=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        parameters.output=color.Get();parameters.motion=motion.Get();parameters.depth=depth.Get();
        NVSDK_NGX_Handle* handle{};
        alignas(16) std::array<unsigned char,16> alias_storage{},engine_storage{},native_storage{};
        if(outlaws) {
            direct_creation_handle=reinterpret_cast<const NVSDK_NGX_Handle*>(native_storage.data());
            state().modules[1].typed_implementation=true;
            state().modules[1].create=&direct_creation_original;
            state().modules[1].evaluate=&actual_outlaws_native_original;
            require(NVSDK_NGX_SUCCEED(create<1>(bootstrap_list.Get(),feature,&parameters,&handle)),
                "actual native RR creation");
        } else require(NVSDK_NGX_SUCCEED(create_feature(bootstrap_list.Get(),feature,&parameters,&handle)),
            "actual native SR creation");
        parameters.present=true;
        const auto direct_identity=state().features.at(handle).identity;
        if(final_public_color) {
            state().modules[0].typed_implementation=true;
            state().modules[0].evaluate=state().modules[3].evaluate;
            state().modules[3].evaluate=&actual_crimson_public_forwarder;
        }
        if(test_unlisted_typed_dispatch) {
            // Public generic call outside, REAL VR in between, typed SR inside.
            state().modules[0].typed_implementation=true;
            state().modules[0].evaluate=state().modules[3].evaluate;
            state().modules[3].evaluate=&actual_unlisted_public_forwarder;
        }
        if(logical_family) {
            actual_family_alias=reinterpret_cast<const NVSDK_NGX_Handle*>(alias_storage.data());
            actual_family_direct=handle;actual_realvr_evaluate=evaluate_feature;
            if(outlaws) {
                state().modules[3].evaluate=&actual_outlaws_public_forwarder;
                handle=late_public_handle;
                require(!state().features.contains(handle),
                    "late Outlaws public evaluation invented a missing creation contract");
            } else {
                state().modules[0].typed_implementation=true;state().modules[0].evaluate=&nested_native_original;
                state().modules[3].evaluate=&actual_family_forwarder;
                state().modules[2].evaluate=&actual_family_core;
                state().modules[4].evaluate=&actual_family_engine;state().modules[4].create=&direct_creation_original;
                direct_creation_handle=reinterpret_cast<const NVSDK_NGX_Handle*>(engine_storage.data());
                require(NVSDK_NGX_SUCCEED(create<4>(bootstrap_list.Get(),feature,&parameters,&handle)),
                    "actual engine public creation");
            }
            parameters.present=true;
        }
        const unsigned release_slot=logical_family&&!outlaws?4:3;
        const auto native_release=state().modules[3].release;
        state().modules[2].release=native_release;
        state().modules[release_slot].release=&actual_recycling_release;
        {std::scoped_lock lock(state().mutex);state().queue=queue;state().active=true;}
        if(dead_space&&test_dead_space_diagnostics) {
            const auto feature_metadata=state().features.at(handle);
            auto unknown_flags=feature_metadata;unknown_flags.flags_known=false;
            dead_space_capture_report_at=0;
            require(!capture(bootstrap_list.Get(),handle,&parameters,0,0,nullptr,0,nullptr,&unknown_flags,nullptr,0),
                "diagnostics bypassed missing creation flags");
            dead_space_capture_report_at=0;
            require(!capture(bootstrap_list.Get(),handle,&parameters,0,0),"diagnostics invented an exact native camera");
            const auto context=native_ingress::luke_context_diagnostics();
            require(context.admitted&&context.pending_lists==0&&context.submissions>0&&context.queue==reinterpret_cast<UINT64>(queue.Get()),
                "diagnostic snapshot lost actual submission admission");
            dead_space_capture_report_at=0;
        }
        configure_dlss_motion_vector_tracking(true);configure_luke_color_origin_extent(512,32);
        require(embedded::afw_snapshot().desired.mode!=0,"actual capture test requires its AFW defaults");
        ActualNgxCaptureContext context{evaluate_feature,nullptr,queue.Get(),handle,&parameters};
        context.create=logical_family&&!outlaws?&create<4>:create_feature;
        context.release=logical_family?&release<4>:release_feature;
        if(outlaws)context.release=release_feature;
        else if(logical_family)context.evaluate=&evaluate<4>;
        context.logical_family=logical_family;context.direct_native_identity=direct_identity;
        context.scene_camera=scene_camera;context.feature=feature;
        context.public_output=color.Get();context.public_motion=motion.Get();context.public_depth=depth.Get();
        context.injector_output=injector_output.Get();context.injector_motion=injector_motion.Get();
        context.injector_depth=injector_depth.Get();context.composited_color=composited_color.Get();
        if(outlaws||gta) {
            const auto desc=injector_output->GetDesc();
            device->GetCopyableFootprints(&desc,0,1,0,&context.before_public_footprint,nullptr,nullptr,&context.before_public_bytes);
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width=context.before_public_bytes;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;
            buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,IID_PPV_ARGS(&context.before_public_readback)),"Outlaws pre-public private-color GPU receipt");
        }
        actual_ngx_capture_context=&context;
        if(scene_camera)native_parameter_observer=&actual_kcd_native_parameter_observer;
        ComPtr<ID3D12RootSignature> scene_signature,injector_signature;
        ComPtr<ID3D12Resource> scene_upload,injector_upload;
        void* scene_bytes{};
        const auto scene_packet=[](float position) {
            std::array<unsigned char,kcd_camera::packet_bytes> bytes{};
            std::array<float,16> projection{},view{},inverse{},vp{};
            projection[0]=1.5F*3.F/7.F;projection[5]=1.5F;
            projection[10]=.001F;projection[11]=.1F;projection[14]=-1;
            for(unsigned i=0;i<4;++i)view[i*4+i]=inverse[i*4+i]=1;
            view[3]=-position;inverse[3]=position;
            for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
                for(unsigned k=0;k<4;++k)vp[r*4+c]+=projection[r*4+k]*view[k*4+c];
            std::memcpy(bytes.data(),projection.data(),64);std::memcpy(bytes.data()+80,vp.data(),64);
            std::memcpy(bytes.data()+880,view.data(),64);std::memcpy(bytes.data()+944,inverse.data(),64);
            return bytes;
        };
        if(scene_bindings) {
            D3D12_ROOT_PARAMETER roots[3]{};
            for(unsigned i=0;i<3;++i){roots[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;roots[i].Descriptor.ShaderRegister=i;}
            D3D12_ROOT_SIGNATURE_DESC description{};description.NumParameters=3;description.pParameters=roots;
            ComPtr<ID3DBlob> bytes,errors;
            check(D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&errors),"KCD scene root serialization");
            check(device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&scene_signature)),"KCD scene root");
            roots[0].Descriptor.RegisterSpace=1;
            check(D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&errors),"KCD injector root serialization");
            check(device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&injector_signature)),"KCD injector root");
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=2048;
            buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,
                IID_PPV_ARGS(&scene_upload)),"KCD scene upload");
            check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,
                IID_PPV_ARGS(&injector_upload)),"KCD injector upload");
            check(scene_upload->Map(0,nullptr,&scene_bytes),"KCD persistent scene bytes");
            void* injector_bytes{};check(injector_upload->Map(0,nullptr,&injector_bytes),"KCD injector bytes");
            const auto unrelated=scene_packet(999.F);std::memcpy(injector_bytes,unrelated.data(),unrelated.size());
            injector_upload->Unmap(0,nullptr);
            context.injector_signature=injector_signature.Get();context.injector_camera=injector_upload->GetGPUVirtualAddress();
            static_cast<void>(scene_upload->GetGPUVirtualAddress());
        }
        const auto transition=[](ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource=resource;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore=before;barrier.Transition.StateAfter=after;return barrier;
        };
        const auto verify_private_snapshots=[&](const std::shared_ptr<const DlssMotionVectorFrame>& current) {
            const auto& previous=current->previous_opposite_guide;
            const std::array resources{current->motion_vectors,previous->motion_vectors,current->depth,previous->depth};
            const std::array protections{current->motion_retention,previous->motion_retention,current->depth_retention,previous->depth_retention};
            std::array<ComPtr<ID3D12Resource>,4> readbacks;
            std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,4> footprints{};
            std::array<UINT64,4> sizes{};
            ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
            make_list(allocator,list);
            for(unsigned i=0;i<resources.size();++i) {
                require(resources[i]&&protections[i],"private ingress lost protected current/previous snapshots");
                const auto desc=resources[i]->GetDesc();device->GetCopyableFootprints(&desc,0,1,0,&footprints[i],nullptr,nullptr,&sizes[i]);
                D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=sizes[i];
                buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr,IID_PPV_ARGS(&readbacks[i])),"private immutable snapshot readback");
                const auto state=i<2?current->resource_state:current->depth_resource_state;
                auto before=transition(resources[i].Get(),state,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&before);
                D3D12_TEXTURE_COPY_LOCATION source{},destination{};source.pResource=resources[i].Get();
                source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;destination.pResource=readbacks[i].Get();
                destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=footprints[i];
                list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
                auto after=transition(resources[i].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,state);list->ResourceBarrier(1,&after);
            }
            check(list->Close(),"private snapshot readback close");
            ComPtr<ID3D12Fence> gate,completion;
            check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"private readback gate");
            check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&completion)),"private readback completion");
            struct ReleaseGate {ID3D12Fence* fence;~ReleaseGate(){static_cast<void>(fence->Signal(1));}} release{gate.Get()};
            check(execution->Wait(gate.Get(),1),"queue exact private GPU read gate");
            for(const auto& retention:protections){retention->begin_gpu_read();require(!retention->reusable(),"pending GPU read failed to pin snapshot");}
            ID3D12CommandList* submitted[]{list.Get()};execution->ExecuteCommandLists(1,submitted);
            check(execution->Signal(completion.Get(),1),"private GPU read receipt");
            for(const auto& retention:protections) {
                check(retention->retire_gpu_read(completion.Get(),1),"retire exact private GPU read");
                require(!retention->reusable(),"unfinished private read released protected snapshot");
            }
            wait(); // The original producer advances while its independent consumer is gated.
            check(gate->Signal(1),"release private GPU read gate");
            ResetEvent(done);check(completion->SetEventOnCompletion(1,done),"private GPU read notification");
            require(WaitForSingleObject(done,10000)==WAIT_OBJECT_0,"private GPU read completion timeout");
            for(unsigned i=0;i<resources.size();++i) {
                require(protections[i]->reusable(),"completed private GPU read retained a stale pin");
                void* bytes{};const D3D12_RANGE range{0,static_cast<SIZE_T>(sizes[i])};
                check(readbacks[i]->Map(0,&range,&bytes),"private snapshot map");
                const unsigned native_frame=(i&1)?previous->camera_frame_token:current->camera_frame_token;
                const unsigned pixel_bytes=i<2&&!dead_space?8:4;
                const float expected[]{scene_camera?(i<2?8.F+native_frame:.875F):
                    (i<2?.125F+.01F*native_frame:.125F*native_frame),scene_camera?9.F+native_frame:-.25F};
                const std::uint16_t half_expected[]{static_cast<std::uint16_t>(0x3c00+native_frame*64),0xb400};
                if(dead_space&&i<2)require(resources[i]->GetDesc().Format==DXGI_FORMAT_R16G16_FLOAT&&
                    motion->GetDesc().Format==DXGI_FORMAT_R16G16_TYPELESS&&resources[i].Get()!=motion.Get(),
                    "Dead Space did not type only its owned motion snapshot");
                for(unsigned y=0;y<3;++y)for(unsigned x=0;x<7;++x)
                    require(!std::memcmp(static_cast<char*>(bytes)+footprints[i].Offset+y*footprints[i].Footprint.RowPitch+x*pixel_bytes,
                        dead_space&&i<2?static_cast<const void*>(half_expected):static_cast<const void*>(expected),
                        pixel_bytes),"private current/previous motion or depth changed GPU bits");
                const D3D12_RANGE no_write{0,0};readbacks[i]->Unmap(0,&no_write);
            }
        };
        const auto verify_final_public_color=[&](unsigned production) {
            const auto desc=color->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
            device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
            buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;
            buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> readback;
            check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,IID_PPV_ARGS(&readback)),"final public output readback");
            ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
            make_list(allocator,list);auto before=transition(color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&before);
            D3D12_TEXTURE_COPY_LOCATION source{},destination{};source.pResource=color.Get();
            source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;destination.pResource=readback.Get();
            destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=footprint;
            list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
            auto after=transition(color.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1,&after);check(list->Close(),"final public output readback close");
            ID3D12CommandList* submitted[]{list.Get()};consumer->ExecuteCommandLists(1,submitted);
            ComPtr<ID3D12Fence> completion;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&completion)),"final public output completion");
            check(consumer->Signal(completion.Get(),1),"final public output submission receipt");
            ResetEvent(done);check(completion->SetEventOnCompletion(1,done),"final public output notification");
            require(WaitForSingleObject(done,10000)==WAIT_OBJECT_0,"final public output GPU timeout");
            void* data{};const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
            check(readback->Map(0,&range,&data),"final public output bytes");
            const std::array<unsigned char,4> expected{static_cast<unsigned char>(40+production),51,63,255};
            // The marker occupies its small negotiated band near the top;
            // the last complete row proves the original post-SR composite
            // reached the EXACT caller output, alongside its decoded CRC.
            for(unsigned x=0;x<desc.Width;++x)require(!std::memcmp(static_cast<char*>(data)+footprint.Offset+
                (desc.Height-1)*footprint.Footprint.RowPitch+x*4,expected.data(),4),
                "final public output lost original post-SR composite GPU bytes");
            const D3D12_RANGE no_write{0,0};readback->Unmap(0,&no_write);
        };
        std::vector<std::shared_ptr<const DlssMotionVectorFrame>> retained;
        const unsigned recycle_production=logical_family?14:4;
        const unsigned productions=recycle_production+1;
        std::shared_ptr<const DlssMotionVectorFrame> calibrated;
        if(outlaws) {
            ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
            make_list(allocator,list);context.list=list.Get();context.expect_capture=false;
            sl1::Constants constants{};
            auto* projection=reinterpret_cast<float*>(&constants.cameraViewToClip);
            auto* inverse=reinterpret_cast<float*>(&constants.clipToCameraView);
            projection[0]=projection[5]=1;projection[11]=-1;projection[14]=.1F;
            inverse[0]=inverse[5]=1;inverse[11]=10;inverse[14]=-1;
            constants.cameraRight={1,0,0};constants.cameraUp={0,1,0};constants.cameraFwd={0,0,-1};
            constants.cameraNear=.1F;constants.cameraFar=100;constants.cameraFOV=1;
            constants.depthInverted=static_cast<decltype(constants.depthInverted)>(1);
            require(set_constants(constants,0,0)&&evaluate_sl(reinterpret_cast<sl1::CommandBuffer*>(list.Get()),
                sl1::eFeatureDLSS,0,0),"Outlaws cold typed-contract original callback");
            const auto lifetime=state().outlaws_public_lifetimes.at(handle);
            require(lifetime->guide_contract_known&&!state().features.contains(handle)&&
                state().public_production_sequences.at(kKcdPublicStreamNamespace|lifetime->identity)==1&&
                !context.frame&&dlss_motion_vector_statistics().published==0,
                "late public warmup invented creation metadata, compacted native clock or borrowed modified guides");
            check(list->Close(),"Outlaws cold production close");ID3D12CommandList* submitted[]{list.Get()};
            queue->ExecuteCommandLists(1,submitted);wait();
        }
        for(unsigned n=1;n<=productions;++n) {
            ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
            make_list(allocator,list);context.list=list.Get();
            context.recycle_on_release=n==recycle_production;
            const auto old_feature=state().features.find(handle);
            const auto old_identity=old_feature==state().features.end()?0:old_feature->second.identity;
            const auto old_stream=retained.empty()?0:retained.back()->stream;
            const auto old_token=retained.empty()?0:retained.back()->color_origin_token;
            std::array<ComPtr<ID3D12Resource>,5> uploads;
            for(unsigned index=0;index<(scene_camera||outlaws||final_public_color?5u:2u);++index) {
                auto* resource=index==0?motion.Get():index==1?depth.Get():index==2?injector_motion.Get():
                    index==3?injector_depth.Get():composited_color.Get();
                const auto desc=resource->GetDesc();
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
                device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
                D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
                buffer.Width=bytes;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;
                buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,IID_PPV_ARGS(&uploads[index])),"actual capture input upload");
                void* data{};check(uploads[index]->Map(0,nullptr,&data),"actual capture upload map");
                std::memset(data,0,static_cast<std::size_t>(bytes));
                const float values[]{index==0?.125F+.01F*n:index==1?.125F*n:index==2?8.F+n:.875F,
                    index<2?-.25F:9.F+n};
                const unsigned pixel_bytes=(index==0&&!dead_space)||index==2?8:4;
                const std::uint16_t half_values[]{static_cast<std::uint16_t>(0x3c00+n*64),0xb400};
                const std::array<unsigned char,4> pixel{static_cast<unsigned char>(40+n),51,63,255};
                for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x)
                    std::memcpy(static_cast<char*>(data)+footprint.Offset+y*footprint.Footprint.RowPitch+x*pixel_bytes,
                        index==4?static_cast<const void*>(pixel.data()):dead_space&&index==0?
                        static_cast<const void*>(half_values):static_cast<const void*>(values),pixel_bytes);
                uploads[index]->Unmap(0,nullptr);
                const auto resource_state=index==4?D3D12_RESOURCE_STATE_COPY_SOURCE:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                auto before=transition(resource,resource_state,D3D12_RESOURCE_STATE_COPY_DEST);
                list->ResourceBarrier(1,&before);
                D3D12_TEXTURE_COPY_LOCATION source{},destination{};source.pResource=uploads[index].Get();
                source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprint;
                destination.pResource=resource;destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
                auto after=transition(resource,D3D12_RESOURCE_STATE_COPY_DEST,resource_state);
                list->ResourceBarrier(1,&after);
            }
            sl1::Constants constants{};
            for(auto* matrix:{&constants.cameraViewToClip,&constants.clipToCameraView,&constants.clipToPrevClip,&constants.prevClipToClip}) {
                std::memset(matrix,0,sizeof(*matrix));
                auto* values=reinterpret_cast<float*>(matrix);values[0]=values[5]=values[10]=values[15]=1;
            }
            constants.cameraPos={logical_family?100.F+.2F*n+((n&1)?-3.2F:3.2F):float(n),0,0};
            constants.cameraRight={1,0,0};constants.cameraUp={0,1,0};constants.cameraFwd={0,0,-1};
            if(logical_family) {
                std::memset(&constants.cameraViewToClip,0,sizeof(constants.cameraViewToClip));
                std::memset(&constants.clipToCameraView,0,sizeof(constants.clipToCameraView));
                auto* projection=reinterpret_cast<float*>(&constants.cameraViewToClip);
                auto* inverse=reinterpret_cast<float*>(&constants.clipToCameraView);
                projection[0]=projection[5]=1;projection[11]=-1;projection[14]=.1F;
                inverse[0]=inverse[5]=1;inverse[11]=10;inverse[14]=-1;
                actual_family_use_alias=(n&1)!=0;
                context.expect_capture=n!=12;
                parameters.motion=n==12?nullptr:motion.Get();
            }
            if(gta) {
                // A new caller production supplies its own public bindings;
                // do not reuse the previous wrapper's in-place mutation.
                parameters.output=color.Get();parameters.motion=motion.Get();parameters.depth=depth.Get();
            }
            if(scene_camera||outlaws) {
                parameters.output=color.Get();parameters.depth=depth.Get();parameters.input_color=color.Get();
                parameters.reset=0;parameters.scale_x=parameters.scale_y=1;
                parameters.jitter_x=.125F;parameters.jitter_y=-.25F;parameters.frame_time=17.F;
            }
            constants.cameraNear=.1F;constants.cameraFar=100;constants.cameraFOV=1;
            const float expected_scene_position=constants.cameraPos.x;
            kcd_camera::Camera expected_scene_camera{};
            if(scene_bindings) {
                const auto packet=scene_packet(outlaws?999.F:expected_scene_position);
                require(kcd_camera::decode(packet.data(),packet.size(),7,3,expected_scene_camera)&&
                    std::abs(expected_scene_camera.position[0]-(outlaws?999.F:expected_scene_position))<.001F,"exact render packet");
                std::memcpy(scene_bytes,packet.data(),packet.size());
                list->SetGraphicsRootSignature(scene_signature.Get());
                list->SetGraphicsRootConstantBufferView(1,scene_upload->GetGPUVirtualAddress());
                // The dispatch fixture still uses SL to invoke its callback,
                // but that other camera must never authorize this KCD route.
                if(scene_camera)constants.cameraPos={-777.F,0,0};
            }
            constants.depthInverted=static_cast<decltype(constants.depthInverted)>(1);
            if(dead_space&&test_dead_space_diagnostics&&n==1) {
                constants.cameraRight={2,0,0};dead_space_capture_report_at=0;test_dead_space_rejected_camera=true;
                require(set_constants(constants,n,0)&&evaluate_sl(reinterpret_cast<sl1::CommandBuffer*>(list.Get()),sl1::eFeatureDLSS,n,0),
                    "rejected exact basis original Streamline result");
                test_dead_space_rejected_camera=false;constants.cameraRight={1,0,0};dead_space_capture_report_at=0;
            }
            require(set_constants(constants,n,0)&&evaluate_sl(reinterpret_cast<sl1::CommandBuffer*>(list.Get()),sl1::eFeatureDLSS,n,0),
                "actual capture original Streamline results");
            if(context.frame && !scene_camera)require(
                context.frame->raw_motion_native_span==((dead_space || test_ghost_native_motion ||
                    test_horizon_native_motion||test_unlisted_motion_swap)?2u:1u),
                "actual title/runtime capture used the wrong native motion interval");
            const unsigned successful=n>=recycle_production?n-recycle_production+1:
                n-(logical_family&&n>=12?1:0);
            if(logical_family&&n!=12&&n>=2&&n<recycle_production)
                require(context.frame&&context.frame->serial==successful&&
                    context.frame->native_production_sequence==n+(outlaws?1:0)&&(context.frame->stream&(std::uint64_t{1}<<63))&&
                    context.frame->stream==retained.front()->stream,
                    "two native handles split one proven public production family");
            if(context.recycle_on_release) {
                const auto replacement=state().features.find(handle);
                require(replacement!=state().features.end()&&replacement->second.identity!=old_identity,
                    "outer release deleted a callback-reused native feature generation");
                if(logical_family) {
                    const auto mapped=state().guide_streams.find(handle);
                    require(mapped!=state().guide_streams.end()&&mapped->second==context.frame->stream&&
                        mapped->second!=old_stream&&(outlaws||std::uint32_t(mapped->second)==replacement->second.identity)&&
                        state().public_production_sequences.at(mapped->second)==1&&
                        context.frame->native_production_sequence==1,
                        "outer release deleted the replacement public-family mapping or clock");
                } else require(context.frame&&context.frame->stream==old_stream,
                    "callback reuse did not exercise the exact same raw-pointer guide stream");
            }
            if(n!=12)require(context.frame&&context.frame->serial==successful&&context.frame->camera_frame_token==n&&
                context.frame->motion_retention&&context.frame->depth_retention&&
                d3d_owner::same(context.frame->motion_snapshot_device.Get(),device.Get())&&
                d3d_owner::same(context.frame->depth_snapshot_device.Get(),device.Get())&&
                same(context.frame->producer_queue.Get(),queue.Get())&&same(context.frame->producer_command_list.Get(),list.Get()),
                "actual capture lost one of its queried owners, exact list/queue or owned snapshots");
            if(scene_camera&&n!=12)require(std::abs(context.frame->camera_position[0]-expected_scene_position)<.001F&&
                std::abs(context.frame->camera_position[0]-999.F)>100.F&&context.frame->scale_x==1.25F&&context.frame->scale_y==.75F&&
                context.frame->jitter_x==-.375F&&context.frame->jitter_y==.25F&&!context.frame->reset&&
                context.frame->frame_time_delta_ms==19.F&&context.frame->output.Get()==color.Get()&&
                context.frame->raw_motion_native_span==2&&
                !std::memcmp(context.frame->camera_position.data(),expected_scene_camera.position.data(),sizeof(float)*3)&&
                !std::memcmp(context.frame->camera_view_to_clip.data(),expected_scene_camera.projection.data(),sizeof(float)*16)&&
                !std::memcmp(context.frame->camera_clip_to_view.data(),expected_scene_camera.inverse_projection.data(),sizeof(float)*16),
                "typed SR lost original camera/final Output or did not snapshot its actual native input scalars");
            if(outlaws&&n!=12)require(context.frame->camera_frame_token==n&&
                std::abs(context.frame->camera_position[0]-expected_scene_position)<.001F&&
                std::abs(context.frame->camera_position[0]-999.F)>100.F&&
                context.frame->scale_x==1&&context.frame->scale_y==1&&
                context.frame->jitter_x==.125F&&context.frame->jitter_y==-.25F&&!context.frame->reset&&
                context.frame->frame_time_delta_ms==17.F&&
                native_dispatch_production==context.frame->native_production_sequence,
                "Outlaws RR replaced the exact original SL production camera with KCD root bindings");
            if(outlaws&&n<recycle_production)require(!state().features.contains(handle)&&
                native_dispatch_metadata.type==NVSDK_NGX_Feature_RayReconstruction&&
                native_dispatch_metadata.flags_known&&
                (actual_family_use_alias?!native_dispatch_metadata.creation_observed&&
                    !native_dispatch_metadata.creation_source_identity&&native_dispatch_metadata.width==0&&
                    native_dispatch_metadata.out_width==0:
                    native_dispatch_metadata.creation_observed&&native_dispatch_metadata.width==7&&
                    native_dispatch_metadata.out_width==512)&&
                (n==12||context.frame->output_width==512&&context.frame->output_height==32&&
                    context.frame->motion_width==7&&context.frame->motion_height==3),
                "late public RR family invented creation geometry or changed the exact native contract");
            if(outlaws&&n!=12)require(context.frame->output.Get()==color.Get()&&
                parameters.output==injector_output.Get(),
                "Outlaws marker used mutated private Output instead of retained original public target");
            check(list->Close(),"actual capture production close");ID3D12CommandList* submitted[]{list.Get()};
            if(selector&&context.frame)require(!resolve_luke_guide_token(context.frame->color_origin_token,consumer,device.Get()),
                "unsubmitted actual native production acquired a private-consumer fence receipt");
            queue->ExecuteCommandLists(1,submitted);
            if(selector&&context.frame) {
                const auto submitted_frame=resolve_dlss_guide_publication(context.frame->stream,context.frame->publication,consumer);
                require(submitted_frame&&submitted_frame->producer_fence&&submitted_frame->producer_fence_value&&
                    !submitted_frame->producer_command_list&&context.frame->producer_command_list&&
                    !context.frame->producer_fence&&d3d_owner::same(submitted_frame->producer_fence_device.Get(),device.Get()),
                    "original Execute did not stamp immutable actual captures for the private consumer");
            }
            wait();
            std::uint32_t token{};const auto marker_result=read_luke_color_origin(consumer,color.Get(),&token,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,5000);
            if(n==12) {
                require(!context.frame&&(scene_camera||outlaws?FAILED(marker_result):
                    SUCCEEDED(marker_result)&&token==retained.back()->color_origin_token),
                    "failed public guide capture authorized the injector's modified native guides/color");
                continue;
            }
            check(marker_result,"actual capture completed final public color marker read");
            require(token==context.frame->color_origin_token,"actual capture color marker did not carry its exact publication");
            if(outlaws||gta) {
                void* data{};const D3D12_RANGE range{0,static_cast<SIZE_T>(context.before_public_bytes)};
                check(context.before_public_readback->Map(0,&range,&data),"Outlaws private color before public completion");
                std::array<float,512> row{};bool native_marker=false;
                for(unsigned y=0;y<32;++y) {
                    for(unsigned x=0;x<row.size();++x)row[x]=luke_origin_luma(
                        static_cast<const std::byte*>(data)+context.before_public_footprint.Offset+
                        y*context.before_public_footprint.Footprint.RowPitch+x*4,DXGI_FORMAT_R8G8B8A8_UNORM);
                    const auto decoded=decode_luke_origin_row(row);
                    if(gta) {
                        require(!decoded.ambiguous&&(!decoded.token||decoded.token==context.frame->color_origin_token),
                            "GTA native output carries a different production from original N1 guides");
                        native_marker|=decoded.token!=0;
                    } else require(!decoded.token&&!decoded.ambiguous,
                        "Outlaws native origin marker became visible before original public wrapper completion");
                }
                if(gta)require(native_marker,"GTA actual native color lost original N1 identity before wrapper copyback");
                const D3D12_RANGE no_write{0,0};context.before_public_readback->Unmap(0,&no_write);
            }
            if(context.recycle_on_release) {
                const auto replacement=resolve_dlss_guide_publication(context.frame->stream,
                    context.frame->publication,queue.Get());
                require(replacement&&replacement->publication==context.frame->publication&&
                    replacement->producer_fence_value&&replacement->serial==1&&
                    !resolve_luke_guide_token(old_token,queue.Get(),device.Get()),
                    "outer release retired a newly submitted reused stream or preserved old colors");
            }
            if(n==productions) {
                const auto replacement=resolve_luke_guide_token(token,consumer,device.Get());
                require(replacement&&replacement->serial==2&&replacement->previous_opposite_guide&&
                    replacement->previous_opposite_guide->serial==1&&
                    replacement->previous_opposite_guide->camera_frame_token==n-1&&
                    (!logical_family||replacement->native_production_sequence==2),
                    "callback-reused production lost its exact new-generation N-1 token history");
            }
            if(n==3||logical_family&&n>=11&&n<recycle_production) {
                const auto joined=resolve_luke_guide_token(token,consumer,device.Get());
                require(joined&&joined->publication==context.frame->publication&&joined->producer_fence_value&&
                    joined->previous_opposite_guide&&joined->previous_opposite_guide->camera_frame_token==(n==13?11:n-1)&&
                    joined->previous_same_native_camera&&joined->previous_same_native_camera->token==(n==13?10:n-2),
                    "actual capture submission did not retain exact fenced current/N-1/N-2 cameras");
                if(selector) {
                    require(joined->previous_opposite_guide->producer_fence&&
                        joined->previous_opposite_guide->producer_fence_value&&
                        !joined->previous_opposite_guide->producer_command_list&&
                        !resolve_luke_guide_token(token,foreign_queue.Get(),foreign_device.Get())&&
                        !resolve_luke_guide_token(token,consumer,foreign_device.Get()),
                        "private ingress weakened previous submission or real foreign owner admission");
                    verify_private_snapshots(joined);
                    if(scene_camera||outlaws)verify_final_public_color(n);
                }
                if(final_public_color||dead_space) {
                    verify_private_snapshots(joined);
                    if(final_public_color)verify_final_public_color(n);
                }
                if(logical_family) {
                    LukeNativeMotionConstants corrected{};unsigned why{};
                    const bool valid=luke_native_motion_constants(*joined,0,0,corrected,why);
                    require(n==13?!valid&&why==4:valid&&why==0,
                        "logical source relaxed the actual native production-gap or camera guards");
                    if(n==11) {
                        require(std::abs(joined->calibrated_game_eye_baseline-6.4F)<.001F,
                            "two native eye handles did not calibrate nine real ABA camera intervals");
                        calibrated=joined;
                    }
                }
            }
            retained.push_back(context.frame);
        }
        require(dlss_motion_vector_statistics().published==(logical_family?14:5),"actual capture count changed during native callbacks");
        if(scene_camera) {
            // Release old test-only references after completed readbacks, so
            // subsequent failure controls exercise transaction retirement and
            // not an artificial exhaustion of the protected snapshot ring.
            retained.erase(retained.begin(),retained.end()-1);
            context.recycle_on_release=false;context.reject_family=true;actual_family_use_alias=false;
            const std::array failures{KcdNegative::second_child,KcdNegative::native_failure,KcdNegative::outer_failure};
            for(unsigned i=0;i<failures.size();++i) {
                ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
                make_list(allocator,list);context.list=list.Get();parameters.present=true;
                parameters.motion=motion.Get();parameters.depth=depth.Get();parameters.output=color.Get();
                parameters.input_color=color.Get();parameters.reset=0;parameters.scale_x=parameters.scale_y=1;
                parameters.jitter_x=.125F;parameters.jitter_y=-.25F;parameters.frame_time=17;
                list->SetGraphicsRootSignature(scene_signature.Get());
                list->SetGraphicsRootConstantBufferView(1,scene_upload->GetGPUVirtualAddress());
                actual_kcd_negative=failures[i];native_dispatch_fail=failures[i]==KcdNegative::native_failure;
                context.expect_original_failure=failures[i]!=KcdNegative::second_child;
                sl1::Constants constants{};
                for(auto* matrix:{&constants.cameraViewToClip,&constants.clipToCameraView,&constants.clipToPrevClip,&constants.prevClipToClip}) {
                    std::memset(matrix,0,sizeof(*matrix));auto* values=reinterpret_cast<float*>(matrix);
                    values[0]=values[5]=values[10]=values[15]=1;
                }
                constants.cameraPos={-777.F,0,0};constants.cameraRight={1,0,0};
                constants.cameraUp={0,1,0};constants.cameraFwd={0,0,-1};
                constants.cameraNear=.1F;constants.cameraFar=100;constants.cameraFOV=1;
                constants.depthInverted=static_cast<decltype(constants.depthInverted)>(1);
                require(set_constants(constants,16+i,0)&&evaluate_sl(reinterpret_cast<sl1::CommandBuffer*>(list.Get()),
                    sl1::eFeatureDLSS,16+i,0),"KCD original SL result changed by a rejected transaction");
                check(list->Close(),"KCD rejected public transaction close");
                ID3D12CommandList* submitted[]{list.Get()};queue->ExecuteCommandLists(1,submitted);wait();
                std::uint32_t token{};const auto result=read_luke_color_origin(consumer,color.Get(),&token,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,5000);
                require(FAILED(result)||!resolve_luke_guide_token(token,consumer,device.Get()),
                    "KCD repeated/failed native child or failed wrapper retained its provisional public token");
            }
            actual_kcd_negative=KcdNegative::none;native_dispatch_fail=false;
            context.reject_family=false;context.expect_original_failure=false;
            std::cout<<"KCD public transaction negatives: actual repeated native child/clock, failed native evaluation and failed enclosing wrapper retire protected provisional guides and caller color admission PASS\n";
        }
        if(scene_camera) {
            // Exercise the production carrier with real native identities and
            // actual upload/root bindings, without publishing extra histories.
            ComPtr<ID3D12CommandAllocator> allocator,foreign_allocator;
            ComPtr<ID3D12GraphicsCommandList> list,foreign;
            make_list(allocator,list);make_list(foreign_allocator,foreign);
            ComPtr<IUnknown> list_identity,foreign_identity,device_identity;
            check(list.As(&list_identity),"KCD scope list identity");
            check(foreign.As(&foreign_identity),"KCD foreign list identity");
            check(device.As(&device_identity),"KCD scope device identity");
            const auto creator=state().features.at(handle);
            list->SetGraphicsRootSignature(scene_signature.Get());
            list->SetGraphicsRootConstantBufferView(1,scene_upload->GetGPUVirtualAddress());
            {
                KcdCameraInvocationScope public_scope(creator,list_identity.Get(),device_identity.Get(),creator.identity,
                    true,list.Get(),&parameters);
                KcdCameraInvocationScope wrong_list(creator,foreign_identity.Get(),device_identity.Get(),creator.identity,
                    false,foreign.Get(),&parameters);
                DlssMotionVectorPublication publication;bool valid{};
                require(wrong_list.apply(publication,foreign_identity.Get(),device_identity.Get(),creator.identity,7,3,valid)&&!valid,
                    "foreign native list inherited public scene receipt");
            }
            list->SetGraphicsRootConstantBufferView(1,scene_upload->GetGPUVirtualAddress());
            {
                KcdCameraInvocationScope public_scope(creator,list_identity.Get(),device_identity.Get(),creator.identity,
                    true,list.Get(),&parameters);
                KcdCameraInvocationScope native_scope(creator,list_identity.Get(),device_identity.Get(),creator.identity,
                    false,list.Get(),&parameters);
                DlssMotionVectorPublication publication;bool valid{};
                require(native_scope.apply(publication,list_identity.Get(),device_identity.Get(),creator.identity,7,3,valid)&&valid,
                    "exact public scene receipt missing");
                require(native_scope.apply(publication,list_identity.Get(),device_identity.Get(),creator.identity,7,3,valid)&&!valid,
                    "recursive/repeated observation reused consumed scene receipt");
            }
            const auto different=scene_packet(1010.F);
            std::memcpy(static_cast<unsigned char*>(scene_bytes)+1024,different.data(),different.size());
            list->SetGraphicsRootConstantBufferView(1,scene_upload->GetGPUVirtualAddress());
            list->SetGraphicsRootConstantBufferView(2,scene_upload->GetGPUVirtualAddress()+1024);
            {
                KcdCameraInvocationScope public_scope(creator,list_identity.Get(),device_identity.Get(),creator.identity,
                    true,list.Get(),&parameters);
                KcdCameraInvocationScope native_scope(creator,list_identity.Get(),device_identity.Get(),creator.identity,
                    false,list.Get(),&parameters);
                DlssMotionVectorPublication publication;bool valid{};
                require(native_scope.apply(publication,list_identity.Get(),device_identity.Get(),creator.identity,7,3,valid)&&!valid,
                    "ambiguous public scene packets acquired an inner fallback");
            }
            check(list->Close(),"KCD scene negatives close");check(foreign->Close(),"KCD foreign negatives close");
        }
        if(scene_bindings)scene_upload->Unmap(0,nullptr);
        if(outlaws) {
            // These are actual cached public-entry calls into original typed
            // dispatchers on real canonical command-list/device identities.
            // A first child may record a snapshot, but an ambiguous/failed
            // enclosing original call must retire it before token resolution.
            ComPtr<ID3D12CommandAllocator> other_allocator,foreign_allocator;
            ComPtr<ID3D12GraphicsCommandList> other,foreign;
            make_list(other_allocator,other);
            check(foreign_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&foreign_allocator)),"Outlaws foreign allocator");
            check(foreign_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,foreign_allocator.Get(),
                nullptr,IID_PPV_ARGS(&foreign)),"Outlaws foreign native list");
            alignas(16) std::array<unsigned char,16> sr_storage{};
            direct_creation_handle=reinterpret_cast<const NVSDK_NGX_Handle*>(sr_storage.data());
            state().modules[0].typed_implementation=true;
            state().modules[0].create=&direct_creation_original;
            state().modules[0].evaluate=&actual_outlaws_native_original;
            NVSDK_NGX_Handle* sr{};parameters.present=true;
            require(NVSDK_NGX_SUCCEED(create<0>(other.Get(),NVSDK_NGX_Feature_SuperSampling,&parameters,&sr)),
                "Outlaws negative native SR creation");
            actual_outlaws_sr_handle=sr;
            const std::array failures{OutlawsNegative::foreign_list,OutlawsNegative::foreign_device,
                OutlawsNegative::wrong_type,OutlawsNegative::second_child,OutlawsNegative::native_failure,
                OutlawsNegative::outer_failure,OutlawsNegative::recursive_public,OutlawsNegative::recreated_public,
                OutlawsNegative::no_child,OutlawsNegative::changed_contract};
            context.recycle_on_release=false;context.reject_family=true;
            actual_family_use_alias=false;
            for(unsigned i=0;i<failures.size();++i) {
                ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
                make_list(allocator,list);context.list=list.Get();parameters.present=true;
                parameters.output=color.Get();parameters.motion=motion.Get();parameters.depth=depth.Get();
                actual_outlaws_negative=failures[i];
                actual_outlaws_other_list=failures[i]==OutlawsNegative::foreign_device?foreign.Get():other.Get();
                context.expect_original_failure=failures[i]==OutlawsNegative::outer_failure||failures[i]==OutlawsNegative::native_failure;
                sl1::Constants constants{};
                for(auto* matrix:{&constants.cameraViewToClip,&constants.clipToCameraView,&constants.clipToPrevClip,&constants.prevClipToClip}) {
                    std::memset(matrix,0,sizeof(*matrix));auto* values=reinterpret_cast<float*>(matrix);
                    values[0]=values[5]=values[10]=values[15]=1;
                }
                auto* projection=reinterpret_cast<float*>(&constants.cameraViewToClip);
                auto* inverse=reinterpret_cast<float*>(&constants.clipToCameraView);
                std::memset(projection,0,sizeof(constants.cameraViewToClip));
                std::memset(inverse,0,sizeof(constants.clipToCameraView));
                projection[0]=projection[5]=1;projection[11]=-1;projection[14]=.1F;
                inverse[0]=inverse[5]=1;inverse[11]=10;inverse[14]=-1;
                constants.cameraPos={103.F,0,0};constants.cameraRight={1,0,0};
                constants.cameraUp={0,1,0};constants.cameraFwd={0,0,-1};
                constants.cameraNear=.1F;constants.cameraFar=100;constants.cameraFOV=1;
                constants.depthInverted=static_cast<decltype(constants.depthInverted)>(1);
                require(set_constants(constants,16+i,0)&&evaluate_sl(reinterpret_cast<sl1::CommandBuffer*>(list.Get()),
                    sl1::eFeatureDLSS,16+i,0),"Outlaws original SL result changed by a rejected public child");
                check(list->Close(),"Outlaws negative production close");
                ID3D12CommandList* submitted[]{list.Get()};queue->ExecuteCommandLists(1,submitted);wait();
                std::uint32_t token{};const auto rejected_marker=read_luke_color_origin(consumer,color.Get(),&token,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,5000);
                require(FAILED(rejected_marker)||!resolve_luke_guide_token(token,consumer,device.Get()),
                    "Outlaws foreign/ambiguous/failed public call left a valid fenced color origin");
                if(failures[i]==OutlawsNegative::changed_contract) {
                    const auto lifetime=state().outlaws_public_lifetimes.at(handle);
                    require(lifetime->guide_contract_known&&lifetime->guide_contract.flags==parameters.flags&&
                        native_dispatch_metadata.creation_observed&&native_dispatch_metadata.flags==parameters.flags,
                        "changed actual typed creation did not supply the next warmup receipt");
                    require(native_dispatch_metadata.identity!=context.direct_native_identity&&
                        native_dispatch_metadata.creation_source_identity==native_dispatch_metadata.identity&&
                        native_dispatch_metadata.width==7&&native_dispatch_metadata.height==3&&
                        native_dispatch_metadata.out_width==512&&native_dispatch_metadata.out_height==32,
                        "genuine changed-flags creation altered its real native geometry or generation receipt");
                    // The next positive call belongs to this actual newly
                    // observed native generation, not the retired first one.
                    context.direct_native_identity=native_dispatch_metadata.identity;
                    const auto provisional=dlss_motion_vector_statistics().published;
                    require(!resolve_dlss_guide_publication(kKcdPublicStreamNamespace|lifetime->identity,provisional,queue.Get()),
                        "changed native flags retained the already recorded provisional guide transaction");
                }
            }
            actual_outlaws_negative=OutlawsNegative::none;actual_outlaws_other_list=nullptr;
            context.reject_family=false;context.expect_original_failure=false;
            // Retained native creation dimensions7x3 are a fallback. A genuine
            // per-evaluation6x2 render subrect must survive DLSS mode changes.
            {
                ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
                make_list(allocator,list);context.list=list.Get();context.expect_capture=true;
                parameters.present=true;parameters.output=color.Get();parameters.motion=motion.Get();parameters.depth=depth.Get();
                parameters.reset=0;parameters.scale_x=parameters.scale_y=1;
                parameters.jitter_x=.125F;parameters.jitter_y=-.25F;parameters.frame_time=17.F;
                parameters.render_width=6;parameters.render_height=2;
                sl1::Constants constants{};
                auto* projection=reinterpret_cast<float*>(&constants.cameraViewToClip);
                auto* inverse=reinterpret_cast<float*>(&constants.clipToCameraView);
                projection[0]=projection[5]=1;projection[11]=-1;projection[14]=.1F;
                inverse[0]=inverse[5]=1;inverse[11]=10;inverse[14]=-1;
                constants.cameraPos={103.F,0,0};constants.cameraRight={1,0,0};constants.cameraUp={0,1,0};constants.cameraFwd={0,0,-1};
                constants.cameraNear=.1F;constants.cameraFar=100;constants.cameraFOV=1;
                constants.depthInverted=static_cast<decltype(constants.depthInverted)>(1);
                require(set_constants(constants,64,0)&&evaluate_sl(reinterpret_cast<sl1::CommandBuffer*>(list.Get()),
                    sl1::eFeatureDLSS,64,0),"retained-handle render subrect original callback");
                require(context.frame&&context.frame->motion_width==6&&context.frame->motion_height==2&&
                    context.frame->depth_width==6&&context.frame->depth_height==2&&
                    context.frame->output_width==512&&context.frame->output_height==32&&
                    context.frame->scale_x==1&&context.frame->jitter_x==.125F&&!context.frame->reset&&
                    context.frame->frame_time_delta_ms==17.F&&native_dispatch_metadata.width==7&&
                    native_dispatch_metadata.height==3&&native_dispatch_metadata.flags==parameters.flags,
                    "dynamic public render subrect was replaced by static creation geometry or native modified scalars");
                check(list->Close(),"dynamic subrect production close");ID3D12CommandList* submitted[]{list.Get()};
                queue->ExecuteCommandLists(1,submitted);wait();
                std::uint32_t token{};check(read_luke_color_origin(consumer,color.Get(),&token,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,5000),"dynamic subrect final public origin");
                require(token==context.frame->color_origin_token,"dynamic subrect lost exact final color correlation");
                parameters.render_width=parameters.render_height=0;
            }
            {
                // Two threads cannot reserve the same next logical production.
                // Exercise the actual lifetime guard, including the interval
                // where the first caller has returned but overlap is live.
                ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
                make_list(allocator,list);parameters.output=color.Get();parameters.motion=motion.Get();parameters.depth=depth.Get();
                ComPtr<IUnknown> identity,owner;check(list.As(&identity),"overlap list identity");
                check(device.As(&owner),"overlap canonical device identity");
                const auto lifetime=state().outlaws_public_lifetimes.at(handle);
                const auto epoch=lifetime->failure_epoch;
                const auto stream=kKcdPublicStreamNamespace|lifetime->identity;
                const auto sequence=state().public_production_sequences.at(stream);
                std::atomic_bool second_ready{},release_second{};
                std::thread duplicate;
                bool overlap_valid{};
                {
                    OutlawsPublicInvocationScope first(3,handle,identity.Get(),owner.Get(),list.Get(),&parameters);
                    first.prepare_public_frame();
                    duplicate=std::thread([&] {
                        OutlawsPublicInvocationScope second(3,handle,identity.Get(),owner.Get(),list.Get(),&parameters);
                        second.prepare_public_frame();second.complete(NVSDK_NGX_Result_Success);
                        second_ready.store(true,std::memory_order_release);
                        while(!release_second.load(std::memory_order_acquire))std::this_thread::yield();
                    });
                    while(!second_ready.load(std::memory_order_acquire))std::this_thread::yield();
                    overlap_valid=lifetime->active_calls==2&&lifetime->failure_epoch!=epoch&&
                        state().public_production_sequences.at(stream)==sequence;
                    first.complete(NVSDK_NGX_Result_Success);
                }
                overlap_valid=overlap_valid&&lifetime->active_calls==1;
                {
                    const auto prior=lifetime->failure_epoch;
                    OutlawsPublicInvocationScope third(3,handle,identity.Get(),owner.Get(),list.Get(),&parameters);
                    third.prepare_public_frame();third.complete(NVSDK_NGX_Result_Success);
                    overlap_valid=overlap_valid&&lifetime->active_calls==2&&lifetime->failure_epoch!=prior&&
                        state().public_production_sequences.at(stream)==sequence;
                }
                release_second.store(true,std::memory_order_release);duplicate.join();
                require(overlap_valid,"live rejected overlap admitted a third public production after first completion");
                require(lifetime->active_calls==0&&!resolve_dlss_guide_publication(stream,
                    dlss_motion_vector_statistics().published,queue.Get()),
                    "overlap retained provisional history or leaked the generation lease");
                check(list->Close(),"overlap abandoned producer list close");
                ID3D12CommandList* submitted[]{list.Get()};queue->ExecuteCommandLists(1,submitted);wait();
            }
            check(other->Close(),"Outlaws negative other list close");
            check(foreign->Close(),"Outlaws negative foreign list close");
            std::cout<<"Outlaws public copyback: original target survives private composite overwrite; foreign list/device, native SR, duplicate child, native/wrapper failure, recursion and generation replacement retire admission PASS\n";
        }
        state().modules[release_slot].release=native_release;
        if(private_bootstrap_complete) {
            ResetEvent(done);check(private_bootstrap_complete->SetEventOnCompletion(1,done),"retire private bootstrap receipt");
            require(WaitForSingleObject(done,10000)==WAIT_OBJECT_0&&private_bootstrap_complete->GetCompletedValue()>=1,
                "private bootstrap allocator/list retired before exact completion");
            private_bootstrap_list.Reset();private_bootstrap_allocator.Reset();
        }
        if(logical_family) {
            const auto live_token=retained.back()->color_origin_token;
            const auto final_stream=retained.back()->stream;
            require(NVSDK_NGX_SUCCEED(context.release(handle))&&
                !resolve_luke_guide_token(calibrated->color_origin_token,queue.Get(),device.Get())&&
                !resolve_luke_guide_token(live_token,consumer,device.Get())&&
                (!outlaws||!state().outlaws_public_lifetimes.contains(handle)&&
                    !state().guide_streams.contains(handle)&&!state().public_production_sequences.contains(final_stream)),
                "public release left its native-eye family or clock alive");
        }
        {std::scoped_lock lock(state().mutex);state().active=false;state().queue.Reset();}
        native_ingress::stop_queue(queue.Get());configure_dlss_motion_vector_tracking(false);
        retire_disabled_dlss_motion_vector_resources();configure_luke_color_origin_extent(0,0);
        observer(nullptr);native_parameter_observer=nullptr;actual_ngx_capture_context=nullptr;CloseHandle(done);
        ComPtr<ID3D12InfoQueue> info;
        if(SUCCEEDED(device.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
            SIZE_T bytes{};info->GetMessage(i,nullptr,&bytes);std::vector<char> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());check(info->GetMessage(i,message,&bytes),"actual capture debug message");
            if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)
                std::cerr<<"Actual capture D3D12 message id="<<message->ID<<" severity="<<message->Severity
                    <<": "<<(message->pDescription?message->pDescription:"<empty>")<<'\n';
            require(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR&&message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,
                "actual capture D3D12 debug error");
        }
        std::cout<<"Actual native NGX capture: original scoped SL camera/NGX callback, four queried GPU owners, exact CRC colors/submission histories and callback-reused generation PASS\n";
        if(logical_family&&!outlaws)std::cout<<"KCD public source: full engine4/core2/REALVR3/native0 chain, direct/recovered native geometry, eleven strict ABA productions/calibration, actual capture miss and callback-reused public retirement PASS\n";
        if(outlaws)std::cout<<"Outlaws public RR: actual title-selected marked cached late-public3/native1 dispatch, no invented creation geometry, alternating native handles/exact SL camera, logical ABA clock/calibration, true capture gap and callback-reused creator lifetime PASS\n";
        if(dead_space)std::cout<<"Dead Space public SR: actual title-selected marked cached dispatcher, original SL camera, unchanged original RG16 typeless resource, bit-exact typed float16 current/previous owned GPU snapshots, color CRC and retirement PASS\n";
        if(gta)std::cout<<"GTA Enhanced public SR: actual title-selected marked cached dispatcher, exact GPU guides/color CRC and retirement PASS\n";
        if(scene_camera)std::cout<<"KCD scene camera: original public CBV/final Output, actual typed-native GPU guides/scalars after wrapper replacement and in-place edits, unchanged native delegation, deferred caller color CRC, strict list/once/gap and fenced private GPU bits/lifetime PASS\n";
        if(selector)std::cout<<"Luke private ingress: actual selector and original Execute, exact private CRC/current/N-1 fenced owners and GPU bits/lifetimes, unsubmitted and foreign rejection PASS\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
int run_actual_ngx_capture_test(const wchar_t* realvr,const wchar_t* streamline) {
    return run_actual_ngx_capture_pipeline(realvr,streamline,false);
}
int run_dead_space_ngx_capture_test(const wchar_t* realvr,const wchar_t* streamline) {
    return run_actual_ngx_capture_pipeline(realvr,streamline,false,false,nullptr,false,true);
}
int run_gta_enhanced_ngx_capture_test(const wchar_t* realvr,const wchar_t* streamline) {
    const auto root=std::filesystem::temp_directory_path()/
        (L"ofxr-gta-enhanced-diagnostics-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    {std::ofstream ini(root/"ofxr_bridge.ini");ini<<"[diagnostics]\nlogging_enabled=1\n";}
    auto& logger=bridge_flight_logger();logger.shutdown();logger.initialize(root);
    const auto result=run_actual_ngx_capture_pipeline(realvr,streamline,false,false,nullptr,false,false,true);
    if(result)return result;
    std::fflush(state().log);
    wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);
    std::ifstream file(std::filesystem::path(module).parent_path()/
        (L"ofxr-ngx-capture-pid"+std::to_wstring(GetCurrentProcessId())+L".log"));
    const std::string text((std::istreambuf_iterator<char>(file)),{});
    if(text.find("gta_enhanced_capture_gate")==std::string::npos||
        text.find("gta_enhanced_camera_receipt")==std::string::npos)return 1;
    test_gta_enhanced_production_identity=false;dead_space_capture_report_at=0;
    if(sample_dead_space_capture()||dead_space_capture_report_at!=0)return 1;
    logger.shutdown();return 0;
}
int run_dead_space_ingress_diagnostics_test(const wchar_t* realvr,const wchar_t* streamline) {
    const auto root=std::filesystem::temp_directory_path()/
        (L"ofxr-dead-space-diagnostics-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    {std::ofstream ini(root/"ofxr_bridge.ini");ini<<"[diagnostics]\nlogging_enabled=1\n";}
    auto& logger=bridge_flight_logger();logger.shutdown();logger.initialize(root);
    test_dead_space_diagnostics=true;
    const auto result=run_actual_ngx_capture_pipeline(realvr,streamline,false,false,nullptr,false,true);
    if(result)return result;
    if(state().log)fflush(state().log);
    wchar_t module[MAX_PATH]{};GetModuleFileNameW(nullptr,module,MAX_PATH);
    const auto path=std::filesystem::path(module).parent_path()/
        (L"ofxr-ngx-capture-pid"+std::to_wstring(GetCurrentProcessId())+L".log");
    std::ifstream input(path);const std::string text(std::istreambuf_iterator<char>(input),{});
    for(unsigned reason:{0U,1U,3U,11U,12U})if(text.find("dead_space_capture_gate a="+std::to_string(reason)+" ")==std::string::npos) {
        std::cerr<<"Missing actual capture rejection/success diagnostic "<<reason<<"\n";return 1;
    }
    if(text.find("dead_space_camera_receipt")==std::string::npos || text.find("dead_space_queue_state")==std::string::npos)return 1;
    if(text.find("dead_space_camera_right a=1073741824 b=0 c=0 ")==std::string::npos ||
        text.find("dead_space_camera_projection_3")==std::string::npos)
        {std::cerr<<"Rejected basis raw float bits/matrix missing\n";return 1;}
    dead_space_capture_report_at=0;test_dead_space_production_identity=false;
    if(sample_dead_space_capture()||dead_space_capture_report_at!=0)return 1;
    test_dead_space_production_identity=true;logger.shutdown();dead_space_capture_report_at=0;
    if(sample_dead_space_capture()||dead_space_capture_report_at!=0)return 1;
    std::cout<<"Dead Space diagnostic gates: actual inactive/unsubmitted/flags/camera/success, one-second limit, non-title/F9-off bypass and unchanged GPU results PASS\n";
    return 0;
}
int run_ghost_ngx_capture_test(const wchar_t* realvr,const wchar_t* streamline,bool steamvr) {
    test_ghost_native_motion=true;
    set_ngx_capture_steamvr_runtime(steamvr);
    if(!same_eye_native_motion()) {
        std::cerr<<"Ghost same-eye native interval depends on the XR runtime\n";return 1;
    }
    return run_actual_ngx_capture_pipeline(realvr,streamline,false);
}
int run_horizon_ngx_capture_test(const wchar_t* realvr,const wchar_t* streamline,bool steamvr) {
    test_horizon_native_motion=true;
    set_ngx_capture_steamvr_runtime(steamvr);
    if(!same_eye_native_motion()) {
        std::cerr<<"Horizon same-eye native interval depends on the XR runtime\n";return 1;
    }
    return run_actual_ngx_capture_pipeline(realvr,streamline,false);
}
int run_crimson_ngx_capture_test(const wchar_t* realvr,const wchar_t* streamline) {
    test_crimson_native_motion=true;
    return run_actual_ngx_capture_pipeline(realvr,streamline,false);
}
// An unlisted REAL VR title: N-2 exactly when its typed SR call received
// other MotionVectors than the public call, N-1 when it received the game's.
int run_unlisted_motion_capture_test(const wchar_t* realvr,const wchar_t* streamline,bool swap) {
    test_unlisted_typed_dispatch=true;test_unlisted_motion_swap=swap;
    if(same_eye_native_motion()) {
        std::cerr<<"the unlisted fixture is on the measured same-eye title list\n";return 1;
    }
    return run_actual_ngx_capture_pipeline(realvr,streamline,false);
}
int run_kcd_logical_capture_test(const wchar_t* realvr,const wchar_t* streamline) {
    return run_actual_ngx_capture_pipeline(realvr,streamline,true);
}
int run_kcd_scene_capture_test(const wchar_t* realvr,const wchar_t* streamline,SelectLukeExecutionQueue selector) {
    return run_actual_ngx_capture_pipeline(realvr,streamline,true,true,selector);
}
int run_luke_private_capture_test(const wchar_t* realvr,const wchar_t* streamline,SelectLukeExecutionQueue selector) {
    return run_actual_ngx_capture_pipeline(realvr,streamline,false,false,selector);
}
int run_outlaws_logical_capture_test(const wchar_t* realvr,const wchar_t* streamline,SelectLukeExecutionQueue selector) {
    return run_actual_ngx_capture_pipeline(realvr,streamline,true,false,selector,true);
}
