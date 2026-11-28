// Single-stage direct AER 4X stereo assembler (completion-worker owned).
// Stage 1 generates 0.25, 0.50 and 0.75 directly from consecutive real frames
// of each eye (L0->L2 and R1->R3) using the full route (Hybrid guides/depth or
// optical flow). This assembler pairs exact quarter-step eye handles without a
// second optical-flow pass or generation-on-generation compounding.
struct IndependentAerDirect4x {
    struct EyeOutput {
        XrSwapchain application_swapchain{XR_NULL_HANDLE};
        XrSwapchain output_swapchain{XR_NULL_HANDLE};
        bool synthetic_camera{};
        XrPosef pose{};
        XrFovf fov{};
        std::shared_ptr<void> independent_lease;
        std::shared_ptr<DirectSyntheticLease> direct_lease;
        std::shared_ptr<DirectOutputPublication> direct_publication;
        std::shared_ptr<xrfg::D3D12NativeFrameResult> native_result;
        std::optional<DjulesPendingOutput> pending_output;
        bool valid() const noexcept { return output_swapchain != XR_NULL_HANDLE; }
        void clear() noexcept { *this = {}; }
    };

    // Paired delivery (aer_fresh_mask == 3): newer eye's C (real) and S2 (0.75).
    EyeOutput paired_newer_real;
    EyeOutput paired_newer_q3;
    // Alternating delivery (aer_fresh_mask == 1 or 2): last fresh eye's S2 (0.75).
    std::array<EyeOutput, 2> alternating_q3;
    std::uint64_t paired_frames{};
    std::uint64_t generated_midpoints{};
    XrSpace space{XR_NULL_HANDLE};
    std::uint64_t configuration{UINT64_MAX};
    unsigned last_delivery{};

    void reset() noexcept {
        paired_newer_real.clear();
        paired_newer_q3.clear();
        alternating_q3[0].clear();
        alternating_q3[1].clear();
        space = XR_NULL_HANDLE;
        last_delivery = 0;
    }

    static bool same_eye_chain(const EyeOutput& a, XrSwapchain app_swapchain) noexcept {
        return a.valid() && a.application_swapchain == app_swapchain;
    }

    static std::shared_ptr<GeneratedFrameEndInfo> build_frame(
        const SynthesisJob& job, std::size_t scene,
        const EyeOutput& left, const EyeOutput& right,
        bool is_synthetic, bool is_aer_mixed, bool is_aer_generated_pair,
        float panel_fraction) {
        if (!left.valid() || !right.valid() || !job.metadata) return {};
        std::vector<ProjectionResourceDestination> destinations;
        destinations.push_back({left.application_swapchain, left.output_swapchain, left.synthetic_camera, false});
        if (right.application_swapchain != left.application_swapchain) {
            destinations.push_back({right.application_swapchain, right.output_swapchain, right.synthetic_camera, false});
        }
        GeneratedFrameEndInfo info;
        if (!build_generated_frame_end_info(&job.metadata->info, job.snapshot,
                is_synthetic, destinations, &info, nullptr, panel_fraction)) {
            return {};
        }
        if (scene >= info.projections.size() || info.projections[scene].views.size() != 2) {
            return {};
        }
        info.projections[scene].views[0].subImage.swapchain = left.output_swapchain;
        info.projections[scene].views[0].pose = left.pose;
        info.projections[scene].views[0].fov = left.fov;
        info.projections[scene].views[1].subImage.swapchain = right.output_swapchain;
        info.projections[scene].views[1].pose = right.pose;
        info.projections[scene].views[1].fov = right.fov;
        const auto attach = [&](const EyeOutput& eye) {
            if (eye.independent_lease) info.independent_output_leases.push_back(eye.independent_lease);
            if (eye.direct_lease) info.direct_synthetic_leases.push_back(eye.direct_lease);
            if (eye.direct_publication) info.direct_publications.push_back(eye.direct_publication);
            if (eye.native_result) info.native_results.push_back(eye.native_result);
            if (eye.pending_output) info.pending_outputs.push_back(*eye.pending_output);
        };
        attach(left);
        attach(right);
        info.composition_images = job.metadata->composition_images;
        info.synthetic = is_synthetic;
        info.blackbar_pose.paired = false;
        if (is_aer_mixed) {
            info.interface_stamp.mark_aer_mixed();
        } else if (is_aer_generated_pair) {
            info.interface_stamp.aer_mixed = false;
            info.interface_stamp.aer_generated_pair = true;
        }
        return make_presenter_owned_frame(std::move(info));
    }

