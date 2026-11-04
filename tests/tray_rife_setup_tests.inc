std::function<void(HWND)> rife_dialog_script;
std::exception_ptr rife_dialog_error;
INT_PTR run_rife_dialog(RifeInstallDialog& draft,std::function<void(HWND)> script) {
    rife_dialog_error={};rife_dialog_script=std::move(script);
    const auto timer=SetTimer(nullptr,0,50,[](HWND,UINT,UINT_PTR id,DWORD){
        KillTimer(nullptr,id);const auto window=GetActiveWindow();
        try{require(window!=nullptr,"actual RIFE confirmation dialog exists");rife_dialog_script(window);}
        catch(...){rife_dialog_error=std::current_exception();if(window)EndDialog(window,IDCANCEL);}
    });
    const auto result=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_RIFE_INSTALL),nullptr,
        rife_install_dialog_procedure,reinterpret_cast<LPARAM>(&draft));
    KillTimer(nullptr,timer);if(rife_dialog_error)std::rethrow_exception(rife_dialog_error);return result;
}
void rife_setup_ui_tests() {
    namespace r=xrfg::rife_setup;
    RifeInstallDialog dialog{L"C:\\Users\\Tester\\AppData\\Local\\ofxr-bridge\\RIFE\\runtime",L"RIFE is not installed."};
    auto answer=run_rife_dialog(dialog,[&](HWND window){
        require(IsDlgButtonChecked(window,IDC_RIFE_NVIDIA)==BST_CHECKED,"NVIDIA selected by default");
        require(IsWindowEnabled(GetDlgItem(window,IDC_RIFE_NVIDIA))!=FALSE,"NVIDIA is available");
        require(IsWindowEnabled(GetDlgItem(window,IDC_RIFE_AMD))==FALSE,"AMD Radeon RDNA stays disabled");
        require(IsDlgButtonChecked(window,IDC_RIFE_AMD)==BST_UNCHECKED,"future AMD is not selected");
        wchar_t text[1024]{};GetDlgItemTextW(window,IDC_RIFE_DESTINATION,text,1024);
        require(std::filesystem::path(text)==dialog.destination,"actual AppData destination is shown");
        GetDlgItemTextW(window,IDC_RIFE_REASON,text,1024);require(std::wstring(text)==dialog.reason,"check failure reason is shown");
        GetDlgItemTextW(window,IDC_RIFE_SPACE,text,1024);
        require(std::wstring_view(text).find(L"8 GB")!=std::wstring_view::npos&&std::wstring_view(text).find(L"14 GB")!=std::wstring_view::npos&&
            std::wstring_view(text).find(L"caches")!=std::wstring_view::npos,"actual dialog warns about installed, staging and future cache space");
        GetDlgItemTextW(window,IDC_RIFE_PROGRESS,text,1024);
        require(std::wstring_view(text).find(L"PowerShell")!=std::wstring_view::npos&&std::wstring_view(text).find(L"No further input")!=std::wstring_view::npos,
            "actual dialog explains visible unattended progress");
        // Even a synthetic control change cannot submit the disabled backend.
        CheckRadioButton(window,IDC_RIFE_NVIDIA,IDC_RIFE_AMD,IDC_RIFE_AMD);
        SendMessageW(window,WM_COMMAND,IDOK,0);require(IsWindow(window)!=FALSE,"unavailable backend cannot submit installation");
        CheckRadioButton(window,IDC_RIFE_NVIDIA,IDC_RIFE_AMD,IDC_RIFE_NVIDIA);
        SendMessageW(window,WM_COMMAND,IDOK,0);
    });
    require(answer==IDOK,"actual NVIDIA confirmation accepts Install");
    answer=run_rife_dialog(dialog,[](HWND window){SendMessageW(window,WM_COMMAND,IDCANCEL,0);});
    require(answer==IDCANCEL,"Cancel does not accept an installation");
    const std::filesystem::path powershell=L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    const std::filesystem::path script=L"C:\\Package O'Brian $()\\tools\\Setup-RifeRuntime.ps1";
    const std::filesystem::path root=L"C:\\App Data\\T\u00e9st $()\\RIFE\\runtime\\";
    const std::filesystem::path result=L"C:\\App Data\\check result.json";
    for(bool check:{true,false}) {
        const auto command=r::command_line(powershell,script,root,result,check);
        int count{};auto args=CommandLineToArgvW(command.c_str(),&count);require(args!=nullptr,"parse actual Windows command line");
        std::vector<std::wstring> values;for(int i=0;i<count;++i)values.emplace_back(args[i]);LocalFree(args);
        require(values.front()==powershell.wstring(),"explicit system PowerShell executable");
        const auto after=[&](const wchar_t* option){auto at=std::find(values.begin(),values.end(),option);require(at!=values.end()&&at+1!=values.end(),"option has quoted value");return *(at+1);};
        require(after(L"-File")==script.wstring()&&after(L"-Root")==root.wstring()&&after(L"-ResultJson")==result.wstring(),
            "spaces, apostrophes, shell metacharacters, Unicode and trailing slash survive quoting");
        require(std::find(values.begin(),values.end(),L"-NonInteractive")!=values.end(),"PowerShell never asks for input");
        require((std::find(values.begin(),values.end(),L"-Check")!=values.end())==check,"only health check carries Check switch");
        require(std::find(values.begin(),values.end(),L"-Command")==values.end(),"file arguments are not PowerShell expressions");
    }
    require(r::process_flags(false)==CREATE_NO_WINDOW&&r::process_flags(true)==CREATE_NEW_CONSOLE,
        "checks hidden, confirmed installation console visible");
    const auto json=[](std::string_view status){return std::string("{\"schema\":1,\"status\":\"")+std::string(status)+
        "\",\"message\":\"test\\nmessage\",\"root\":\"C:\\\\T\\u00e9st\\\\runtime\",\"backend\":\"nvidia\",\"installed_bytes\":8123456789,\"required_free_bytes\":15032385536,\"log_path\":\"C:\\\\logs\\\\check.log\"}";};
    for(const auto& [name,status]:std::array<std::pair<const char*,r::Status>,6>{{{"ready",r::Status::ready},{"missing",r::Status::missing},
        {"repair",r::Status::repair},{"unsupported",r::Status::unsupported},{"busy",r::Status::busy},{"error",r::Status::error}}}) {
        const auto parsed=r::parse_result(json(name),r::expected_exit(status));require(parsed.has_value(),"parse every documented backend status");
        require(parsed->status==status&&parsed->installed_bytes==8123456789ULL&&parsed->required_free_bytes==15032385536ULL,"preserve status and 64-bit sizes");
        require(parsed->root==std::filesystem::path(L"C:\\T\u00e9st\\runtime")&&parsed->message==L"test\nmessage","decode JSON paths and Unicode escapes");
        require(r::offers_install(status)==(status==r::Status::missing||status==r::Status::repair),"only missing/repair can ask to install");
        require(!r::parse_result(json(name),999),"exit-code mismatch never becomes an install request");
    }
    require(!r::parse_result(json("future-unknown"),0),"unknown health status rejected");
    const auto replace=[](std::string text,std::string_view before,std::string_view after) {
        const auto at=text.find(before);require(at!=text.npos,"negative fixture field exists");text.replace(at,before.size(),after);return text;
    };
    auto invalid=r::parse_result(replace(json("missing"),"\"status\":\"missing\"","\"status\":missing"),10);
    require(!invalid&&!(invalid&&r::offers_install(invalid->status)),"bare missing identifier is invalid JSON and cannot offer Install");
    require(!r::parse_result(replace(json("missing"),"\"status\":\"missing\"","\"status\":null"),10),"status must be a JSON string");
    require(!r::parse_result(replace(json("ready"),"\"schema\":1","\"schema\":\"1\""),0),"schema must be a numeric integer");
    require(!r::parse_result(replace(json("ready"),"\"installed_bytes\":8123456789","\"installed_bytes\":\"8123456789\""),0),"byte count must be numeric");
    require(!r::parse_result(replace(json("ready"),"\"installed_bytes\":8123456789","\"installed_bytes\":08123456789"),0),"leading zeroes are invalid JSON numbers");
    require(!r::parse_result(replace(json("ready"),"\"installed_bytes\":8123456789","\"installed_bytes\":-1"),0),"negative byte counts are rejected");
    require(!r::parse_result("{\"schema\":1,\"schema\":1}",0),"duplicate JSON fields rejected");
    require(!r::parse_result(json("ready")+"trailing",0),"trailing invalid JSON rejected");
    Fixture fixture;auto& app=fixture.state;
    const auto root_menu=CreatePopupMenu();const auto rife_menu=append_rife_menu(root_menu,app);
    require(rife_menu && GetMenuItemCount(rife_menu)==3,"RIFE submenu contains exactly Check, Reinstall and Pre-compile");
    require(GetMenuItemID(rife_menu,0)==check_rife && GetMenuItemID(rife_menu,1)==reinstall_rife &&
        GetMenuItemID(rife_menu,2)==precompile_rife,"RIFE actions retain their dedicated command IDs");
    DestroyMenu(root_menu);

    app.settings.rife_runtime_dir="D:/developer override must not affect Check RIFE";
    app.executable_directory=app.local_directory/L"Package O'Brian $()";
    std::filesystem::create_directories(app.executable_directory/L"tools");
    // Owned synthetic helper: no Python/CUDA download, live runtime or profile.
    std::ofstream(app.executable_directory/L"tools/Setup-RifeRuntime.ps1")<<R"PS(param([switch]$Check,[string]$Root,[string]$ResultJson)
Start-Sleep -Milliseconds 900
$r=[ordered]@{schema=1;status='missing';message='fixture health result';root=$Root;backend='nvidia';installed_bytes=0;required_free_bytes=15032385536;log_path="$ResultJson.log"}
$text=$r | ConvertTo-Json -Compress
[IO.File]::WriteAllText($ResultJson,$text,[Text.UTF8Encoding]::new($false))
exit 10
)PS";
    std::wstring error;
    require(begin_rife_operation(app,false,&error),"actual asynchronous hidden PowerShell check starts");
    require(app.rife.process&&WaitForSingleObject(app.rife.process,0)==WAIT_TIMEOUT,"check returns while own helper is still running");
    require(!begin_rife_operation(app,false,&error),"second concurrent check does not spawn another helper");
    const auto before=GetTickCount64();require(!collect_rife_completion(app),"pending check does not block the message loop");
    require(GetTickCount64()-before<250,"pending poll is a zero-timeout observation");
    HWND ping=CreateWindowExW(0,L"STATIC",L"before",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(ping!=nullptr,"test message-only window");SendMessageW(ping,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"responsive"));
    wchar_t text[32]{};GetWindowTextW(ping,text,32);DestroyWindow(ping);require(std::wstring(text)==L"responsive","window messages progress during helper");
    std::optional<RifeCompletion> completion;const auto deadline=GetTickCount64()+15000;
    while(!completion&&GetTickCount64()<deadline){completion=collect_rife_completion(app);if(!completion)Sleep(10);}
    require(completion&&completion->result&&completion->result->status==r::Status::missing&&!app.rife.process,
        "actual helper result is collected once, without starting installation");
    require(completion->result->root==xrfg::managed_rife_runtime(),"Check RIFE uses AppData even with a manual development override");
    require(std::filesystem::is_regular_file(completion->fallback_log),"hidden check keeps console diagnostics");
    require(!collect_rife_completion(app),"completed process cannot be delivered twice");
    require(begin_rife_operation(app,false,&error),"second owned check for timeout test");
    HANDLE owned{};require(DuplicateHandle(GetCurrentProcess(),app.rife.process,GetCurrentProcess(),&owned,SYNCHRONIZE,FALSE,0)!=FALSE,"preserve own fixture retirement handle");
    app.rife.started=GetTickCount64()-r::operation_timeout(false)-1;
    completion=collect_rife_completion(app);
    require(completion&&!completion->result&&completion->error.find(L"timed out")!=std::wstring::npos&&!app.rife.process,
        "timeout reports error without automatic reinstall");
    require(WaitForSingleObject(owned,15000)==WAIT_OBJECT_0,"own timed-out fixture retires before temporary directory cleanup");CloseHandle(owned);
    std::ofstream(app.executable_directory/L"tools/Setup-RifeRuntime.ps1")<<R"PS(param([switch]$Check,[switch]$UpdateOverlay,[string]$Root,[string]$ResultJson)
$r=[ordered]@{schema=1;status='ready';message='code-only fixture';root=$Root;backend='overlay';installed_bytes=0;required_free_bytes=0;log_path=''}
if(-not $UpdateOverlay){exit 1}
[IO.File]::WriteAllText($ResultJson,($r|ConvertTo-Json -Compress),[Text.UTF8Encoding]::new($false))
exit 0
)PS";
    require(begin_rife_operation(app,false,&error,false,false,true),"startup overlay helper starts without installation or GPU check");
    completion.reset();const auto overlay_deadline=GetTickCount64()+15000;
    while(!completion&&GetTickCount64()<overlay_deadline){completion=collect_rife_completion(app);if(!completion)Sleep(10);}
    require(completion&&completion->updating_overlay&&completion->result&&completion->result->status==r::Status::ready,
        "startup overlay-only mode reaches the helper and returns its own completion kind");
    std::cout<<"Check RIFE: controls, routing, quoting, async health and startup code alignment PASS\n";
}
