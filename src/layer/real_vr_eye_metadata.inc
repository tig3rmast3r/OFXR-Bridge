// The REAL VR compositor carries a render ID next to each queued eye texture.
// Observe the ID only where that texture feeds a single-source eye draw.
namespace real_vr_eye_metadata {
#if defined(_WIN64)
namespace eye_layout = xrfg::native_ingress::real_vr_eye_layout;

struct DrawLayout {
    std::uint32_t original, endpoint, endpoint_probe, color_release, owner, swapchains;
};

constexpr DrawLayout draw_layout(eye_layout::Abi abi) noexcept {
    return abi == eye_layout::Abi::v26_3
        ? DrawLayout{0xaaf, 0x97a, 0x96c, 0xb90, 0x14e8, 0x5f0}
        : DrawLayout{0xd7a, 0xb42, 0xb42, 0xdb9, 0x15b0, 0x5f8};
}

bool read(std::uintptr_t address, void* out, std::size_t size) noexcept {
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &copied) &&
        copied == size;
}

template<class T>
bool read(std::uintptr_t address, T& value) noexcept {
    return read(address, &value, sizeof(value));
}

template<class... Byte>
bool instructions(std::uintptr_t address, Byte... bytes) noexcept {
    const std::array<unsigned char, sizeof...(bytes)> expected{static_cast<unsigned char>(bytes)...};
    std::array<unsigned char, sizeof...(bytes)> actual{};
    return read(address, actual) && actual == expected;
}

bool match_draw(std::uintptr_t draw, const DrawLayout& offsets, eye_layout::Abi abi) noexcept {
    if (abi == eye_layout::Abi::v26_3) {
        return instructions(draw, 0x89,0x54,0x24,0x10,0x55,0x53,0x56,0x57,0x41,0x57,0x48,0x8d,0xac,0x24,0xc0,0xad,0xff,0xff) &&
            instructions(draw + 0x1f, 0x48,0x8b,0xb1,0xe8,0x14,0,0,0x48,0x8b,0xf9,0x4c,0x63,0xfa) &&
            instructions(draw + offsets.original - 15, 0x49,0x8b,0x44,0x1e,0x58,0x48,0x89,0x44,0x24,0x20) &&
            instructions(draw + offsets.original, 0x49,0x8b,0x44,0x1e,0x10,0x4a,0x89,0x84,0xfe,0x98,0x14,0,0) &&
            instructions(draw + offsets.endpoint_probe, 0x4a,0x8b,0x04,0x20,0x48,0x89,0x44,0x24,0x20) &&
            instructions(draw + offsets.endpoint, 0x49,0x3b,0xdd,0xb8,0x30,0,0,0,0xb9,0x38,0,0,0,0x0f,0x4d,0xc1,0x4a,0x8b,0x04,0x20) &&
            instructions(draw + offsets.color_release, 0x4a,0x8b,0x8c,0xff,0xf0,0x05,0,0,0x48,0x8b,0x01,0xff,0x50,0x28);
    }
    return instructions(draw, 0x89,0x54,0x24,0x10,0x55,0x53,0x56,0x57,0x41,0x54,0x48,0x8d,0xac,0x24,0xf0,0xa9,0xff,0xff) &&
        instructions(draw + 0x1f, 0x48,0x8b,0xb1,0xb0,0x15,0,0,0x48,0x8b,0xf9,0x4c,0x63,0xe2) &&
        instructions(draw + offsets.original, 0x49,0x8b,0x44,0x1e,0x10,0x4a,0x89,0x84,0xe6,0x98,0x14,0,0) &&
        instructions(draw + offsets.endpoint, 0x49,0x3b,0xdd,0xb8,0x30,0,0,0,0xb9,0x38,0,0,0,0x0f,0x4c,0xc1,0x4a,0x8b,0x04,0x30) &&
        instructions(draw + offsets.color_release, 0x4a,0x8b,0x8c,0xe7,0xf8,0x05,0,0,0x48,0x8b,0x01,0xff,0x50,0x28);
}

