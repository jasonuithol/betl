# betl-dotnet — C# / VB.NET task and script components (v0.2).
#
# The plugin itself is a small C library: no link-time .NET dependency.
# It dlopens user scripts after AOT-compiling them via `dotnet publish`
# at validate time. The C# shim under shim/ is compiled per-user-script
# and lives at runtime as an AOT-produced .so in the user's cache dir.
#
# Build-time prerequisite is therefore only libdl (which the host
# always has). The .NET SDK is a *runtime* prerequisite — fetched by
# scripts/install-dotnet.sh into deps/dotnet/. Builds succeed without
# it; tests that exercise dotnet.task skip when the SDK isn't around.

add_library(betl-dotnet MODULE dotnet_provider.c)
target_include_directories(betl-dotnet PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
set_target_properties(betl-dotnet PROPERTIES
    PREFIX ""                       # produces betl-dotnet.so
    C_VISIBILITY_PRESET hidden
)
target_link_libraries(betl-dotnet PRIVATE ${CMAKE_DL_LIBS})

set(BETL_DOTNET_FOUND  TRUE          PARENT_SCOPE)
set(BETL_DOTNET_TARGET betl-dotnet   PARENT_SCOPE)