    bool process(const std::shared_ptr<SessionState>& state, const SynthesisJob& job,
        const PreparedProjectionFrame& prepared,
        std::vector<std::shared_ptr<GeneratedFrameEndInfo>>& outputs) {
        outputs.clear();
        const auto scene = ofr_scene_projection(job.snapshot, job.mappings);
        if (!state || !scene || job.mappings.empty() || job.mappings.size() > 2 ||
            *scene >= job.snapshot.layers.size() ||
            job.snapshot.layers[*scene].views.size() != 2) {
            reset();
            return false;
        }
        const auto epoch = state->djules_configuration_epoch.load();
        const auto current_space = job.snapshot.layers[*scene].space;
        if (configuration != epoch || (space != XR_NULL_HANDLE && space != current_space)) {
            reset();
            configuration = epoch;
        }
        space = current_space;

        std::array<EyeOutput, 2> real_eye{}, s0_eye{}, s1_eye{}, s2_eye{};
        unsigned found_mask{};
        for (const auto& resource : prepared.resources) {
            if (resource.panel) continue;
            const auto mapped = std::find_if(job.mappings.begin(), job.mappings.end(),
                [&](const auto& m) { return m.application_swapchain == resource.application_swapchain; });
            if (mapped == job.mappings.end()) continue;
            const auto& g = resource.generation;
            for (const auto& view_ref : mapped->views) {
                if (view_ref.projection_index != *scene || view_ref.view_index > 1) continue;
                const unsigned eye = static_cast<unsigned>(view_ref.view_index);
                const auto& base_view = job.snapshot.layers[*scene].views[eye];
                auto view_from = [&](const std::optional<ProjectionSnapshot>& cam) -> const XrCompositionLayerProjectionView& {
                    if (cam && *scene < cam->layers.size() && eye < cam->layers[*scene].views.size()) {
                        return cam->layers[*scene].views[eye];
                    }
                    return base_view;
                };
                const auto fill = [&](EyeOutput& dst, XrSwapchain handle, bool synth_cam,
                    const XrCompositionLayerProjectionView& view,
                    const std::shared_ptr<void>& ind_lease,
                    const std::shared_ptr<DirectSyntheticLease>& dir_lease,
                    const std::shared_ptr<DirectOutputPublication>& dir_pub,
                    const std::shared_ptr<xrfg::D3D12NativeFrameResult>& native_res) {
                    dst.application_swapchain = resource.application_swapchain;
                    dst.output_swapchain = handle;
                    dst.synthetic_camera = synth_cam;
                    dst.pose = view.pose;
                    dst.fov = view.fov;
                    dst.independent_lease = ind_lease;
                    dst.direct_lease = dir_lease;
                    dst.direct_publication = dir_pub;
                    dst.native_result = native_res;
                };
                fill(real_eye[eye], g.current_handle, false, base_view,
                    g.independent_current_lease, nullptr, g.direct_current_publication, g.native_result);
                if (job.pair && g.synthetic_handle != XR_NULL_HANDLE) {
                    fill(s0_eye[eye], g.synthetic_handle, g.synthetics_in_target_camera,
                        view_from(job.synthetic_camera), g.independent_synthetic_lease,
                        g.direct_synthetic_lease, g.direct_synthetic_publication, g.native_result);
                }
                if (job.pair && g.extra_handle != XR_NULL_HANDLE) {
                    fill(s1_eye[eye], g.extra_handle, g.synthetics_in_target_camera,
                        view_from(job.extra_camera), g.independent_extra_lease,
                        g.direct_extra_lease, g.direct_extra_publication, g.native_result);
                }
                if (job.pair && g.third_handle != XR_NULL_HANDLE) {
                    fill(s2_eye[eye], g.third_handle, g.synthetics_in_target_camera,
                        view_from(job.third_camera), g.independent_third_lease,
                        g.direct_third_lease, g.direct_third_publication, g.native_result);
                }
                found_mask |= 1U << eye;
            }
        }
        if (found_mask != 3U || !real_eye[0].valid() || !real_eye[1].valid()) {
            reset();
            return false;
        }

        const unsigned newer = xrfg::ofr::newer_eye(state->nvidia_options.ofr_swap);
        const unsigned older = 1U - newer;
        const unsigned fresh_mask = job.snapshot.aer_fresh_mask;

        // Prime or non-pair fallback: emit the real frame and retain the newer
        // eye's real output so the very first paired frame can assemble t=1.0.
        if (!job.pair || prepared.kind != PreparedGenerationKind::pair ||
            !s0_eye[0].valid() || !s0_eye[1].valid() ||
            !s1_eye[0].valid() || !s1_eye[1].valid() ||
            !s2_eye[0].valid() || !s2_eye[1].valid() || fresh_mask == 0) {
            reset();
            space = current_space;
            auto prime = build_frame(job, *scene, real_eye[0], real_eye[1], false, false, false, .5F);
            if (!prime) return false;
            if (fresh_mask == 3) {
                paired_newer_real = real_eye[newer];
                last_delivery = 2;
            }
            outputs.push_back(std::move(prime));
            xrfg::embedded::independent_aer_double_state = 1;
            return true;
        }

        if (fresh_mask == 3) {
            // Paired delivery: raw A=(older@0, newer@1), B=(older@2, newer@3).
            if (last_delivery && last_delivery != 2) {
                reset();
                space = current_space;
            }
            last_delivery = 2;
            const auto pair_eyes = [&](const EyeOutput& older_out, const EyeOutput& newer_out,
                bool is_synth, bool mixed, bool gen_pair, float panel_f) {
                return older == 0
                    ? build_frame(job, *scene, older_out, newer_out, is_synth, mixed, gen_pair, panel_f)
                    : build_frame(job, *scene, newer_out, older_out, is_synth, mixed, gen_pair, panel_f);
            };
            // Frame 0 (t=0.5): older@0.25 + retained newer@0.75 from previous pair.
            if (same_eye_chain(paired_newer_q3, real_eye[newer].application_swapchain)) {
                if (auto f0 = pair_eyes(s0_eye[older], paired_newer_q3, true, false, true, .25F)) {
                    outputs.push_back(std::move(f0));
                    ++generated_midpoints;
                }
            }
            // Frame 1 (t=1.0): older@0.50 + retained newer real from previous frame.
            if (same_eye_chain(paired_newer_real, real_eye[newer].application_swapchain)) {
                if (auto f1 = pair_eyes(s1_eye[older], paired_newer_real, true, true, false, .50F)) {
                    outputs.push_back(std::move(f1));
                }
            }
            // Frame 2 (t=1.5): older@0.75 + newer@0.25 (both from this pair).
            if (auto f2 = pair_eyes(s2_eye[older], s0_eye[newer], true, false, true, .75F)) {
                outputs.push_back(std::move(f2));
                ++generated_midpoints;
            }
            // Frame 3 (t=2.0): older real + newer@0.50.
            if (auto f3 = pair_eyes(real_eye[older], s1_eye[newer], false, true, false, 1.0F)) {
                outputs.push_back(std::move(f3));
            }
            // Retain newer@0.75 and newer real for the next paired source.
            paired_newer_q3 = std::move(s2_eye[newer]);
            paired_newer_real = std::move(real_eye[newer]);
            alternating_q3[0].clear();
            alternating_q3[1].clear();
            ++paired_frames;
        } else {
            // Alternating delivery: fresh_eye advanced (0->2), held_eye stayed at 1.
            if (last_delivery && last_delivery != 1) {
                reset();
                space = current_space;
            }
            last_delivery = 1;
            const unsigned fresh_eye = (fresh_mask == 1) ? 0U : 1U;
            const unsigned held_eye = 1U - fresh_eye;
            const auto pair_eyes = [&](const EyeOutput& fresh_out, const EyeOutput& held_out,
                bool is_synth, bool mixed, bool gen_pair, float panel_f) {
                return fresh_eye == 0
                    ? build_frame(job, *scene, fresh_out, held_out, is_synth, mixed, gen_pair, panel_f)
                    : build_frame(job, *scene, held_out, fresh_out, is_synth, mixed, gen_pair, panel_f);
            };
            // Frame 0 (t=0.5): fresh@0.25 + retained held@0.75 from previous alternating frame.
            if (same_eye_chain(alternating_q3[held_eye], real_eye[held_eye].application_swapchain)) {
                if (auto f0 = pair_eyes(s0_eye[fresh_eye], alternating_q3[held_eye], true, false, true, .25F)) {
                    outputs.push_back(std::move(f0));
                    ++generated_midpoints;
                }
            }
            // Frame 1 (t=1.0): fresh@0.50 + held real.
            if (auto f1 = pair_eyes(s1_eye[fresh_eye], real_eye[held_eye], true, true, false, .50F)) {
                outputs.push_back(std::move(f1));
            }
            // Retain fresh@0.75 for the next alternating frame.
            alternating_q3[fresh_eye] = std::move(s2_eye[fresh_eye]);
            alternating_q3[held_eye].clear();
            paired_newer_real.clear();
            paired_newer_q3.clear();
            ++paired_frames;
        }
        xrfg::embedded::independent_aer_double_state = generated_midpoints > 0 ? 2U : 1U;
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
            110742, paired_frames, generated_midpoints, outputs.size());
        return !outputs.empty();
    }
};
