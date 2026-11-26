// MSFS overlaps Wait(N+1) with the rendered frame N. The high-resolution
// r17 trace began every one of 449 tail frames behind two unfinished jobs.
// Keep one old job plus the current render, including before End queues it.
static int independent_pipelined_admission_test() {
    xrfg_fixture::ControlProfile profile;
    auto settings=xrfg::autonomous::snapshot().desired;
    settings.enabled=true;settings.frame_generation=0;settings.independent_pacing=true;
    check(xrfg::autonomous::request(settings),"pipelined admission settings");
    xrfg::autonomous::apply_runtime_settings();
    independent_fixture_mode=true;
    {
        Fixture f(true);f.throttled=false;f.unblock();
        for(unsigned factor:{2U,3U,4U}) {
            auto dispatch=std::make_shared<Dispatch>();dispatch->wait_frame=Fixture::wait;dispatch->begin_frame=Fixture::begin;
            auto state=std::make_shared<SessionState>(dispatch);
            state->handle=reinterpret_cast<XrSession>(2);state->private_runtime=f.transport;
            state->independent_pacing=state->menu_enabled=state->presenter_active=true;
            state->graphics_binding=SessionGraphicsBinding::d3d12;
            state->nvidia_options.rife_multiplier=factor;
            {std::scoped_lock lock(g_state_mutex);g_sessions[state->handle]=state;}
            XrFrameState seed{XR_TYPE_FRAME_STATE};
            xr_ok(f.transport->predict_frame(&seed),"seed native clock before admission probe");
            {
                std::scoped_lock lock(state->presenter_mutex);
                state->independent_begun_credit.acquire_locked(state);
                state->independent_completion_busy=true;
            }
            state->application_frame_in_progress=true;
            auto waiting=std::async(std::launch::async,[&] {
                XrFrameState next{XR_TYPE_FRAME_STATE};
                return layer_wait_frame_impl(state->handle,nullptr,&next);
            });
            check(waiting.wait_for(20ms)==std::future_status::timeout,
                "one older job plus begun frame must not admit a third source");
            // This is End's actual frame-call lock and atomic job handoff.
            // Old Wait held the lock here until GPU completion, ageing N.
            auto ending=std::async(std::launch::async,[&] {
                std::scoped_lock frame_lock(state->frame_call_mutex);
                auto credit=std::move(state->independent_begun_credit);
                state->application_frame_in_progress=false;
                std::scoped_lock lock(state->presenter_mutex);
                state->independent_sources.push_back(std::make_shared<SynthesisJob>());
                credit.release_locked(*state,1);
            });
            check(ending.wait_for(100ms)==std::future_status::ready,
                "full Wait admission must leave preceding End free to hand off");
            ending.get();
            check(waiting.wait_for(5ms)==std::future_status::timeout,
                "transferring a begun credit to a job must not open admission");
            {
                std::scoped_lock lock(state->presenter_mutex);
                state->independent_completion_busy=false;
            }
            state->presenter_condition.notify_all();
            check(waiting.wait_for(1000ms)==std::future_status::ready,"retiring the older job admits next Wait");
            xr_ok(waiting.get(),"next Wait succeeds after genuine capacity retires");
            check(state->independent_source_reservations==1 && state->independent_sources.size()==1,
                "exactly one old job and one reserved Wait ticket");
            xr_ok(layer_begin_frame_impl(state->handle,nullptr),"Begin transfers Wait credit");
            check(state->independent_source_reservations==1,"Begin neither drops nor duplicates admission credit");
            state->independent_sources.clear();
            reset_frame_bookkeeping(state);
            check(state->independent_source_reservations==0,"VR reset returns begun admission credit");
            {std::scoped_lock lock(g_state_mutex);g_sessions.erase(state->handle);}
            state->private_runtime.reset(); // Fixture owns the endpoint reused by the next multiplier.
        }
    }
    independent_fixture_mode=false;
    std::puts("PIPELINED_ADMISSION 2X/3X/4X: rendered End progresses during full Wait; two-source bound survives handoff PASS");
    return 0;
}

