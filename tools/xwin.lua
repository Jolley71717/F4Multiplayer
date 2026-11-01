-- Builds the plugin for Windows on a Mac: clang-cl and lld-link (LLVM) against the MSVC runtime and the
-- Windows SDK that xwin (https://github.com/Jake-Shadle/xwin) downloads from Microsoft. xmake's own clang-cl
-- toolchain only works on a Windows host, or on Linux with a Visual Studio installed under Wine; this one needs
-- neither. Only tried on macOS. docs/misc/MAC.md says how to set it up. A release is still built with MSVC on
-- Windows (tools/package.ps1).
--
--   xmake f -p windows -a x64 -m releasedbg --toolchain=xwin [--xwin=~/.xwin] [--llvm=<LLVM folder>]
option("xwin")
	set_default("")
	set_showmenu(true)
	set_description("Folder xwin splatted the MSVC runtime and Windows SDK into (default ~/.xwin)")
option_end()

option("llvm")
	set_default("")
	set_showmenu(true)
	set_description("LLVM folder holding bin/clang-cl, if not Homebrew's llvm")
option_end()

-- The folder xwin splatted into.
local function xwin_root()
	local root = get_config("xwin")
	if not root or root == "" then
		root = path.join(os.getenv("HOME") or "~", ".xwin")
	end
	return root
end

-- A tool's full path. Homebrew's llvm isn't on PATH, and lld-link is a formula of its own. Looked up by file:
-- find_program runs a tool to vet it, and llvm-rc answers none of its questions.
local function xwin_tool(a_name)
	local dirs = {}
	local llvm = get_config("llvm")
	if llvm and llvm ~= "" then
		table.insert(dirs, path.join(llvm, "bin"))
	end
	table.join2(dirs, { "/opt/homebrew/opt/llvm/bin", "/opt/homebrew/opt/lld/bin" }, path.splitenv(os.getenv("PATH") or ""))
	for _, dir in ipairs(dirs) do
		if os.isfile(path.join(dir, a_name)) then
			return path.join(dir, a_name)
		end
	end
end

local XWIN_INCLUDES = { "crt/include", "sdk/include/ucrt", "sdk/include/um", "sdk/include/shared" }
local XWIN_LIBS = { "crt/lib/x86_64", "sdk/lib/ucrt/x86_64", "sdk/lib/um/x86_64" }

-- What every compile needs: the game's target (clang-cl's default is the host's, arm64 macOS) and the SDK's headers.
local function xwin_cxflags()
	local flags = { "--target=x86_64-pc-windows-msvc" }
	for _, dir in ipairs(XWIN_INCLUDES) do
		table.insert(flags, "-imsvc" .. path.join(xwin_root(), dir))
	end
	return flags
end

toolchain("xwin")
	set_kind("standalone")
	set_description("clang-cl and lld-link against xwin's MSVC runtime and Windows SDK")
	set_runtimes("MT", "MTd", "MD", "MDd")

	on_check(function(a_toolchain)
		local found = true
		for _, name in ipairs({ "clang-cl", "lld-link", "llvm-ar", "llvm-lib", "llvm-rc" }) do
			if not xwin_tool(name) then
				cprint("${color.error}xwin toolchain: %s not found (brew install llvm lld)", name)
				found = false
			end
		end
		local root = xwin_root()
		if not os.isdir(path.join(root, "sdk", "include", "um")) then
			cprint("${color.error}xwin toolchain: no Windows SDK in %s (xwin --accept-license --arch x86_64 splat --output %s)", root, root)
			found = false
		end
		return found
	end)

	on_load(function(a_toolchain)
		a_toolchain:set("toolset", "cc", xwin_tool("clang-cl"))
		a_toolchain:set("toolset", "cxx", xwin_tool("clang-cl"))
		a_toolchain:set("toolset", "ld", xwin_tool("lld-link"))
		a_toolchain:set("toolset", "sh", xwin_tool("lld-link"))
		a_toolchain:set("toolset", "ar", xwin_tool("llvm-ar"))
		a_toolchain:set("toolset", "mrc", xwin_tool("llvm-rc"))

		for _, flag in ipairs(xwin_cxflags()) do
			a_toolchain:add("cxflags", flag)
		end
		-- CommonLibF4's F4SE/RegistrationMap*.cpp have a bare "return;" in templates that are never instantiated; MSVC
		-- never looks inside them, clang refuses them at parse
		a_toolchain:add("cxxflags", "-Wno-return-mismatch")
		for _, dir in ipairs(XWIN_INCLUDES) do
			a_toolchain:add("mrcflags", "/I" .. path.join(xwin_root(), dir))
		end
		for _, dir in ipairs(XWIN_LIBS) do
			a_toolchain:add("ldflags", "/libpath:" .. path.join(xwin_root(), dir))
			a_toolchain:add("shflags", "/libpath:" .. path.join(xwin_root(), dir))
		end
	end)
toolchain_end()

-- xmake builds Windows packages with CMake only through Visual Studio, so with this toolchain ENet and spdlog are
-- compiled here, straight into static libraries. They come from git, so they follow whatever version xmake.lua and
-- CommonLibF4 ask for. This gives the commands; the package's install runs them (this file's own functions see only
-- the description scope's os, which runs nothing).
local function xwin_static_commands(a_package, a_name, a_sources, a_flags)
	local runtime = a_package:has_runtime("MD", "MDd") and "/MD" or "/MT"
	local commands = {}
	local objects = {}
	for _, source in ipairs(a_sources) do
		local object = path.basename(source) .. ".obj"
		local argv = table.join(xwin_cxflags(), { "/nologo", "/c", "/O2", runtime }, a_flags, { source, "/Fo" .. object })
		table.insert(commands, { xwin_tool("clang-cl"), argv })
		table.insert(objects, object)
	end
	local lib = path.join(a_package:installdir("lib"), a_name .. ".lib")
	table.insert(commands, { xwin_tool("llvm-lib"), table.join({ "/nologo", "/out:" .. lib }, objects) })
	return commands
end

if get_config("toolchain") == "xwin" then
	package("enet")
		set_homepage("http://enet.bespin.org")
		set_description("Reliable UDP networking library (built with clang-cl for the xwin toolchain)")
		set_license("MIT")
		add_urls("https://github.com/lsalzman/enet.git")
		add_syslinks("winmm", "ws2_32")

		on_install(function(a_package)
			for _, command in ipairs(xwin_static_commands(a_package, "enet", os.files("*.c"), { "/Iinclude" })) do
				os.vrunv(command[1], command[2])
			end
			os.cp("include", a_package:installdir())
		end)
	package_end()

	-- Only what CommonLibF4 asks of spdlog (compiled, std::format, wchar_t): any other configuration is refused rather
	-- than built wrong.
	package("spdlog")
		set_homepage("https://github.com/gabime/spdlog")
		set_description("Fast C++ logging library (built with clang-cl for the xwin toolchain)")
		set_license("MIT")
		add_urls("https://github.com/gabime/spdlog.git")
		add_configs("header_only", { description = "Use header only version.", default = true, type = "boolean" })
		add_configs("std_format", { description = "Use std::format instead of fmt library.", default = false, type = "boolean" })
		add_configs("wchar", { description = "Support wchar api.", default = false, type = "boolean" })

		on_load(function(a_package)
			assert(not a_package:config("header_only") and a_package:config("std_format"),
				"the xwin toolchain builds spdlog only as CommonLibF4 asks for it (header_only = false, std_format = true)")
			a_package:add("defines", "SPDLOG_COMPILED_LIB", "SPDLOG_USE_STD_FORMAT")
			if a_package:config("wchar") then
				a_package:add("defines", "SPDLOG_WCHAR_TO_UTF8_SUPPORT")
			end
		end)

		on_install(function(a_package)
			local flags = { "/std:c++latest", "/EHsc", "/utf-8", "/Iinclude", "/DSPDLOG_COMPILED_LIB", "/DSPDLOG_USE_STD_FORMAT" }
			if a_package:config("wchar") then
				table.insert(flags, "/DSPDLOG_WCHAR_TO_UTF8_SUPPORT")
			end
			-- bundled_fmtlib_format.cpp is fmt's, unused with std::format
			local sources = os.files("src/*.cpp|bundled_fmtlib_format.cpp")
			for _, command in ipairs(xwin_static_commands(a_package, "spdlog", sources, flags)) do
				os.vrunv(command[1], command[2])
			end
			os.cp("include", a_package:installdir())
		end)
	package_end()
end
