// Reuse the established D3D12 fixture without changing its regression matrix.
#define main xrfg_history_tests_unused_entry
#include "d3d12_history_tests.cpp"
#undef main
#include "xrfg/rife_runtime_locator.hpp"
#include <iterator>
#include <optional>

namespace {
class ScopedTestAppData {
public:
    explicit ScopedTestAppData(const std::filesystem::path& evidence) {
        wchar_t previous[32768]{};
        if(GetEnvironmentVariableW(L"LOCALAPPDATA",previous,32768)) previous_=previous;
        directory_=std::filesystem::absolute(evidence)/
            (L"appdata-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(!std::filesystem::exists(directory_),"native status test requires an empty owned cache");
        std::filesystem::create_directories(directory_);
        require(SetEnvironmentVariableW(L"LOCALAPPDATA",directory_.c_str())!=FALSE,
            "isolate native worker/compiler cache from user profiles");
    }
    ~ScopedTestAppData() {
        SetEnvironmentVariableW(L"LOCALAPPDATA",previous_?previous_->c_str():nullptr);
    }
    const auto& directory()const noexcept{return directory_;}
private:
    std::optional<std::wstring> previous_;
    std::filesystem::path directory_;
};

class ScopedRuntimeLocator {
public:
    explicit ScopedRuntimeLocator(const std::filesystem::path& runtime) {
        require(xrfg::valid_rife_runtime(runtime),"explicit isolated V800 runtime is required");
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
        path_=std::filesystem::path(executable).parent_path()/L"ofxr_bridge.ini";
        if(std::filesystem::exists(path_)) {
            std::ifstream before(path_,std::ios::binary);
            require(bool(before),"read existing test-local runtime locator before replacement");
            previous_=std::string(std::istreambuf_iterator<char>(before),{});
        }
        const auto utf8=runtime.generic_u8string();
        std::ofstream output(path_,std::ios::binary|std::ios::trunc);
        output<<"[ofxr]\nrife_runtime_dir=";
        output.write(reinterpret_cast<const char*>(utf8.data()),static_cast<std::streamsize>(utf8.size()));
        output<<"\n";
        require(bool(output),"write owned native-test runtime locator");
    }
    ~ScopedRuntimeLocator() {
        if(previous_) {
            std::ofstream output(path_,std::ios::binary|std::ios::trunc);output<<*previous_;
        } else {
            std::error_code ignored;std::filesystem::remove(path_,ignored);
        }
    }
private:
    std::filesystem::path path_;
    std::optional<std::string> previous_;
};

void require_native_stereo(D3D12WarpFixture& fixture,ID3D12Resource* output) {
    const auto pixels=readback_pattern(fixture,output,D3D12_RESOURCE_STATE_RENDER_TARGET);
    for(UINT eye=0;eye<2;++eye) {
        require(!pixels[eye].empty(),"fallback and TensorRT must transport both native eyes");
        double red=0;std::size_t count=0;
        for(std::size_t i=0;i<pixels[eye].size();i+=4) {
            require(pixels[eye][i+3]==255,"native stereo output remains opaque");
            red+=pixels[eye][i];++count;
        }
        require(eye==0?red/count>150:red/count<80,
            "Torch/TensorRT fallback must preserve independent left and right colour inputs");
    }
}

void test_async_optimization_receipts(D3D12WarpFixture& fixture,const std::filesystem::path& cache) {
    constexpr UINT width=512,height=256;
    auto a=create_source_texture(fixture,width,height),b=create_source_texture(fixture,width,height);
    auto output=create_source_texture(fixture,width,height);
    StereoPattern before,after;
    for(UINT eye=0;eye<2;++eye) {
        before[eye].resize(width*height*4);after[eye].resize(width*height*4);
        for(std::size_t i=0;i<before[eye].size();i+=4) {
            before[eye][i]=eye?50:200;after[eye][i]=eye?30:180;
            before[eye][i+1]=after[eye][i+1]=eye?140:55;
            before[eye][i+2]=45;after[eye][i+2]=70;
            before[eye][i+3]=after[eye][i+3]=255;
        }
    }
    upload_pattern(fixture,a.Get(),before);upload_pattern(fixture,b.Get(),after);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* commands){
        const std::array transitions{
            transition_barrier(a.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(b.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON)};
        commands->ResourceBarrier(static_cast<UINT>(transitions.size()),transitions.data());
    });
    const auto views=make_reprojection_views();
    const std::array<std::array<float,4>,2> rotations{{{0,0,0,1},{0,0,0,1}}};
    bool saw_optimizing=false,saw_tensorrt=false;
    unsigned fallback_outputs=0;double max_call_ms=0;
    {
        xrfg::D3D12Rife generator;
        require_hresult(generator.initialize(fixture.device(),fixture.queue(),kFormat,3,.15F),
            "initialize portable RIFE4.4 optimization test");
        const auto deadline=GetTickCount64()+310'000;
        do {
            const auto started=std::chrono::steady_clock::now();
            require_hresult(generator.generate(a.Get(),b.Get(),output.Get(),views,views,views,
                rotations,rotations,D3D12_RESOURCE_STATE_RENDER_TARGET,.5F),
                "fallback/TensorRT native stereo generation");
            const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            max_call_ms=std::max(max_call_ms,elapsed);
            require(elapsed<8000,"engine compilation must not block native generation for tens of seconds");
            const auto state=xrfg::rife_execution_status();
            require(state.active && state.updated_ms,"infer replies publish execution status without F9 or memory sampling");
            if(state.optimizing) {
                require(!state.tensorrt,"optimizing receipt identifies running Torch fallback");
                if(!saw_optimizing)require_native_stereo(fixture,output.Get());
                saw_optimizing=true;++fallback_outputs;
            }
            if(state.tensorrt) {
                require(!state.optimizing,"successful TensorRT activation clears optimization status");
                require_native_stereo(fixture,output.Get());saw_tensorrt=true;break;
            }
            require(state.optimizing,"installed fresh backend must optimize rather than silently fail to Torch");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }while(GetTickCount64()<deadline);
        require(saw_optimizing && fallback_outputs>=2,"cold engine builds while usable Torch stereo output continues");
        require(saw_tensorrt,"new engine must become active within the integration deadline");
        for(float fraction:{.25F,.5F,.75F}) {
            require_hresult(generator.generate(a.Get(),b.Get(),output.Get(),views,views,views,
                rotations,rotations,D3D12_RESOURCE_STATE_RENDER_TARGET,fraction),"compiled fractional generation");
            const auto state=xrfg::rife_execution_status();
            require(state.active && state.tensorrt && !state.optimizing,
                "subsequent compiled fractions do not falsely re-enter optimization");
            require_native_stereo(fixture,output.Get());
        }
    }
    require(!xrfg::rife_execution_status().active,"destroyed RIFE worker cannot leave a stale optimization receipt");
    {
        xrfg::D3D12Rife cached;
        require_hresult(cached.initialize(fixture.device(),fixture.queue(),kFormat,3,.15F),"initialize cached worker");
        require_hresult(cached.generate(a.Get(),b.Get(),output.Get(),views,views,views,
            rotations,rotations,D3D12_RESOURCE_STATE_RENDER_TARGET,.5F),"first cached-worker generation");
        const auto state=xrfg::rife_execution_status();
        require(state.active && state.tensorrt && !state.optimizing,
            "fresh process loads the existing engine without a false optimization status");
        require_native_stereo(fixture,output.Get());
    }
    fixture.require_no_debug_errors();
    // This cache belongs solely to the test. Simulate the logged compiler
    // timeout after a successful run, then start a fresh protocol worker.
    std::filesystem::path engine;
    for(const auto& entry:std::filesystem::directory_iterator(cache))
        if(entry.path().extension()==L".engine")engine=entry.path();
    require(!engine.empty(),"compiled engine is present in the owned test cache");
    const auto backup=engine.wstring()+L".test-backup";
    std::filesystem::rename(engine,backup);
    const auto failure=cache/(engine.stem().wstring()+L".failed.json");
    {std::ofstream file(failure);file<<"{\"reason\":\"TensorRT optimization timed out\",\"retry_after\":4102444800}";}
    {
        xrfg::D3D12Rife failed;
        require_hresult(failed.initialize(fixture.device(),fixture.queue(),kFormat,3,.15F),"initialize failed-cache worker");
        require_hresult(failed.generate(a.Get(),b.Get(),output.Get(),views,views,views,rotations,rotations,
            D3D12_RESOURCE_STATE_RENDER_TARGET,.5F),"failed compilation still has safe fallback output");
        const auto state=xrfg::rife_execution_status();
        require(state.active && !state.optimizing && !state.tensorrt && state.error==1,
            "compiler timeout crosses the real Python/native protocol as an explicit error");
        require_native_stereo(fixture,output.Get());
    }
    std::filesystem::remove(failure);std::filesystem::rename(backup,engine);
    std::cout<<"RIFE_STATUS cold_fallback_outputs="<<fallback_outputs<<" max_native_call_ms="<<max_call_ms
        <<" compiled_fractions=3 cached_new_worker=1 eyes=2 logging_required=0\n";
}
} // namespace

int main(int argc,char** argv) {
    try {
        require(argc>=2,"usage: xrfg_rife_tensorrt_status_tests <isolated-V800-runtime> [evidence-directory]");
        const auto runtime=std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        const auto evidence=argc>=3?std::filesystem::u8path(argv[2]):
            std::filesystem::temp_directory_path()/L"ofxr-rife-v800-status";
        ScopedTestAppData appdata(evidence);
        ScopedRuntimeLocator locator(runtime);
        std::cout<<"RIFE_STATUS owned_appdata="<<appdata.directory().string()<<'\n';
        D3D12WarpFixture fixture(true);
        test_async_optimization_receipts(fixture,appdata.directory()/L"ofxr-bridge/RIFECache");
        return 0;
    }catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