const eye_layout::Layout* match_module(std::uintptr_t base) noexcept {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 4096 - static_cast<LONG>(sizeof(nt)) ||
        !read(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    const auto* layout = eye_layout::find(nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage);
    if (!layout) return nullptr;
    const auto draw = base + layout->draw_eye;
    const auto offsets = draw_layout(layout->abi);
    std::uintptr_t release{};
    std::int32_t original_call{}, endpoint_call{};
    if (!read(base + layout->swapchain_vtable + 5 * sizeof(release), release) ||
        release != base + layout->release ||
        !match_draw(draw, offsets, layout->abi) ||
        !instructions(draw + offsets.original - 5, 0xe8) ||
        !instructions(draw + offsets.endpoint - 5, 0xe8) ||
        !read(draw + offsets.original - 4, original_call) ||
        !read(draw + offsets.endpoint - 4, endpoint_call) ||
        std::int64_t(offsets.original) + original_call != std::int64_t(offsets.endpoint) + endpoint_call ||
        !instructions(base + layout->acquire_index, 0x4c,0x8d,0x41,0x20,0x48,0x8b,0x49,0x10,0x33,0xd2))
        return nullptr;
    return layout;
}

struct Hooks {
    using DrawEye = void (*)(std::uintptr_t, std::int32_t);
    std::mutex install_mutex;
    std::atomic_bool installed{};
    std::array<HMODULE, 3> attempted{};
    std::uintptr_t table{};
    eye_layout::Abi abi = eye_layout::Abi::current;
    std::atomic<DrawEye> original{};
    SafetyHookInline draw;
    SafetyHookMid direct;
    SafetyHookMid endpoint;
};

Hooks& hooks() {
    // REAL VR's compositor thread can outlive an XR session.
    static auto* state = new Hooks;
    return *state;
}

struct DrawScope {
    std::uintptr_t renderer;
    std::int32_t eye;
    XrSwapchain chain{};
    std::optional<OfxrEyeFrameMetadataV1> frame;
};

thread_local DrawScope* active_draw{};

void draw(std::uintptr_t renderer, std::int32_t eye) {
    DrawScope scope{renderer, eye, {}, {}};
    struct Restore {
        DrawScope* previous;
        ~Restore() { active_draw = previous; }
    } restore{std::exchange(active_draw, &scope)};
    hooks().original.load(std::memory_order_acquire)(renderer, eye);
}

void observe(const SafetyHookContext& ctx, std::uintptr_t render_address, std::uintptr_t texture_address) noexcept {
    auto* scope = active_draw;
    const auto& state = hooks();
    const auto eye = state.abi == eye_layout::Abi::v26_3 ? ctx.r15 : ctx.r12;
    if (!scope || scope->renderer != ctx.rdi || scope->eye < 0 || scope->eye > 1 ||
        eye != static_cast<std::uintptr_t>(scope->eye)) return;

    scope->frame.reset();
    const auto offsets = draw_layout(state.abi);
    std::uintptr_t owner{}, texture{}, chain_object{}, table{};
    std::uint64_t render{};
    std::uint32_t index{};
    XrSwapchain chain{};
    if (!read(ctx.rdi + offsets.owner, owner) || !owner || owner != ctx.rsi ||
        !read(texture_address, texture) || !texture ||
        !read(render_address, render) || render >= (1ULL << 63) ||
        !read(ctx.rdi + offsets.swapchains + eye * sizeof(chain_object), chain_object) || !chain_object ||
        !read(chain_object, table) || table != state.table ||
        !read(chain_object + 0x10, chain) || chain == XR_NULL_HANDLE ||
        !read(chain_object + 0x20, index)) return;

    scope->chain = chain;
    scope->frame = OfxrEyeFrameMetadataV1{sizeof(OfxrEyeFrameMetadataV1),
        OFXR_EYE_SOURCE_REAL_VR, index, 1, {{owner, render, 0, static_cast<std::uint32_t>(scope->eye)}}};
}

void observe_original(SafetyHookContext& ctx) noexcept {
    // The same ring record supplies the colour texture at +0x58 and the
    // render ID that the compositor stores as its last drawn eye at +0x10.
    const auto source = ctx.r14 + ctx.rbx;
    observe(ctx, source + 0x10, source + 0x58);
}

void observe_endpoint(SafetyHookContext& ctx) noexcept {
    // This branch selects one of two retained textures. Use the same signed
    // comparison as REAL VR's render ID load.
    const bool v26_3 = hooks().abi == eye_layout::Abi::v26_3;
    const bool below = static_cast<std::int64_t>(ctx.rbx) < static_cast<std::int64_t>(ctx.r13);
    const bool choose_other = v26_3 ? !below : below;
    const std::uintptr_t texture_offset = choose_other ? 8 : 0;
    // 26.3 reaches this probe just before loading the actual colour texture.
    if (v26_3 && ctx.rax != texture_offset) return;
    const auto source = (v26_3 ? ctx.r12 : ctx.r14) + texture_offset;
    observe(ctx, source + 0x30, source);
}

std::optional<OfxrEyeFrameMetadataV1> take(XrSwapchain chain) noexcept {
    if (!active_draw || active_draw->chain != chain) return {};
    return std::exchange(active_draw->frame, std::nullopt);
}

void install() noexcept {
    try {
        auto& state = hooks();
        if (state.installed.load(std::memory_order_acquire)) return;
        std::scoped_lock lock(state.install_mutex);
        if (state.installed.load(std::memory_order_relaxed)) return;
        constexpr const wchar_t* names[]{L"dxgi.dll", L"dxgi2.dll", L"RealVR64.dll"};
        for (std::size_t i = 0; i < std::size(names); ++i) {
            const auto module = GetModuleHandleW(names[i]);
            if (!module || module == state.attempted[i] || !GetProcAddress(module, "RealVRVersion") ||
                !GetProcAddress(module, "g_rvrShared")) continue;
            state.attempted[i] = module;
            const auto base = reinterpret_cast<std::uintptr_t>(module);
            const auto layout = match_module(base);
            if (!layout) continue;
            const auto draw_address = base + layout->draw_eye;
            const auto offsets = draw_layout(layout->abi);
            auto entry = safetyhook::create_inline(reinterpret_cast<void*>(draw_address),
                reinterpret_cast<void*>(&draw), SafetyHookInline::StartDisabled);
            auto direct = safetyhook::create_mid(reinterpret_cast<void*>(draw_address + offsets.original),
                observe_original, SafetyHookMid::StartDisabled);
            auto endpoint = safetyhook::create_mid(reinterpret_cast<void*>(draw_address + offsets.endpoint_probe),
                observe_endpoint, SafetyHookMid::StartDisabled);
            HMODULE retained{};
            if (!entry || !direct || !endpoint ||
                !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(base), &retained)) continue;
            state.table = base + layout->swapchain_vtable;
            state.abi = layout->abi;
            state.original.store(entry.original<Hooks::DrawEye>(), std::memory_order_release);
            if (!direct.enable() || !endpoint.enable() || !entry.enable()) continue;
            state.draw = std::move(entry);
            state.direct = std::move(direct);
            state.endpoint = std::move(endpoint);
            state.installed.store(true, std::memory_order_release);
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::eye_frame_metadata,
                40, layout->timestamp, layout->draw_eye);
            return;
        }
    } catch (...) {}
}
#else
void install() noexcept {}
std::optional<OfxrEyeFrameMetadataV1> take(XrSwapchain) noexcept { return {}; }
#endif
}
