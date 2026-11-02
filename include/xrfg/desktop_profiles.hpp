#pragma once
#include "autonomous_control.hpp"
#include <optional>
#include <vector>

namespace xrfg::autonomous {
struct ActiveGameProfile {
    std::filesystem::path file;
    std::uint32_t process_id{};
};
// nullopt means discovery was temporarily busy, not a game disconnect.
std::optional<ActiveGameProfile> active_game_profile() noexcept;
std::vector<std::filesystem::path> saved_game_profiles() noexcept;
bool same_profile_file(const std::filesystem::path&,const std::filesystem::path&);
bool is_saved_game_profile(const std::filesystem::path&) noexcept;

// Only this desktop UI operation is routed to the chosen profile. The tray's
// hotkeys/discovery remain automatic; a bound game's profile always takes priority.
class ScopedConfigurationSelection {
    const std::filesystem::path* previous_{};
public:
    ScopedConfigurationSelection() noexcept; // automatic routing for tray/reentrant messages
    explicit ScopedConfigurationSelection(const std::filesystem::path&) noexcept;
    ~ScopedConfigurationSelection();
    ScopedConfigurationSelection(const ScopedConfigurationSelection&)=delete;
    ScopedConfigurationSelection& operator=(const ScopedConfigurationSelection&)=delete;
    void select(const std::filesystem::path&) noexcept;
};

class DesktopProfiles {
    std::filesystem::path selected_{global_configuration_path()};
    ActiveGameProfile observed_;
    bool manual_{};
    std::vector<std::filesystem::path> saved_;
public:
    const std::filesystem::path& selected() const noexcept {return selected_;}
    const auto& saved() const noexcept {return saved_;}
    void refresh() {saved_=saved_game_profiles();}
    bool choose(const std::filesystem::path& file) {
        if(!same_profile_file(file,global_configuration_path()) && !is_saved_game_profile(file))return false;
        selected_=file;manual_=true;return true;
    }
    bool observe(const std::optional<ActiveGameProfile>& active) {
        if(!active)return false;
        const bool started=active->process_id &&
            (active->process_id!=observed_.process_id || !same_profile_file(active->file,observed_.file));
        observed_=*active;
        if(started)manual_=false;
        const auto next=manual_?selected_:active->process_id?active->file:global_configuration_path();
        if(same_profile_file(next,selected_))return false;
        selected_=next;return true;
    }
};
} // namespace xrfg::autonomous
