// Reuse actual D12 resource/fence fixtures; this mode supplies cancellation
// ONLY through the shipping native Reset hook, never through a manual receipt.
#define main snapshot_lifetime_unused_standalone_main
#define XRFG_SNAPSHOT_LIFETIME_FIXTURE_ONLY
#include "dlss_snapshot_lifetime_tests.cpp"
#undef XRFG_SNAPSHOT_LIFETIME_FIXTURE_ONLY
#undef main

void native_reset_lifetime_test() {
    using namespace dlss_snapshot_lifetime_fixture;
    Fixture fixture(false);
    xrfg::native_ingress::configure_queue(fixture.queue.Get());
    const auto no_luke_authority=[&] {
        ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
        require(!xrfg::native_ingress::query_luke_context(&device,&queue),
            "ordinary guide Reset observation granted Luke producer authority");
    };
    no_luke_authority();begin_tracking();
    auto counts=std::make_shared<Counts>();auto recording=fixture.publish(41201,counts);
    xrfg::native_ingress::observe_guide_list(recording.list.Get());
    recording.frame.reset();disable();require(counts->live==3,"original recorded snapshots were not pinned");
    auto replacement=fixture.reset(recording,false);
    require(!counts->live,"actual original Reset hook failed to cancel unsubmitted OFF snapshots");
    ok(recording.list->Close(),"hook-reset original close");no_luke_authority();

    begin_tracking();auto failed_counts=std::make_shared<Counts>();
    auto failed=fixture.publish(41202,failed_counts,true,nullptr,false);failed.frame.reset();disable();fixture.check_debug();
    ComPtr<ID3D12InfoQueue> messages;fixture.device.As(&messages);
    D3D12_MESSAGE_SEVERITY expected[]{D3D12_MESSAGE_SEVERITY_ERROR,D3D12_MESSAGE_SEVERITY_CORRUPTION};
    D3D12_INFO_QUEUE_FILTER filter{};filter.DenyList.NumSeverities=2;filter.DenyList.pSeverityList=expected;
    if(messages)ok(messages->PushStorageFilter(&filter),"expected original failed-Reset scope");
    const auto failed_result=failed.list->Reset(failed.allocator.Get(),nullptr);
    if(messages)messages->PopStorageFilter();
    require(FAILED(failed_result) && failed_counts->live==3,"Reset hook changed failure HRESULT or canceled failed recordings");
    ok(failed.list->Close(),"original failed-Reset still-open list close");
    auto failed_replacement=fixture.reset(failed,false);
    require(!failed_counts->live,"later successful original Reset did not retire snapshots");
    ok(failed.list->Close(),"later hooked reset close");

    begin_tracking();auto submitted_counts=std::make_shared<Counts>();auto submitted=fixture.publish(41203,submitted_counts);
    auto gate=fixture.make_fence();ok(fixture.queue->Wait(gate.Get(),1),"hold original native Execute GPU copies");
    ID3D12CommandList* lists[]{submitted.list.Get()};
    fixture.queue->ExecuteCommandLists(1,lists); // shipping Execute hook is the sole writer reporter
    ReleaseGate release{fixture,fixture.queue,gate};submitted.frame.reset();disable();
    auto submitted_replacement=fixture.reset(submitted,false);
    require(submitted_counts->live==3,"actual Reset hook canceled already-executed GPU copies");
    ok(gate->Signal(1),"release original GPU copies");fixture.wait();xrfg::retire_disabled_dlss_motion_vector_resources();
    require(!submitted_counts->live,"actual native writer receipt did not retire after Reset and completion");
    ok(submitted.list->Close(),"submitted hooked Reset close");no_luke_authority();fixture.check_debug();
    xrfg::native_ingress::stop_queue(fixture.queue.Get());
    std::puts("Native original Reset hook: non-Luke cancellation, exact failure HRESULT, actual Execute/fence preservation and zero OFF owned bytes PASS");
}
