#include "xrfg/autonomous_control.hpp"
#include "xrfg/desktop_profiles.hpp"
#include "xrfg/benchmark_suite.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace a=xrfg::autonomous;
namespace {
void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
std::string read(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
struct Child {
    PROCESS_INFORMATION process{};
    HANDLE ready{},proceed{};
    Child(const std::wstring& exe,const std::wstring& game,const std::wstring& mode,const std::wstring& suffix) {
        const auto event=L"Local\\OFXR-v637-profile-"+suffix;
        ready=CreateEventW(nullptr,TRUE,FALSE,(event+L"-ready").c_str());
        proceed=CreateEventW(nullptr,TRUE,FALSE,(event+L"-go").c_str());
        auto command=L"\""+exe+L"\" --child "+game+L" "+mode+L" "+event;
        STARTUPINFOW startup{sizeof(startup)};
        check(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"spawn profile process");
        check(WaitForSingleObject(ready,5000)==WAIT_OBJECT_0,"profile child ready");
    }
    void finish(){SetEvent(proceed);check(WaitForSingleObject(process.hProcess,5000)==WAIT_OBJECT_0,"profile child exit");DWORD code{};GetExitCodeProcess(process.hProcess,&code);check(code==0,"child profile assertions");}
    ~Child(){if(process.hProcess){if(WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT){SetEvent(proceed);WaitForSingleObject(process.hProcess,5000);}CloseHandle(process.hProcess);CloseHandle(process.hThread);}if(ready)CloseHandle(ready);if(proceed)CloseHandle(proceed);}
};
int child(const std::filesystem::path& game,const std::wstring& mode,const std::wstring& event) {
    if(mode==L"failure") {
        check(!a::initialize_game_configuration(game),"reject directory in place of profile");
        check(a::configuration_path()==a::global_configuration_path(),"failed binding does not claim game profile");
        HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,(event+L"-ready").c_str());
        HANDLE go=OpenEventW(SYNCHRONIZE,FALSE,(event+L"-go").c_str());SetEvent(ready);
        check(WaitForSingleObject(go,5000)==WAIT_OBJECT_0,"failure child rendezvous");CloseHandle(ready);CloseHandle(go);return 0;
    }
    check(a::initialize_game_configuration(game),"initialize game before startup choices");
    const auto first=a::snapshot().desired;
    a::Settings factory;factory.advanced_mode=first.advanced_mode;factory=a::sanitize(factory);
    if(mode==L"factory")check(first==factory && !first.nr_second_pass,"reset survives next process launch without resetting global UI preference");
    else if(mode==L"persisted")check(first.backend==3 && first.nr_second_pass && first.nr_intensity==.625F,
        "same game restores its second-pass choice and shared NR settings on restart");
    else check(first.backend==1 && first.scale==3 && first.afw_only && first.advanced_mode && first.nr_second_pass,
        "new game inherits customized main settings including second NR pass");
    check(a::configuration_path()==a::game_configuration_path(game),"process bound to own game profile");
    {const auto global=a::global_configuration_path();a::ScopedConfigurationSelection selected(global);
     check(a::configuration_path()==a::game_configuration_path(game),"desktop selection cannot redirect a bound game/headset profile");}
    check(read(a::configuration_path()).find("[tray]")==std::string::npos && read(a::configuration_path()).find("[benchmark]")==std::string::npos &&
        read(a::configuration_path()).find("AdvancedMode")==std::string::npos,"global sections and AdvancedMode excluded from game files");
    if(mode!=L"factory") {auto edit=first;edit.backend=3;edit.nr_intensity=.625F;edit.nr_second_pass=mode!=L"reset";check(a::request(edit),"game edit saved");}
    a::RuntimeStatus report{};report.sessions=report.ready=1;a::report_runtime(report);
    if(mode==L"benchmark") {
        const auto original=a::snapshot().desired;
        check(a::start_benchmark(false),"suite starts in game profile with global enable flag");
        a::tick_benchmark();
        check(a::snapshot().desired.nr_second_pass==original.nr_second_pass,
            "automatic route switches preserve the shared Neural Rendering second-pass choice");
        const auto csv=std::filesystem::u8path(a::snapshot().benchmark.csv_path);
        check(csv.parent_path()==a::common_folder()/L"Benchmarks","profile benchmark output remains in common folder");
        a::stop_benchmark();a::tick_benchmark();
        check(a::snapshot().desired==original,"benchmark restores only the game profile");
    }
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,(event+L"-ready").c_str());
    HANDLE go=OpenEventW(SYNCHRONIZE,FALSE,(event+L"-go").c_str());
    check(ready && go,"open child rendezvous");SetEvent(ready);
    check(WaitForSingleObject(go,5000)==WAIT_OBJECT_0,"continue child");
    if(mode==L"reset")check(a::snapshot().desired==factory,"desktop reset reached game without resetting global UI preference");
    if(mode==L"advanced-flip") {const auto current=a::snapshot();check(!current.desired.advanced_mode&&
        current.desired.pacing_mode==1&&current.desired.preset!=2&&current.desired.scale==3&&
        !current.desired.backward&&!current.desired.afw_debug&&!current.desired.r19_camera,
        "already bound child process observes normalized settings after global Advanced Off");}
    if(mode==L"independent")check(a::snapshot().desired.nr_intensity==.625F && a::snapshot().desired.nr_second_pass,
        "other game's reset did not overwrite this game's NR settings or second-pass choice");
    a::report_runtime({});CloseHandle(ready);CloseHandle(go);return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    std::filesystem::path root;
    try {
        if(argc==5 && std::wstring_view(argv[1])==L"--child")return child(argv[2],argv[3],argv[4]);
        root=std::filesystem::temp_directory_path()/(L"ofxr-v637-profiles-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",root.c_str());
        check(a::initialize_user_configuration(),"initialize base configuration");
        auto defaults=a::Settings{};check(!defaults.advanced_mode && !defaults.afw_only && defaults.backend==2 && defaults.scale==2 && !defaults.nr_second_pass,"release factory defaults");
        auto customized=defaults;customized.advanced_mode=true;customized.backend=1;customized.scale=3;customized.afw_only=true;
        customized.nr_second_pass=true;
        check(a::request(customized),"customize desktop template");
        check(a::write_configuration_sections(a::global_configuration_path(),"[tray]\npacing=force_vdxr\ncrash_debug=1\n[benchmark]\nenabled=1\n[dlssg]\npreset=3\n"),"save global choices");
        const auto original=read(a::global_configuration_path());
        a::DesktopProfiles desktop;
        desktop.observe(a::active_game_profile());
        check(desktop.choose(a::global_configuration_path()),"desktop pins Global before a game launch");
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
        const auto suffix=std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        {
            Child alpha(executable,L"Alpha.exe",L"independent",suffix+L"-a");
            check(a::configuration_path()==a::game_configuration_path(L"Alpha.exe"),"desktop discovers running game");
            desktop.observe(a::active_game_profile());
            check(desktop.selected()==a::game_configuration_path(L"Alpha.exe"),"already-open desktop automatically follows a new process");
            check(desktop.choose(a::global_configuration_path()),"manually select Global while Alpha runs");
            desktop.observe(a::active_game_profile());check(desktop.selected()==a::global_configuration_path(),"same running process does not override a manual desktop choice");
            {a::ScopedConfigurationSelection selected(desktop.selected());check(a::snapshot().desired==a::sanitize(customized),"selected Global UI reads Global preferences");
             {a::ScopedConfigurationSelection hotkey;check(a::configuration_path()==a::game_configuration_path(L"Alpha.exe"),"reentrant tray message follows live game even inside a desktop profile operation");}
             check(a::configuration_path()==a::global_configuration_path(),"reentrant message restores the desktop's selected profile");}
            check(a::configuration_path()==a::game_configuration_path(L"Alpha.exe"),"outside desktop scope tray actions still target active Alpha");
            check(a::snapshot().desired.nr_intensity==.625F && a::snapshot().desired.nr_second_pass,"desktop follows active game mapping");
            {
                Child beta(executable,L"Beta.exe",L"reset",suffix+L"-b");
                check(a::configuration_path()==a::game_configuration_path(L"Beta.exe"),"second game has separate mapping");
                desktop.observe(a::active_game_profile());check(desktop.selected()==a::game_configuration_path(L"Beta.exe"),"new game overrides manual desktop Global selection");
                check(!a::snapshot().desired.nr_second_pass,"second game can disable its inherited second pass independently");
                check(a::reset_to_defaults(),"reset active game");
                check(!a::snapshot().desired.nr_second_pass,"factory reset disables second Neural Rendering pass");
                check(GetPrivateProfileIntW(L"dlssg",L"preset",0,a::configuration_path().c_str())==1,"reset restores factory preset A preference");
                check(read(a::global_configuration_path())==original,"game changes/reset leave main byte-identical");
                check(a::benchmark_enabled(),"benchmark flag remains global");
                beta.finish();
            }
            alpha.finish();
        }
        check(a::configuration_path()==a::global_configuration_path(),"desktop returns to global defaults on disconnect");
        check(a::snapshot().desired.backend==1,"global settings restored without game contamination");
        {Child beta(executable,L"Beta.exe",L"factory",suffix+L"-c");beta.finish();}
        {Child alpha(executable,L"Alpha.exe",L"persisted",suffix+L"-persisted");alpha.finish();}
        check(read(a::global_configuration_path())==original,"restart still leaves main unchanged");
        {Child gamma(executable,L"Gamma.exe",L"benchmark",suffix+L"-bench");gamma.finish();}
        check(read(a::global_configuration_path())==original,"game benchmark leaves main unchanged");
        std::filesystem::create_directory(a::game_configuration_path(L"Broken.exe"));
        {Child broken(executable,L"Broken.exe",L"failure",suffix+L"-fail");broken.finish();}
        check(read(a::global_configuration_path())==original,"failed profile initialization never alters main");
        {
            Child live(executable,L"Live.exe",L"advanced-flip",suffix+L"-advanced");
            auto live_settings=a::snapshot().desired;live_settings.pacing_mode=2;live_settings.preset=0;live_settings.scale=3;live_settings.backward=true;live_settings.r19_camera=true;live_settings.afw_debug=true;
            check(a::request(live_settings),"select an existing advanced game route");
            const auto live_revision=a::snapshot().revision;
            const auto global=a::global_configuration_path();
            {a::ScopedConfigurationSelection selected(global);auto settings=a::snapshot().desired;
             settings.advanced_mode=false;check(a::request(settings),"change only the global UI policy");}
            check(!a::snapshot().desired.advanced_mode,"active profile immediately reflects the global preference");
            const auto normalized=a::snapshot();
            check(normalized.desired.pacing_mode==1 && normalized.revision>live_revision &&
                normalized.desired.preset==0&&normalized.desired.scale==3&&!normalized.desired.backward&&
                !normalized.desired.r19_camera&&!normalized.desired.afw_debug,
                "global Advanced Off publishes default hidden settings as an applicable game revision");
            check(a::snapshot().revision==normalized.revision,"observing an already normalized profile does not churn revisions");
            live.finish();
        }
        {
            const auto global=a::global_configuration_path();
            {a::ScopedConfigurationSelection selected(global);auto settings=a::snapshot().desired;
             settings.advanced_mode=true;settings.scale=3;check(a::request(settings),"enable Advanced for live suite restore fixture");}
            Child live(executable,L"LiveBench.exe",L"advanced-flip",suffix+L"-advanced-suite");
            auto saved=a::snapshot().desired;saved.pacing_mode=2;saved.scale=3;saved.r19_camera=true;
            check(a::request(saved)&&a::start_benchmark(false),"start a suite with saved Advanced choices");
            a::tick_benchmark();
            {a::ScopedConfigurationSelection selected(global);auto settings=a::snapshot().desired;
             settings.advanced_mode=false;check(a::request(settings),"turn Advanced off during a suite");}
            check(a::snapshot().benchmark.phase==a::BenchmarkPhase::stopped,"global policy stops active suite");
            a::tick_benchmark();const auto restored=a::snapshot();
            check(!restored.desired.advanced_mode&&restored.desired.pacing_mode==1&&
                restored.desired.preset==1&&restored.desired.scale==3&&!restored.desired.r19_camera,
                "suite restore cannot resurrect the saved Advanced policy or hidden custom values");
            live.finish();
        }
        const auto global_after=read(a::global_configuration_path());
        const auto legacy=a::game_configuration_path(L"Legacy.exe");
        {std::ofstream file(legacy);file<<"[ofxr]\nAdvancedMode=1\npacing_mode=2\nbackend=2\nmotion_vectors=1\n";}
        {a::ScopedConfigurationSelection selected(legacy);
         auto settings=a::snapshot().desired;
         check(!settings.advanced_mode && settings.pacing_mode==1 && !settings.motion_vectors,
            "legacy per-game flag cannot override global basic policy before sanitation");
         auto edit=settings;edit.advanced_mode=true;edit.nr_intensity=.75F;
         check(a::request(edit) && !a::snapshot().desired.advanced_mode,"game request cannot change global UI preference");
         check(read(legacy).find("AdvancedMode")==std::string::npos,"saving migrates away the legacy per-game key");
         check(a::request_changes(settings,edit) && !a::snapshot().desired.advanced_mode,"game delta cannot change global UI preference");
         check(a::reset_to_defaults() && !a::snapshot().desired.advanced_mode,"game reset preserves global basic policy");}
        check(read(a::global_configuration_path())==global_after,"all game actions leave the global file byte-identical");
        check(a::parse_settings("[ofxr]\nmotion_vectors=1\n").motion_vectors==0,"basic mode forces optical flow");
        check(a::parse_settings("[ofxr]\nAdvancedMode=1\nmotion_vectors=1\n").motion_vectors==1,"advanced mode preserves native selection");
        check(a::parse_settings("[ofxr]\nframe_generation=1\n").motion_vectors==1,"basic mode retains FSR native guides");
        check(a::parse_settings("[ofxr]\nbenchmark_csv=0\n").benchmark_csv,"benchmark CSV cannot be disabled");
        std::filesystem::remove_all(root);
        std::cout<<"Game inheritance, two-process isolation, discovery, factory reset, global independence and basic-mode checks passed\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
