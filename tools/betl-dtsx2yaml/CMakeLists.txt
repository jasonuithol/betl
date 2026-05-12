# betl-dtsx2yaml — SSIS DTSX → betl YAML converter.
#
# A small framework-dependent .NET console app. Unlike the
# betl-dotnet runtime shim this is *not* NativeAOT — it only runs at
# conversion time on a developer machine, where a regular `dotnet publish`
# keeps deploy size sane and (later) lets us pull in Roslyn-based
# VB.NET → C# translation without dragging it into the runtime cost.
#
# Build is gated on deps/dotnet/dotnet being present (same as the
# betl-dotnet provider). When the SDK isn't installed we skip the
# build but keep the parent configure step happy.

set(DTSX2YAML_DOTNET "${CMAKE_SOURCE_DIR}/deps/dotnet/dotnet")
if(NOT EXISTS "${DTSX2YAML_DOTNET}")
    message(STATUS "betl-dtsx2yaml: .NET SDK not found at "
        "${DTSX2YAML_DOTNET} — converter will not be built. "
        "Run scripts/install-dotnet.sh to enable.")
    set(BETL_DTSX2YAML_FOUND FALSE PARENT_SCOPE)
    return()
endif()

set(DTSX2YAML_PROJ "${CMAKE_CURRENT_SOURCE_DIR}/Betl.Dtsx2Yaml.csproj")
set(DTSX2YAML_OUT  "${CMAKE_CURRENT_BINARY_DIR}/out")
set(DTSX2YAML_DLL  "${DTSX2YAML_OUT}/Betl.Dtsx2Yaml.dll")

# `dotnet publish` is idempotent and dependency-tracked by .NET itself,
# but CMake doesn't see those source dependencies. We declare every
# .cs file under this directory as input so a touched source triggers
# a rebuild.
file(GLOB_RECURSE DTSX2YAML_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cs"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.csproj"
)

add_custom_command(
    OUTPUT  "${DTSX2YAML_DLL}"
    COMMAND "${DTSX2YAML_DOTNET}"
            publish "${DTSX2YAML_PROJ}"
            -c Release
            --self-contained false
            --nologo
            -v minimal
            -o "${DTSX2YAML_OUT}"
    DEPENDS ${DTSX2YAML_SOURCES}
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Building betl-dtsx2yaml (dotnet publish)"
    VERBATIM
)

add_custom_target(dtsx2yaml ALL DEPENDS "${DTSX2YAML_DLL}")

set(BETL_DTSX2YAML_FOUND  TRUE          PARENT_SCOPE)
set(BETL_DTSX2YAML_DOTNET "${DTSX2YAML_DOTNET}" PARENT_SCOPE)
set(BETL_DTSX2YAML_DLL    "${DTSX2YAML_DLL}"    PARENT_SCOPE)
