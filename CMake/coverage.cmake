#call add_converage(module_name) to add coverage targets for the given module
function(add_converage module)
    if("${CMAKE_C_COMPILER_ID}" MATCHES "(Apple)?[Cc]lang" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "(Apple)?[Cc]lang")
        message("[Coverage] Building with llvm Code Coverage Tools")
        # Using llvm gcov ; llvm install by xcode
        set(LLVM_COV_PATH /Library/Developer/CommandLineTools/usr/bin)
        if(NOT EXISTS ${LLVM_COV_PATH}/llvm-cov)
            message(FATAL_ERROR "llvm-cov not found! Aborting.")
        endif()

        # set Flags
        target_compile_options(${module} PRIVATE -fprofile-instr-generate -fcoverage-mapping)
        target_link_options(${module} PRIVATE -fprofile-instr-generate -fcoverage-mapping)

        # llvm-cov
        add_custom_target(${module}-ccov-preprocessing
            COMMAND LLVM_PROFILE_FILE=${module}.profraw $<TARGET_FILE:${module}>
            COMMAND ${LLVM_COV_PATH}/llvm-profdata merge -sparse ${module}.profraw -o ${module}.profdata
            DEPENDS ${module})

        add_custom_target(${module}-ccov-show
            COMMAND ${LLVM_COV_PATH}/llvm-cov show $<TARGET_FILE:${module}> -instr-profile=${module}.profdata -show-line-counts-or-regions
            DEPENDS ${module}-ccov-preprocessing)

        # add summary for CI parse
        add_custom_target(${module}-ccov-report
            COMMAND ${LLVM_COV_PATH}/llvm-cov report $<TARGET_FILE:${module}> -instr-profile=${module}.profdata -ignore-filename-regex=".*_makefiles|.*unittests" -show-region-summary=false
            DEPENDS ${module}-ccov-preprocessing)

        # exclude libs and unittests self
        add_custom_target(${module}-ccov
            COMMAND ${LLVM_COV_PATH}/llvm-cov show $<TARGET_FILE:${module}> -instr-profile=${module}.profdata -show-line-counts-or-regions -output-dir=${module}-llvm-cov -format="html" -ignore-filename-regex=".*_makefiles|.*unittests" > /dev/null 2>&1
            DEPENDS ${module}-ccov-preprocessing)

        add_custom_command(TARGET ${module}-ccov POST_BUILD
            COMMENT "Open ${module}-llvm-cov/index.html in your browser to view the coverage report."
        )
    else()
        # gcc WIP
        message(FATAL_ERROR "Complier not support yet")
    endif()
endfunction()