static int independent_source_credit_lifecycle_test() {
    xrfg_fixture::ControlProfile profile;
    auto settings=xrfg::autonomous::snapshot().desired;settings.enabled=true;
    check(xrfg::autonomous::request(settings),"credit lifecycle enabled settings");
    xrfg::autonomous::apply_runtime_settings();
    auto dispatch=std::make_shared<Dispatch>();dispatch->begin_frame=Fixture::begin;
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
    state->independent_pacing=state->menu_enabled=true;
    {std::scoped_lock lock(g_state_mutex);g_sessions[state->handle]=state;}
    {
        IndependentSourceCredit credit;
        xr_ok(wait_for_independent_source_budget(*state,3,&credit,state),"bootstrap reserves first source without a worker");
        check(state->independent_source_reservations==1,"bootstrap credit exists");
    }
    check(state->independent_source_reservations==0,"failed Wait scope returns credit");
    state->presenter_active=true;
    {
        IndependentSourceCredit credit;
        xr_ok(wait_for_independent_source_budget(*state,3,&credit,state),"first Wait reserves source");
        state->independent_wait_credit=std::move(credit);
        state->application_wait_pending_begin=true;
    }
    XrFrameBeginInfo invalid{XR_TYPE_FRAME_WAIT_INFO};
    check(layer_begin_frame_impl(state->handle,&invalid)==XR_ERROR_VALIDATION_FAILURE,
        "failed Begin must refuse invalid ticket consumption");
    check(state->independent_source_reservations==0 && !state->application_wait_pending_begin,
        "failed Begin returns its Wait credit");
    // Begin discarding an older begun frame returns the old credit once.
    {
        std::scoped_lock lock(state->presenter_mutex);
        state->independent_wait_credit.acquire_locked(state);
        state->independent_begun_credit.acquire_locked(state);
    }
    state->application_wait_pending_begin=true;
    xr_ok(layer_begin_frame_impl(state->handle,nullptr),"replacement Begin");
    check(state->independent_source_reservations==1,"discarded begun frame returns only its own credit");
    {
        auto ending=std::move(state->independent_begun_credit);
        check(state->independent_source_reservations==1,"End retains credit before successful job handoff");
    }
    check(state->independent_source_reservations==0,"empty or failed End returns credit");
    // Keep one old GPU job overlapping one new source, as before this fix.
    state->independent_completion_busy=true;
    {
        IndependentSourceCredit credit;
        xr_ok(wait_for_independent_source_budget(*state,3,&credit,state),"single-thread render overlaps one GPU job");
        check(state->independent_source_reservations==1,"overlap credit reserved");
    }
    state->independent_completion_busy=false;
    // FG-off still tracks its ticket so an on transition cannot overlook it.
    state->menu_enabled=false;
    {
        IndependentSourceCredit credit;
        xr_ok(wait_for_independent_source_budget(*state,3,&credit,state),"FG-off ticket remains restart-safe");
        state->independent_wait_credit=std::move(credit);
    }
    reset_frame_bookkeeping(state);
    check(state->independent_source_reservations==0,"reset returns unbegun FG-off ticket");
    // A live credit uses a weak session owner, so it cannot retain a session.
    IndependentSourceCredit orphan;
    {std::scoped_lock lock(state->presenter_mutex);orphan.acquire_locked(state);}
    {std::scoped_lock lock(g_state_mutex);g_sessions.erase(state->handle);}
    std::weak_ptr<SessionState> weak=state;state.reset();
    check(weak.expired(),"admission credit must not retain the session");orphan.reset();
    std::puts("SOURCE_CREDIT bootstrap, failed Wait/Begin, discarded Begin, failed End, overlap, FG-off, VR reset and session lifetime PASS");
    return 0;
}
