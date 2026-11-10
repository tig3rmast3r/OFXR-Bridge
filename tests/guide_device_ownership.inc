class UnprovedSameAdapterAlias final:public ReportedDeviceAlias {
public:
    using ReportedDeviceAlias::ReportedDeviceAlias;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID,UINT*,void*) override {return DXGI_ERROR_NOT_FOUND;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID,UINT,const void*) override {return S_OK;}
};

void test_guide_device_ownership(D3D12WarpFixture& fixture) {
    auto motion=create_source_texture(fixture),depth=create_source_texture(fixture);
    xrfg::DlssMotionVectorFrame frame;
    frame.motion_vectors=motion;frame.depth=depth;
    frame.motion_snapshot_device=frame.depth_snapshot_device=fixture.device();
    ComPtr<ReportedDeviceAlias> alias;alias.Attach(new ReportedDeviceAlias(fixture.device()));
    for(auto* resource:{motion.Get(),depth.Get()}) {
        require(xrfg::guide_resource_on_device(fixture.device(),frame,resource,false),"canonical guide owner refused");
        require(!xrfg::guide_resource_on_device(alias.Get(),frame,resource,false),"generic consumer admitted a Luke alias");
        require(xrfg::guide_resource_on_device(alias.Get(),frame,resource,true),"Luke consumer refused a proven snapshot alias");
    }
    auto no_receipt=frame;no_receipt.motion_snapshot_device.Reset();
    require(!xrfg::guide_resource_on_device(alias.Get(),no_receipt,motion.Get(),true),"Luke alias admitted without allocation receipt");
    ComPtr<UnprovedSameAdapterAlias> unrelated;unrelated.Attach(new UnprovedSameAdapterAlias(fixture.device()));
    require(!xrfg::guide_resource_on_device(unrelated.Get(),frame,motion.Get(),true),"same adapter admitted without shared backing");
    auto forged=frame;forged.motion_snapshot_device=unrelated;
    require(!xrfg::guide_resource_on_device(alias.Get(),forged,motion.Get(),true),"unproved allocation receipt admitted");
    auto bound=frame;
    bound.motion_allocation=std::make_shared<const xrfg::DlssSnapshotAllocation>(motion.Get(),fixture.device());
    require(xrfg::guide_resource_on_device(alias.Get(),bound,motion.Get(),true),"bound allocation receipt refused");
    auto wrong_resource=bound;wrong_resource.motion_vectors=depth;
    require(!xrfg::guide_resource_on_device(fixture.device(),wrong_resource,depth.Get(),true),"allocation receipt accepted a replacement resource");
    auto wrong_owner=bound;
    wrong_owner.motion_allocation=std::make_shared<const xrfg::DlssSnapshotAllocation>(motion.Get(),unrelated.Get());
    require(!xrfg::guide_resource_on_device(fixture.device(),wrong_owner,motion.Get(),true),"bound allocation accepted an unproved owner");
    std::cout<<"Guide device ownership: native/alias motion and depth, generic/missing/forged/unproved refusals PASS\n";
}
