std::function<void(HWND)> keybind_dialog_script;
std::exception_ptr keybind_dialog_error;

void save_keybind_preview(HWND window) {
    wchar_t path[32768]{};if(!GetEnvironmentVariableW(L"OFXR_KEYBINDS_PREVIEW",path,32768))return;
    RECT r{};GetWindowRect(window,&r);const int width=r.right-r.left,height=r.bottom-r.top;
    auto screen=GetWindowDC(window);auto dc=CreateCompatibleDC(screen);
    auto bitmap=CreateCompatibleBitmap(screen,width,height);auto old=SelectObject(dc,bitmap);
    require(PrintWindow(window,dc,2)!=FALSE,"render actual keybind dialog");
    SelectObject(dc,old);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    std::vector<std::uint8_t> pixels(std::size_t(width)*height*4);
    require(GetDIBits(dc,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS)!=0,"read keybind dialog pixels");
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);header.bfSize=header.bfOffBits+DWORD(pixels.size());
    std::ofstream file(std::filesystem::path(path),std::ios::binary);file.write(reinterpret_cast<char*>(&header),sizeof(header));
    file.write(reinterpret_cast<char*>(&info.bmiHeader),sizeof(info.bmiHeader));file.write(reinterpret_cast<char*>(pixels.data()),pixels.size());
    DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,screen);
}
INT_PTR run_keybind_dialog(KeybindDialog& draft,std::function<void(HWND)> script) {
    keybind_dialog_error={};keybind_dialog_script=std::move(script);
    const auto timer=SetTimer(nullptr,0,50,[](HWND,UINT,UINT_PTR timer,DWORD){
        KillTimer(nullptr,timer);const auto dialog=GetActiveWindow();
        try{require(dialog!=nullptr,"own modal dialog exists");keybind_dialog_script(dialog);}
        catch(...){keybind_dialog_error=std::current_exception();EndDialog(dialog,IDCANCEL);}
    });
    const auto result=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_KEYBINDS),nullptr,keybind_dialog_procedure,reinterpret_cast<LPARAM>(&draft));
    KillTimer(nullptr,timer);if(keybind_dialog_error)std::rethrow_exception(keybind_dialog_error);return result;
}
void keybind_ui_tests(AppState& app) {
    namespace a=xrfg::autonomous;
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_HOTKEY_CLASS};InitCommonControlsEx(&controls);
    app.settings_path=a::global_configuration_path();
    require(a::publish_keybinds({}),"publish initial six defaults");
    KeybindDialog draft;auto fields=xrfg::standalone::keybind_settings(app.settings);
    for(unsigned i=0;i<xrfg::kKeybindCount;++i)draft.settings[i]=*fields[i];
    const auto defaults=draft.settings;
    require(defaults==std::array<std::string,xrfg::kKeybindCount>{"f10","f11","f7","f9","f8","f6"},"six requested defaults");
    auto result=run_keybind_dialog(draft,[&](HWND window){
        require(a::keybinds().suspended,"runtime actions suspended during capture");
        save_keybind_preview(window);
        for(unsigned i=0;i<xrfg::kKeybindCount;++i){
            SendMessageW(window,WM_COMMAND,IDC_KEYBIND_CHANGE+i,0);
            const auto box=GetDlgItem(window,IDC_KEYBIND_CAPTURE+i);
            require(IsWindowVisible(box)!=FALSE,"Change starts native capture for this row");
            SendMessageW(box,WM_KEYDOWN,VK_F17+i,0);
            SendMessageW(box,WM_KEYUP,VK_F17+i,0);
            require(draft.settings[i]=="f"+std::to_string(17+i),"actual native key event captures correct row");
            require(draft.capturing==-1,"capture completes after a main key");
        }
        SendMessageW(window,WM_COMMAND,IDC_KEYBIND_CLEAR+4,0);require(draft.settings[4]=="off","Clear disables one binding");
        SendMessageW(window,WM_COMMAND,IDC_KEYBIND_DEFAULTS,0);require(draft.settings==defaults,"Restore defaults resets all six");
        for(unsigned i=0;i<2;++i){
            SendMessageW(window,WM_COMMAND,IDC_KEYBIND_CHANGE+i,0);
            SendMessageW(GetDlgItem(window,IDC_KEYBIND_CAPTURE+i),WM_KEYDOWN,VK_F17,0);
        }
        SendMessageW(window,WM_COMMAND,IDOK,0);
        wchar_t status[512]{};GetDlgItemTextW(window,IDC_KEYBIND_STATUS,status,512);
        require(std::wstring_view(status).find(L"same binding")!=std::wstring_view::npos,"duplicate actions cannot be saved");
        for(unsigned i=0;i<xrfg::kKeybindCount;++i){
            SendMessageW(window,WM_COMMAND,IDC_KEYBIND_CHANGE+i,0);
            const auto box=GetDlgItem(window,IDC_KEYBIND_CAPTURE+i);
            SendMessageW(box,HKM_SETHOTKEY,hotkey_control_value({MOD_CONTROL|MOD_SHIFT,VK_F17+i}),0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_KEYBIND_CAPTURE+i,EN_CHANGE),reinterpret_cast<LPARAM>(box));
            require(draft.settings[i]=="ctrl+shift+f"+std::to_string(17+i),"existing modifier conversion retained");
        }
        SendMessageW(window,WM_COMMAND,IDOK,0);
    });
    require(result==IDOK && !a::keybinds().suspended,"OK ends capture lease");
    {
        a::ScopedConfigurationSelection offline(a::game_configuration_path(L"Offline.exe"));
        require(apply_keybinds(app,draft.settings),"save global keybinds while an offline game is selected");
        const auto active=a::keybinds();
        for(unsigned i=0;i<xrfg::kKeybindCount;++i)require(active.bindings.keys[i]==xrfg::KeybindChord{6,VK_F17+i},"all runtime actions receive the new binding live");
        require(!std::filesystem::exists(a::game_configuration_path(L"Offline.exe")),"keybinds do not write selected game profile");
    }
    std::ifstream saved(app.settings_path);std::string text((std::istreambuf_iterator<char>(saved)),{});
    const auto restored=xrfg::standalone::parse_settings(text);auto copy=restored;const auto restored_fields=xrfg::standalone::keybind_settings(copy);
    for(unsigned i=0;i<xrfg::kKeybindCount;++i)require(*restored_fields[i]==draft.settings[i],"all six bindings survive serialization and restart");
    const auto before=a::keybinds().bindings;
    result=run_keybind_dialog(draft,[&](HWND window){SendMessageW(window,WM_COMMAND,IDC_KEYBIND_DEFAULTS,0);SendMessageW(window,WM_COMMAND,IDCANCEL,0);});
    require(result==IDCANCEL && a::keybinds().bindings==before && !a::keybinds().suspended,"Cancel preserves published bindings and releases lease");
    std::array<bool,256> down{};auto read=[&](unsigned key){return down[key];};
    down[VK_F17]=true;
    require(!xrfg::keybind_matches(before[xrfg::KeybindAction::open_interface],read),"bare key cannot trigger a modified chord");
    down[VK_CONTROL]=down[VK_SHIFT]=true;
    require(xrfg::keybind_matches(before[xrfg::KeybindAction::open_interface],read),"exact configured chord triggers");
    down[VK_MENU]=true;
    require(!xrfg::keybind_matches(before[xrfg::KeybindAction::open_interface],read),"extra modifiers do not also trigger the base chord");
    xrfg::DiagnosticKeyEdge edge;const auto chord=before[xrfg::KeybindAction::dump];
    require(edge.pressed(chord,true),"configured dump gets one press edge");
    require(!edge.pressed(chord,true),"holding configured dump cannot repeat");
    edge.pressed(chord,false);
    require(!edge.pressed(chord,true,true) && !edge.pressed(chord,true),"captured key does not fire when the dialog closes while held");
    edge.pressed(chord,false);
    require(edge.pressed(chord,true),"release and new press rearm configured action");
    require(!edge.pressed({0,VK_F8},true),"changing a binding onto a held key does not fire");
    edge.pressed({0,VK_F8},false);require(edge.pressed({0,VK_F8},true),"remapped key rearms after release");
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    std::wstring command=L"\""+std::wstring(executable)+L"\" keybind-child";STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION child{};
    require(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)!=FALSE,"start independent bound game reader");
    const auto wait=WaitForSingleObject(child.hProcess,10000);DWORD exit=1;GetExitCodeProcess(child.hProcess,&exit);CloseHandle(child.hThread);CloseHandle(child.hProcess);
    require(wait==WAIT_OBJECT_0 && exit==0,"bound game process reads global live keybinds");
    a::publish_keybinds({});
    std::cout<<"Six native capture rows, default/clear/duplicate/OK/Cancel, persistence, global profile isolation, exact modifiers and independent bound game read PASS\n";
}
