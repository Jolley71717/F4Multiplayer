-- include subprojects. Only the plugin needs CommonLibF4 (and MSVC); on Linux (.oss-scanner/Dockerfile)
-- the game-free targets below are built with gcc and the plugin is skipped.
if is_plat("windows") then
    includes("lib/commonlibf4")
end

-- the plugin built for Windows on a Mac (tools/xwin.lua)
if is_host("macosx") then
    includes("tools/xwin.lua")
end

-- set project constants
set_project("F4Multiplayer")
set_version("0.10.1")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")
if not is_plat("windows") then
    -- gcc's -Wextra warns on every aggregate init that leaves fields to their defaults, which the code
    -- does on purpose everywhere (MSVC, the plugin's compiler, says nothing)
    add_cxxflags("-Wno-missing-field-initializers")
end

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

add_requires("enet v1.3.18")  -- pinned: a new enet release must not slip into a release build unnoticed

-- keep the build machine's paths (and so the user name) out of the binaries: __FILE__ loses the
-- project directory, and the .pdb is referred to by name only
if is_plat("windows") then
    add_cxxflags("/d1trimfile:" .. os.projectdir():gsub("/", "\\") .. "\\", { force = true })
    add_ldflags("/PDBALTPATH:%_PDB%", { force = true })
    add_shflags("/PDBALTPATH:%_PDB%", { force = true })
end

-- static analysis: xmake f --analyze=y runs the MSVC analyser on our targets
option("analyze")
    set_default(false)
    set_showmenu(true)
    set_description("Run the MSVC code analyser (/analyze) on our own sources")
option_end()

-- only our own sources are analysed and reported; <...> headers (lib/, enet, the SDK) are external
local function analyze()
    if has_config("analyze") and is_plat("windows") then
        add_cxxflags("/analyze", "/analyze:external-", "/external:anglebrackets", "/external:W0", { force = true })
    end
end

-- profiling, developer builds only: xmake f --tracy=<a Tracy source folder> builds the plugin with Tracy's client
-- (the folder's version must be the profiler's). It listens on this machine only and sends only while a capture is
-- connected; tools/package.ps1 always configures without it, and refuses a DLL holding it.
option("tracy")
    set_default("")
    set_showmenu(true)
    set_description("Tracy source folder, to build the plugin with Tracy's client (profiling; never a release)")
option_end()

local tracy = get_config("tracy")
if tracy == "" then
    tracy = nil
end

-- Tracy's client: no sampling, system tracing, call stacks or crash handler (the crash logger keeps those), and
-- started by the plugin's load rather than in DllMain
if tracy and is_plat("windows") then
    target("TracyClient")
        set_kind("static")
        set_warnings("none")
        add_files(path.join(tracy, "public", "TracyClient.cpp"))
        add_includedirs(path.join(tracy, "public"), { public = true })
        add_defines("TRACY_ENABLE", "TRACY_ON_DEMAND", "TRACY_ONLY_LOCALHOST", "TRACY_NO_BROADCAST", "TRACY_NO_SAMPLING",
            "TRACY_NO_SYSTEM_TRACING", "TRACY_NO_CONTEXT_SWITCH", "TRACY_NO_CALLSTACK", "TRACY_NO_CRASH_HANDLER",
            "TRACY_NO_CODE_TRANSFER", "TRACY_NO_FRAME_IMAGE", "TRACY_NO_VSYNC_CAPTURE", "TRACY_DELAYED_INIT",
            "TRACY_MANUAL_LIFETIME", { public = true })
        add_cxxflags("/d1trimfile:" .. path.absolute(tracy):gsub("/", "\\") .. "\\", { force = true })
        add_syslinks("ws2_32", "dbghelp", "advapi32", "user32", { public = true })
    target_end()
end

-- protocol, networking helpers and the relay server, shared by every target
target("F4MPCommon")
    set_kind("static")
    add_files("common/**.cpp")
    add_headerfiles("common/**.h")
    add_includedirs("common", { public = true })
    analyze()
    add_packages("enet", { public = true })
    if is_plat("windows") then
        add_syslinks("ws2_32", "winmm", { public = true })
    end

-- client plugin, loaded into the game by F4SE (Windows and MSVC only: it needs CommonLibF4 and the game)
if is_plat("windows") then
target("F4Multiplayer")
	add_defines('F4MP_BUILD_LABEL="' .. (os.getenv("F4MP_LABEL") or "") .. '"')  -- tools/package.ps1 sets it to the label and commit ("rc3 1a2b3c4"), shown in the log
    add_rules("commonlibf4.plugin", {
        name = "F4Multiplayer",
        author = "Jolley71717",
        description = "Multiplayer for Fallout 4"
    })

    add_deps("F4MPCommon")
    analyze()

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
    add_syslinks("ws2_32", "bcrypt", "shell32", "ole32", "user32")
    if tracy then
        add_deps("TracyClient")
        add_defines("F4MP_TRACY")
    end
end

-- standalone dedicated server
target("F4MPServer")
    set_kind("binary")
    add_deps("F4MPCommon")
    analyze()
    add_files("server/**.cpp")

-- fake player for testing without a second copy of the game
target("F4MPBot")
    set_kind("binary")
    add_deps("F4MPCommon")
    analyze()
    add_files("bot/**.cpp")

-- unit tests for the protocol, the server and the game-free client logic (no game needed). Built by a
-- plain `xmake build`, so whatever runs the tests after a build never runs an old binary.
target("F4MPTests")
    set_kind("binary")
    add_deps("F4MPCommon")
    analyze()
    add_files("tests/**.cpp")
    add_includedirs("src")  -- for headers with no game dependency, e.g. game/ShotLoops.h
    add_headerfiles("tests/**.h")
