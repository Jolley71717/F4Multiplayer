-- include subprojects
includes("lib/commonlibf4")

-- set project constants
set_project("F4Multiplayer")
set_version("0.7.1")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

add_requires("enet")

-- keep the build machine's paths (and so the user name) out of the binaries: __FILE__ loses the
-- project directory, and the .pdb is referred to by name only
add_cxxflags("/d1trimfile:" .. os.projectdir():gsub("/", "\\") .. "\\", { force = true })
add_ldflags("/PDBALTPATH:%_PDB%", { force = true })
add_shflags("/PDBALTPATH:%_PDB%", { force = true })

-- static analysis: xmake f --analyze=y runs the MSVC analyser on our targets
option("analyze")
    set_default(false)
    set_showmenu(true)
    set_description("Run the MSVC code analyser (/analyze) on our own sources")
option_end()

-- only our own sources are analysed and reported; <...> headers (lib/, enet, the SDK) are external
local function analyze()
    if has_config("analyze") then
        add_cxxflags("/analyze", "/analyze:external-", "/external:anglebrackets", "/external:W0", { force = true })
    end
end

-- protocol, networking helpers and the relay server, shared by every target
target("F4MPCommon")
    set_kind("static")
    add_files("common/**.cpp")
    add_headerfiles("common/**.h")
    add_includedirs("common", { public = true })
    analyze()
    add_packages("enet", { public = true })
    add_syslinks("ws2_32", "winmm", { public = true })

-- client plugin, loaded into the game by F4SE
target("F4Multiplayer")
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

-- unit tests for the protocol and the server (no game needed): xmake build F4MPTests && xmake run F4MPTests
target("F4MPTests")
    set_kind("binary")
    set_default(false)
    add_deps("F4MPCommon")
    analyze()
    add_files("tests/**.cpp")
    add_includedirs("src")  -- for headers with no game dependency, e.g. game/ShotLoops.h
    add_headerfiles("tests/**.h")
