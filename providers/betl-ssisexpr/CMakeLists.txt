# betl-ssisexpr — SSIS Expression Language engine plugin.
#
# A self-contained C engine: no runtime dependencies beyond libc and the
# host's exported symbols. Built as a MODULE library, dlopen'd into the
# host at registry load like the other providers.

add_library(betl-ssisexpr MODULE ssisexpr_provider.c)
target_include_directories(betl-ssisexpr PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
set_target_properties(betl-ssisexpr PROPERTIES
    PREFIX ""                       # produces betl-ssisexpr.so
    C_VISIBILITY_PRESET hidden
)
target_link_libraries(betl-ssisexpr PRIVATE m)

set(BETL_SSISEXPR_FOUND  TRUE              PARENT_SCOPE)
set(BETL_SSISEXPR_TARGET betl-ssisexpr     PARENT_SCOPE)
