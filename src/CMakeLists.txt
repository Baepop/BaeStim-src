cmake_minimum_required(VERSION 3.20)
project(BaeStim VERSION 2.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
# Version string embedded in BaeStim.dll. Empty (the normal case) embeds
# PROJECT_VERSION on EVERY configure. Do not cache the derived value: a cached
# copy used to stick after a release bump, so a 2.0.2 build kept reporting 2.0.1
# and the updater would download the same version forever.
#
# For preview/test builds that must not look like a release:
#   cmake -B build -DBAESTIM_VERSION_OVERRIDE=2.0.2-test
set(BAESTIM_VERSION_OVERRIDE "" CACHE STRING
    "Version string embedded into BaeStim.dll (empty = project version)")
if(BAESTIM_VERSION_OVERRIDE)
    set(BAESTIM_VERSION "${BAESTIM_VERSION_OVERRIDE}")
else()
    set(BAESTIM_VERSION "${PROJECT_VERSION}")
endif()

# Allow CMAKE_MSVC_RUNTIME_LIBRARY to control runtime selection for all targets,
# including dependencies pulled in via FetchContent.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

# Static MSVC runtime everywhere, so the resulting DLL has no extra runtime
# dependencies. Must be set BEFORE FetchContent_MakeAvailable so the fetched
# deps (Lua, Detours, spdlog) inherit it.
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)


if(NOT WIN32)
    message(STATUS "BaeStim: no targets on non-Windows platforms; skipping.")
    return()
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BaeStimBuildInfo.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/BaeStimBuildInfo.h"
    @ONLY
)

