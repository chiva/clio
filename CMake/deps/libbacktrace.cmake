target_compile_definitions(clio PUBLIC BOOST_STACKTRACE_LINK)
target_compile_definitions(clio PUBLIC BOOST_STACKTRACE_USE_BACKTRACE)
find_package(libbacktrace REQUIRED)