-- include subprojects
includes("lib/commonlibf4")

-- set project constants
set_project("F4Multiplayer")
set_version("0.6.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

add_requires("enet")

-- protocol, networking helpers and the relay server, shared by every target
target("F4MPCommon")
    set_kind("static")
    add_files("common/**.cpp")
    add_headerfiles("common/**.h")
    add_includedirs("common", { public = true })
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
    add_files("server/**.cpp")

-- fake player for testing without a second copy of the game
target("F4MPBot")
    set_kind("binary")
    add_deps("F4MPCommon")
    add_files("bot/**.cpp")

-- unit tests for the protocol and the server (no game needed): xmake build F4MPTests && xmake run F4MPTests
target("F4MPTests")
    set_kind("binary")
    set_default(false)
    add_deps("F4MPCommon")
    add_files("tests/**.cpp")
    add_headerfiles("tests/**.h")
