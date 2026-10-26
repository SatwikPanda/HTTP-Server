#pragma once

#include "core/config.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace test {

struct SuiteInventoryEntry {
    std::string_view target_name;
    std::string_view track;
    std::string_view milestone_label;
    bool implemented_in_current_tree;
};

// Explicit runtime test inventory mapping milestones/tracks to CTest targets.
inline constexpr std::array<SuiteInventoryEntry, 30> kRuntimeSuiteInventory{{
    {"socket_ownership_test", "Base", "milestone01", true},
    {"milestone01_test", "Base", "milestone01", true},
    {"echo_test_double_test", "Base", "milestone02", true},
    {"echo_integration_test", "Base", "milestone02", true},
    {"milestone02_test", "Base", "milestone02", true},
    {"milestone03_test", "Base", "milestone03", true},
    {"cli_validation_test", "Base", "milestone03", true},
    {"milestone04_test", "Base", "milestone04", true},
    {"milestone05_test", "Base", "milestone05", true},
    {"http_serialization_test", "Base", "milestone05", true},
    {"http_session_test", "Base", "milestone05", true},
    {"http_integration_test", "Base", "milestone05", true},
    {"milestone06_test", "Base", "milestone06", true},
    {"http_parser_test", "Base", "milestone06", true},
    {"milestone07_test", "Base", "milestone07", true},
    {"milestone08_test", "Base", "milestone08", true},
    {"http_response_test", "Base", "milestone08", true},
    {"milestone09_test", "Base", "milestone09", true},
    {"parallel_foundation_test", "P0", "parallel_foundation", true},
    {"parallel_work_test", "P0", "parallel_foundation", true},
    {"path_policy_test", "A", "milestone10", true},
    {"static_target_test", "A", "milestone10", true},
    {"mime_test", "A", "milestone11", true},
    {"static_response_test", "A", "milestone11", true},
    {"conditional_test", "A", "milestone12", true},
    {"config_parser_test", "H", "milestone29", true},
    {"metrics_and_logging_test", "H", "milestone29", true},
    {"stop_hooks_test", "H", "milestone28;milestone29", true},
    {"fixture_harness_test", "I", "parallel_foundation;milestone30", true},
    {"system_matrix_test", "I", "milestone30", true}
}};

struct ProductMatrixEntry {
    core::ApplicationMode application;
    core::DriverMode driver;
    bool cache_enabled;
    std::string_view required_tracks;
    bool ready_in_current_tree;
};

// The 6 required product combinations (plus reference single-driver static server).
inline constexpr std::array<ProductMatrixEntry, 7> kProductMatrixInventory{{
    {core::ApplicationMode::static_files, core::DriverMode::single, false, "P0 + A", true},
    {core::ApplicationMode::static_files, core::DriverMode::thread_pool, false, "P0 + A + E + F + L", false},
    {core::ApplicationMode::static_files, core::DriverMode::event_loop, false, "P0 + A + E + G + L", false},
    {core::ApplicationMode::proxy, core::DriverMode::thread_pool, false, "P0 + B + E + F + L", false},
    {core::ApplicationMode::proxy, core::DriverMode::event_loop, false, "P0 + B + E + G + L", false},
    {core::ApplicationMode::proxy, core::DriverMode::thread_pool, true, "P0 + B + C + D + K + E + F + L", false},
    {core::ApplicationMode::proxy, core::DriverMode::event_loop, true, "P0 + B + C + D + K + E + G + L", false}
}};

} // namespace test
