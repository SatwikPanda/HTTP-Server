# Track G (event-loop) tests.
function(add_event_loop_test name milestone)
    add_project_test(${name}
        SOURCES ${name}.cpp
        LINKS event_loop server http concurrency net core Threads::Threads
        LABELS "event_loop;milestone${milestone}"
        TIMEOUT 30
    )
endfunction()

add_event_loop_test(milestone25_test 25)
add_event_loop_test(milestone26_test 26)
add_event_loop_test(milestone27_test 27)
add_event_loop_test(milestone28_test 28)
