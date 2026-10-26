# Track I system matrix tests and required-suite inventory check.
add_project_test(system_matrix_test
    SOURCES system_matrix_test.cpp
    LINKS application
    LABELS milestone30 system_matrix TIMEOUT 90
)

require_project_tests(system_matrix_test)
add_project_test(application_lifecycle_test SOURCES lifecycle_test.cpp LINKS application
    LABELS milestone28 milestone30 system_matrix TIMEOUT 45)
require_project_tests(application_lifecycle_test)
add_project_test(resource_soak_test SOURCES resource_soak_test.cpp resource_probe.cpp LINKS application
    LABELS milestone24 milestone28 milestone30 correctness_gate TIMEOUT 120)
require_project_tests(resource_soak_test)

# The C++ inventory is also the configure-time source of truth. A compiled
# executable without a registered CTest does not satisfy this contract.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/test_inventory.hpp" inventory_rows
    REGEX "^[ ]*[{]\"[a-z_0-9]+\",")
get_property(registered GLOBAL PROPERTY REGISTERED_RUNTIME_TESTS)
foreach(row IN LISTS inventory_rows)
    string(REGEX MATCH "\"([a-z_0-9]+)\"" match "${row}")
    set(required_test "${CMAKE_MATCH_1}")
    if(NOT required_test IN_LIST registered)
        message(FATAL_ERROR "Required runtime acceptance test is not registered: ${required_test}")
    endif()
endforeach()