# ---------------------------------------------------------------------------
# Dependency recipes (FetchContent-backed, cached at <repo>/.deps).
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(BAESTIM_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools")
include(Lua)
include(Spdlog)
include(Protobuf)
include(Tomlplusplus)
include(LogMacros)

# Build the host code generators (ipc_codegen) in their own binary subtree so
# their executables never land in the shipped output directory. EXCLUDE_FROM_ALL
# means only the tools actually depended on (ipc_codegen) get built here.
add_subdirectory("${BAESTIM_TOOLS_DIR}" "${CMAKE_BINARY_DIR}/tools" EXCLUDE_FROM_ALL)
include("${BAESTIM_TOOLS_DIR}/cmake/IPCCodegen.cmake")

set(IPC_IDL "${CMAKE_CURRENT_SOURCE_DIR}/Steam/IPCMessages.steamd")
# Per-config output dir, mirroring the protobuf recipe below: a Multi-Config
# build invokes the generator once per config, and a single shared output file
# would ping-pong between the Release/Debug graphs and force needless rebuilds.
baestim_add_ipc_codegen(IPC_GEN
    IDL "${IPC_IDL}"
    CPP_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>"
)

# ---------------------------------------------------------------------------
# Protobuf code generation — one variant per config, from the same .proto:
#
#   Debug   → full Message     (protoc --cpp_out)       → links libprotobuf
#   Release → lite MessageLite (protoc --cpp_out=lite:) → links libprotobuf-lite
#
# Output lands in the per-config generated/ dir (same root as the IPC header),
# so each config owns its own files. A shared output path would ping-pong
# between the Release/Debug graphs (the command embeds the config-specific
# protoc path) and force a regen + recompile on every config switch.
# ---------------------------------------------------------------------------
set(PROTO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/proto/steam_messages.proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>")

add_custom_command(
    OUTPUT "${PROTO_GEN_DIR}/steam_messages.pb.cc"
           "${PROTO_GEN_DIR}/steam_messages.pb.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTO_GEN_DIR}"
    COMMAND $<TARGET_FILE:protoc>
        "--cpp_out=$<$<CONFIG:Release>:lite:>${PROTO_GEN_DIR}"
        "-I${CMAKE_CURRENT_SOURCE_DIR}/proto"
        "${PROTO_SRC}"
    DEPENDS "${PROTO_SRC}" protoc
    COMMENT "Generating protobuf C++ sources"
    VERBATIM
)

# ---------------------------------------------------------------------------
# OSTPlatform — internal static library that owns the OS-specific backends.
# Its build is defined in its own module CMakeLists so this top-level file stays
# focused on host-level products (the DLLs) and code generation.
# ---------------------------------------------------------------------------
add_subdirectory(OSTPlatform)

# ---------------------------------------------------------------------------
# Windows host artifacts: the injected hook DLL and the two loader-hijack DLLs.
# All Windows-only by construction (PE DLL hijacking + Win32 hooking).
# ---------------------------------------------------------------------------

# BaeStim — the hook DLL injected into Steam (always 64-bit).
add_library(BaeStim SHARED
    dllmain.cpp

    # Shared utilities
    Utils/Tickets/AppTicket.cpp
    Utils/Tickets/LegacyCDKey.cpp
    Utils/Tickets/EticketClient.cpp
    Utils/Config/Config.cpp
    Utils/Config/ConfigFileWatcher.cpp
    Utils/Config/LuaConfig.cpp
    Utils/Config/LuaFileWatcher.cpp
    Utils/CloudRedirect/CloudRedirectHost.cpp
    Utils/Achievements/LocalAchievements.cpp
    Utils/HookSupport/VehCommon.cpp
    Utils/Logging/Log.cpp
    Utils/SteamMetadata/IPCLoader.cpp
    Utils/SteamMetadata/ManifestClient.cpp
    Utils/SteamMetadata/ManifestCache.cpp
    Utils/SteamMetadata/ManifestDonor.cpp
    Utils/SteamMetadata/Mirror.cpp
    Utils/SteamMetadata/PatternFallback.cpp
    Utils/SteamMetadata/PatternLoader.cpp
    Utils/SteamMetadata/RemoteToml.cpp
    Utils/Json/MiniJson.cpp
    Utils/SteamIdentity/SteamIdentity.cpp
    Utils/Remote/RemoteConfig.cpp
    Utils/SteamMetadata/StatsClient.cpp
    Utils/SteamMetadata/SteamDiagnostics.cpp
    Utils/Tokeer/TokeerBridge.cpp
    Utils/Tokeer/TokeerUri.cpp

    # Pipe subsystem: generic handshake registry + ProcessInspector core, plus
    # self-contained features that own their primitives.
    Pipe/PipeManager.cpp
    Pipe/ProcessInspector.cpp
    Pipe/Features/DenuvoAuth/DenuvoAuth.cpp
    Pipe/Features/DenuvoAuth/ProtectionScan.cpp
    Pipe/Features/Injection/Injection.cpp

    # Generated IPC message structs (depends on IPCMessages.steamd via add_custom_command)
    "${IPC_GEN}"

    # Per-category hook modules
    Hook/HookManager.cpp
    Hook/Hooks_CallBack.cpp
    Hook/Hooks_Decryption.cpp
    Hook/Hooks_IPC.cpp
    Hook/Hooks_IPC_ISteamUser.cpp
    Hook/Hooks_IPC_ISteamUtils.cpp
    Hook/Hooks_KeyValues.cpp
    Hook/Hooks_Manifest.cpp
    Hook/Hooks_Misc.cpp
    Hook/Hooks_NetPacket.cpp
    Hook/PendingAPICalls.cpp
    Hook/Hooks_SteamUI.cpp
    Hook/Hooks_Package.cpp

    # protobuf generated sources (full in Debug, lite in Release)
    "${PROTO_GEN_DIR}/steam_messages.pb.cc"
)

# Header search path — per-config include directory
target_include_directories(BaeStim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>
)

target_link_libraries(BaeStim PRIVATE
    OSTPlatform
    lua_static
    $<$<CONFIG:Debug>:libprotobuf>
    $<$<CONFIG:Release>:libprotobuf-lite>
    tomlplusplus::tomlplusplus
    $<$<CONFIG:Debug>:spdlog::spdlog>
    Shell32
)

# Logging is compiled in only for Debug; Release reduces LOG_* to no-ops.
target_compile_definitions(BaeStim PRIVATE
    $<$<CONFIG:Debug>:BAESTIM_LOGGING_ENABLED>
)

# Backend endpoint for on-demand eticket minting (strict Denuvo titles that bind
# their encrypted app ticket to a launch nonce). Empty by default: the feature is
# off and the DLL never makes a network request. Point your own build at your own
# backend and the resulting DLL is self-contained, no Lua config needed:
#
#   cmake -B build -DOST_ETICKET_URL="https://your-host/eticket"
#
# Left out of the source deliberately so no single deployment's backend ships
# baked into a public tree. seteticketurl() in the Lua config overrides it.
set(OST_ETICKET_URL "" CACHE STRING
    "Backend URL for on-demand eticket minting (empty disables the feature)")
if(OST_ETICKET_URL)
    target_compile_definitions(BaeStim PRIVATE
        OST_ETICKET_URL="${OST_ETICKET_URL}")
endif()

# Self-update. ON by default. This used to default to OFF because the update
# source was the upstream GitHub mirror, which once replaced a private build
# (observed live: a 1.0.0 build prompted to "update" to upstream v1.0.9). That
# reason no longer applies: the only source now is this project's own public
# Supabase "updates" bucket, where only the owner can write, so an upstream
# release can no longer overwrite a build made here.
#
# The updater checks for a newer DLL on a detached thread at startup and stages
# it for the next launch. With it compiled in, the runtime switch is
# [update] enabled = false in baestim.toml.
#
# Building with -DOST_ENABLE_UPDATER=OFF is still supported for builds that
# must not make a single update request: the unit is not compiled into the
# binary at all, so the code is unreachable and no update traffic is possible.
#
#   cmake -B build -DOST_ENABLE_UPDATER=OFF
option(OST_ENABLE_UPDATER "Compile in the self-updater" ON)
if(OST_ENABLE_UPDATER)
    target_compile_definitions(BaeStim PRIVATE OST_ENABLE_UPDATER)
    target_sources(BaeStim PRIVATE Utils/Update/AppUpdater.cpp)
endif()

# Base URL of the Tokeer code server used by the bst:// bridge (code redemption).
# Overridable per build; defaults to the public host when unset.
#   cmake -B build -DOST_TOKEER_URL="https://luastools.xyz"
set(OST_TOKEER_URL "" CACHE STRING
    "Base URL for the Tokeer code server (bst:// bridge)")
if(OST_TOKEER_URL)
    target_compile_definitions(BaeStim PRIVATE
        OST_TOKEER_URL="${OST_TOKEER_URL}")
endif()

# ---------------------------------------------------------------------------
# dwmapi.dll hijack — small loader DLL placed alongside Steam.
# ---------------------------------------------------------------------------
add_library(dwmapi SHARED
    dwmapi/dwmapi.cpp
)

# ---------------------------------------------------------------------------
# xinput1_4.dll hijack — secondary loader DLL placed alongside Steam.
# ---------------------------------------------------------------------------
add_library(xinput1_4 SHARED
    xinput1_4/xinput1_4.cpp
    xinput1_4/xinput1_4.def
)

# ---------------------------------------------------------------------------
# Unit tests — plain-stdlib fixture tests, no framework. Targets are built
# explicitly (cmake --build build --target <name>), the DLL is unaffected.
# ---------------------------------------------------------------------------
enable_testing()

add_executable(baestim_minijson_test EXCLUDE_FROM_ALL
    ../tests/MiniJsonTest.cpp
    Utils/Json/MiniJson.cpp
)
target_include_directories(baestim_minijson_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME minijson COMMAND baestim_minijson_test)

# RemoteConfig parser/renderer fixtures (tests/RemoteConfigTest.cpp). The unit's
# dependency surface is small: MiniJson for the parse layer, SteamIdentity for
# the worker's identity poll and OSTPlatform for HTTP/encoding/dynamic library.
# Log.h is header-only in Release; spdlog headers are only pulled in by Debug.
# Config.cpp is listed because the worker reads its [supabase] getters; its own
# [manifest] dependency is satisfied by the inert tests/ManifestClientStub.cpp
# so the real ManifestClient (LuaConfig, AppTicket, Steam client) stays out.
add_executable(baestim_remoteconfig_test EXCLUDE_FROM_ALL
    ../tests/RemoteConfigTest.cpp
    ../tests/ManifestClientStub.cpp
    Utils/Remote/RemoteConfig.cpp
    Utils/Config/Config.cpp
    Utils/Json/MiniJson.cpp
    Utils/SteamIdentity/SteamIdentity.cpp
)
target_include_directories(baestim_remoteconfig_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>
)
target_link_libraries(baestim_remoteconfig_test PRIVATE
    OSTPlatform
    tomlplusplus::tomlplusplus
    $<$<CONFIG:Debug>:spdlog::spdlog>
)
add_test(NAME remoteconfig COMMAND baestim_remoteconfig_test)

# AppUpdater pointer parse/URL fixtures (tests/AppUpdaterTest.cpp). AppUpdater.cpp
# needs Config (the [supabase] URL getter), the generated BaeStimBuildInfo.h
# (BAESTIM_VERSION), OSTPlatform (HTTP/Hash/Process/DynamicLibrary) and toml++.
# Config.cpp's [manifest] dependency is satisfied by the inert
# tests/ManifestClientStub.cpp, same as the remoteconfig target.
add_executable(baestim_appupdater_test EXCLUDE_FROM_ALL
    ../tests/AppUpdaterTest.cpp
    ../tests/ManifestClientStub.cpp
    Utils/Update/AppUpdater.cpp
    Utils/Config/Config.cpp
)
target_include_directories(baestim_appupdater_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>
)
target_link_libraries(baestim_appupdater_test PRIVATE
    OSTPlatform
    tomlplusplus::tomlplusplus
    $<$<CONFIG:Debug>:spdlog::spdlog>
)
add_test(NAME appupdater COMMAND baestim_appupdater_test)
