# Track A tests. The root registers this directory only when BUILD_TESTING is on.
function(add_static_test name milestone)
    add_project_test(${name} SOURCES ${name}.cpp
        LINKS server http concurrency net core Threads::Threads LABELS milestone${milestone})
endfunction()

add_static_test(path_policy_test 10)
