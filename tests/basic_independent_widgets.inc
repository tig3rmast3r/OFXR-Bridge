void basic_independent_widgets() {
    const auto saved=snapshot().desired;
    for(bool compact:{false,true}) {
        auto settings=saved;settings.advanced_mode=true;settings.independent_pacing=false;
        settings.enabled=true;settings.afw_only=false;settings.frame_generation=0;settings.backend=0;
        require(request(settings),"explicit Advanced legacy UI setup");
        Context view({compact?1024.f:690.f,1400},compact);view.draw();view.draw(true);
        require(view.rendered_text.find("Independent pacing")!=std::string::npos &&
            view.rendered_text.find("Requires restart")!=std::string::npos,
            "Advanced Independent selector includes the existing yellow restart badge");
        require(view.rendered_text.find("Framework")!=std::string::npos,"Advanced retains legacy pacing");
        settings=snapshot().desired;settings.advanced_mode=false;settings.independent_pacing=false;
        settings.independent_acquire_fix=false;require(request(settings),"Basic migration from explicit saved legacy route");
        view.draw();view.draw(true);
        require(snapshot().desired.independent_pacing && snapshot().desired.independent_acquire_fix,
            "Basic always selects Independent with corrected acquire despite old INI values");
        for(const char* hidden:{"Independent pacing","Framework","Director","Increase buffer","Fast mode"})
            require(view.rendered_text.find(hidden)==std::string::npos,std::string("Basic hides legacy delivery control: ")+hidden);
        view.select_tab("Logging");view.draw();view.draw(true);
        require(view.rendered_text.find("V1103 acquire fix")==std::string::npos,"retired A/B control is not exposed");
        auto actual=snapshot();actual.connected=true;actual.runtime.pacing_mode=0;
        view.injected_snapshot=&actual;view.select_tab("Frame generation");view.draw();view.draw(true);
        require(view.rendered_text.find("Restart the game to apply")!=std::string::npos,
            "already-running legacy session is not falsely reported as converted live");
        actual.runtime.pacing_mode=kIndependentPacingMode;view.draw(true);
        require(view.rendered_text.find("Restart the game to apply")==std::string::npos,
            "Basic active Independent session has no false restart warning");
        view.injected_snapshot=nullptr;
    }
    require(request(saved),"restore Basic/Advanced policy test settings");
}
