-- ImMidi build script for premake5 (5.0.0-beta8 or newer).
--
--   Windows (Visual Studio 2022):  premake5 vs2022   -> build/vs2022/ImMidi.sln
--   Linux and macOS:               premake5 gmake    -> make -C build/gmake config=release
--
-- Programs: ImMidi (the player), ImMidiBridge (ImMidi Bridge: the player's emulation as a realtime
-- MIDI filter for other programs), ImMidiConvEditor (ImMidi Conversion Editor: edits the emulation's
-- conversion tables) and immidi_cli (the command line tool).
--
-- Every dependency (Dear ImGui, GLFW, RtMidi, stb; on Linux the Vulkan headers and the volk
-- loader) is compiled from third_party/, nothing is downloaded during the build.
--
-- Graphics: Direct3D 11 on Windows, Metal on macOS, Vulkan on Linux; OpenGL (deprecated) is
-- built everywhere as the fallback.

local immidiVersion = "1.0"  -- also in packaging/windows/ImMidi.rc

newoption {
    trigger = "no-wayland",
    description = "Linux: build for X11 only (Wayland needs the libwayland-dev and libxkbcommon-dev headers)",
}

workspace "ImMidi"
    location ("build/" .. (_ACTION or "none"))  -- no action: premake5 --help, --version
    configurations { "Debug", "Release" }
    local targetOs = os.target and os.target() or _TARGET_OS
    if targetOs == "windows" then
        platforms { "x64" }
    end
    startproject "ImMidi"

    language "C++"
    cppdialect "C++17"
    cdialect "C11"
    targetdir ("bin/%{cfg.system}-%{cfg.buildcfg}")
    objdir ("obj/%{cfg.system}-%{cfg.buildcfg}/%{prj.name}")
    staticruntime "On"
    warnings "Default"

    filter "platforms:x64"
        architecture "x86_64"

    filter "configurations:Debug"
        defines { "DEBUG" }
        symbols "On"
        optimize "Off"
        runtime "Debug"

    filter "configurations:Release"
        defines { "NDEBUG" }
        optimize "Speed"
        runtime "Release"

    -- A Release build's debug information: Visual Studio keeps it in .pdb files beside the programs, which they do not
    -- need to run, but on Linux and macOS it would go into the programs themselves. There they are built without it
    -- and stripped: gcc links them with -s (premake adds it with symbols "Off"; its clang toolset does not, so clang
    -- gets it here). macOS's linker has no such option, so the makefiles strip what they link right after linking it
    -- (tools/macos-app.sh strip), which also signs the program again, as the change voids the signature Apple's linker
    -- gave it. These workspace-wide commands come before a project's own, so the programs are stripped before their
    -- bundle steps copy them into their apps and sign each app as a whole.
    filter { "configurations:Release", "toolset:msc*" }
        symbols "On"
    filter { "configurations:Release", "toolset:not msc*" }
        symbols "Off"
    filter { "configurations:Release", "system:linux", "toolset:clang" }
        linkoptions { "-s" }
    filter { "configurations:Release", "system:macosx", "kind:ConsoleApp" }
        postbuildcommands { 'sh ../../tools/macos-app.sh strip "$(TARGET)"' }

    filter "system:windows"
        systemversion "latest"
        defines { "_CRT_SECURE_NO_WARNINGS", "NOMINMAX", "WIN32_LEAN_AND_MEAN" }

    filter "toolset:msc*"
        buildoptions { "/utf-8", "/Zc:__cplusplus", "/MP" }
        disablewarnings { "4244", "4267", "4305" }   -- numeric narrowing noise in third-party code

    filter {}

-- ---------------------------------------------------------------- GLFW
project "glfw"
    kind "StaticLib"
    language "C"
    warnings "Off"
    includedirs { "third_party/glfw/include" }
    files {
        "third_party/glfw/include/GLFW/*.h",
        "third_party/glfw/src/internal.h", "third_party/glfw/src/platform.h", "third_party/glfw/src/mappings.h",
        "third_party/glfw/src/context.c", "third_party/glfw/src/init.c", "third_party/glfw/src/input.c",
        "third_party/glfw/src/monitor.c", "third_party/glfw/src/platform.c", "third_party/glfw/src/vulkan.c",
        "third_party/glfw/src/window.c", "third_party/glfw/src/egl_context.c", "third_party/glfw/src/osmesa_context.c",
        "third_party/glfw/src/null_init.c", "third_party/glfw/src/null_monitor.c", "third_party/glfw/src/null_window.c",
        "third_party/glfw/src/null_joystick.c",
    }

    filter "system:windows"
        defines { "_GLFW_WIN32", "UNICODE", "_UNICODE" }
        files {
            "third_party/glfw/src/win32_init.c", "third_party/glfw/src/win32_joystick.c", "third_party/glfw/src/win32_monitor.c",
            "third_party/glfw/src/win32_window.c", "third_party/glfw/src/wgl_context.c", "third_party/glfw/src/win32_module.c",
            "third_party/glfw/src/win32_time.c", "third_party/glfw/src/win32_thread.c",
        }

    filter "system:linux"
        cdialect "gnu11"
        defines { "_GLFW_X11" }
        files {
            "third_party/glfw/src/x11_init.c", "third_party/glfw/src/x11_monitor.c", "third_party/glfw/src/x11_window.c",
            "third_party/glfw/src/xkb_unicode.c", "third_party/glfw/src/glx_context.c", "third_party/glfw/src/linux_joystick.c",
            "third_party/glfw/src/posix_poll.c", "third_party/glfw/src/posix_module.c", "third_party/glfw/src/posix_time.c",
            "third_party/glfw/src/posix_thread.c",
        }
    if not _OPTIONS["no-wayland"] then
        -- Wayland is preferred when the session is a Wayland one; its libraries are loaded at run
        -- time, so the same binary also runs on X11-only systems. The protocol headers in
        -- third_party/glfw/src/wayland were generated with wayland-scanner.
        defines { "_GLFW_WAYLAND" }
        includedirs { "third_party/glfw/src/wayland" }
        files { "third_party/glfw/src/wl_init.c", "third_party/glfw/src/wl_monitor.c", "third_party/glfw/src/wl_window.c" }
    end

    filter "system:macosx"
        defines { "_GLFW_COCOA" }
        files {
            "third_party/glfw/src/cocoa_init.m", "third_party/glfw/src/cocoa_joystick.m", "third_party/glfw/src/cocoa_monitor.m",
            "third_party/glfw/src/cocoa_window.m", "third_party/glfw/src/nsgl_context.m", "third_party/glfw/src/cocoa_time.c",
            "third_party/glfw/src/posix_module.c", "third_party/glfw/src/posix_thread.c",
        }

    filter {}

-- ---------------------------------------------------------------- Dear ImGui
project "imgui"
    kind "StaticLib"
    warnings "Off"
    includedirs { "third_party/imgui", "third_party/imgui/backends", "third_party/glfw/include" }
    files {
        "third_party/imgui/*.h", "third_party/imgui/*.cpp",
        "third_party/imgui/backends/imgui_impl_glfw.*", "third_party/imgui/backends/imgui_impl_opengl3*",
    }
    filter "system:windows"
        files { "third_party/imgui/backends/imgui_impl_dx11.*" }   -- the Direct3D 11 renderer
    filter "system:macosx"
        files { "third_party/imgui/backends/imgui_impl_metal.*" }  -- the Metal renderer
    filter "files:third_party/imgui/backends/imgui_impl_metal.mm"
        buildoptions { "-fobjc-arc" }
    filter "system:linux"
        -- The Vulkan renderer; volk opens the Vulkan loader at run time (no link dependency).
        files { "third_party/imgui/backends/imgui_impl_vulkan.*", "third_party/volk/volk.c", "third_party/volk/volk.h" }
        includedirs { "third_party/volk", "third_party/vulkan/include" }
        defines { "IMGUI_IMPL_VULKAN_USE_VOLK" }
    filter {}

-- ---------------------------------------------------------------- RtMidi
project "rtmidi"
    kind "StaticLib"
    warnings "Off"
    files { "third_party/rtmidi/RtMidi.h", "third_party/rtmidi/RtMidi.cpp" }
    filter "system:windows"
        defines { "__WINDOWS_MM__" }
    filter "system:linux"
        defines { "__LINUX_ALSA__" }
    filter "system:macosx"
        defines { "__MACOSX_CORE__" }
    filter {}

-- ---------------------------------------------------------------- player core
project "immidi_core"
    kind "StaticLib"
    includedirs { "src/core", "third_party/rtmidi" }
    files { "src/core/*.h", "src/core/*.cpp", "src/core/*.inc" }

-- Links shared by the programs (static libraries first, dependants before dependencies).
local function linkPlayer()
    links { "immidi_core", "rtmidi" }
    filter "system:windows"
        links { "winmm", "shell32" }
    filter "system:linux"
        links { "asound", "pthread" }
    filter "system:macosx"
        links { "CoreMIDI.framework", "CoreAudio.framework", "CoreFoundation.framework", "AudioToolbox.framework" }
    filter {}
end

-- ---------------------------------------------------------------- GUI application
project "ImMidi"
    kind "WindowedApp"
    filter "system:not windows"
        kind "ConsoleApp"   -- a plain executable; the window is created by GLFW
    filter {}
    includedirs { "src", "src/ui", "src/core", "third_party/imgui", "third_party/imgui/backends",
                  "third_party/glfw/include", "third_party/stb", "third_party/rtmidi" }
    files { "src/main.cpp", "src/ui/*.h", "src/ui/*.cpp", "src/ui/*.inc" }
    defines { 'IMMIDI_VERSION="' .. immidiVersion .. '"' }
    links { "imgui", "glfw" }
    linkPlayer()
    filter "system:windows"
        entrypoint "mainCRTStartup"
        -- Icon (also used for the window by GLFW) and version information.
        files { "packaging/windows/ImMidi.rc" }
        resincludedirs { "packaging/windows" }
        links { "d3d11", "dxgi", "d3dcompiler", "opengl32", "gdi32", "user32", "shell32", "ole32", "uuid" }   -- ole32/uuid: file dialogs, drag and drop
    filter "system:linux"
        defines { "IMMIDI_VULKAN", "IMGUI_IMPL_VULKAN_USE_VOLK" }
        includedirs { "third_party/volk", "third_party/vulkan/include" }
        links { "GL", "X11", "dl", "m", "rt" }
    filter "system:macosx"
        files { "src/ui/*.mm" }   -- NSOpenPanel / NSSavePanel, the Metal renderer
        links { "Cocoa.framework", "IOKit.framework", "Metal.framework", "OpenGL.framework", "QuartzCore.framework" }
    filter "files:src/ui/RendererMetal.mm"
        buildoptions { "-fobjc-arc" }
    -- Each platform builds its native renderer (and the OpenGL fallback).
    filter "system:not windows"
        removefiles { "src/ui/RendererD3D11.cpp" }
    filter "system:not linux"
        removefiles { "src/ui/RendererVulkan.cpp" }
    filter {}
    -- Instrument definitions are looked up next to the executable. Build commands run in the
    -- project folder (build/<action>/), so the repository root is two levels up.
    filter "system:windows"
        postbuildcommands {
            '{COPYDIR} "../../insdef" "%{cfg.buildtarget.directory}/insdef"',
            '{COPYDIR} "../../conversion" "%{cfg.buildtarget.directory}/conversion"',
        }
    filter "system:not windows"
        -- Copy folder contents ("src/."), so repeated builds do not nest the folders.
        postbuildcommands {
            'mkdir -p "%{cfg.buildtarget.directory}/insdef" "%{cfg.buildtarget.directory}/conversion"',
            'cp -Rf ../../insdef/. "%{cfg.buildtarget.directory}/insdef"',
            'cp -Rf ../../conversion/. "%{cfg.buildtarget.directory}/conversion"',
        }
    filter "system:macosx"
        -- ImMidi.app next to the executable: Info.plist, PkgInfo, icon, data files, ad-hoc signature.
        postbuildcommands {
            'sh ../../tools/macos-app.sh bundle "%{cfg.buildtarget.abspath}" "%{cfg.buildtarget.directory}" ../.. ' .. immidiVersion,
        }
    filter {}

-- ---------------------------------------------------------------- ImMidi Bridge
-- The player's emulation as a realtime MIDI filter for other programs: converts what a MIDI input
-- receives for another sound module and sends it to a MIDI output. A window of its own, with the
-- player's renderers (src/ui/Renderer*), fonts and widgets.
project "ImMidiBridge"
    kind "WindowedApp"
    filter "system:not windows"
        kind "ConsoleApp"   -- a plain executable; the window is created by GLFW
    filter {}
    includedirs { "src", "src/ui", "src/core", "third_party/imgui", "third_party/imgui/backends",
                  "third_party/glfw/include", "third_party/stb", "third_party/rtmidi" }
    files {
        "src/bridge/*.h", "src/bridge/*.cpp",
        "src/ui/AppIcon.h", "src/ui/AppIcon.cpp", "src/ui/Fonts.h", "src/ui/Fonts.cpp", "src/ui/RobotoMedium.inc",
        "src/ui/Renderer.h", "src/ui/Renderer.cpp", "src/ui/RendererGL.cpp", "src/ui/Widgets.h", "src/ui/Widgets.cpp",
    }
    defines { 'IMMIDI_VERSION="' .. immidiVersion .. '"' }
    links { "imgui", "glfw" }
    linkPlayer()
    filter "system:windows"
        entrypoint "mainCRTStartup"
        files { "src/ui/RendererD3D11.cpp", "packaging/windows/ImMidiBridge.rc" }
        resincludedirs { "packaging/windows" }
        links { "d3d11", "dxgi", "d3dcompiler", "opengl32", "gdi32", "user32", "shell32" }
    filter "system:linux"
        files { "src/ui/RendererVulkan.cpp" }
        defines { "IMMIDI_VULKAN", "IMGUI_IMPL_VULKAN_USE_VOLK" }
        includedirs { "third_party/volk", "third_party/vulkan/include" }
        links { "GL", "X11", "dl", "m", "rt" }
    filter "system:macosx"
        files { "src/ui/RendererMetal.mm" }
        links { "Cocoa.framework", "IOKit.framework", "Metal.framework", "OpenGL.framework", "QuartzCore.framework" }
    filter "files:src/ui/RendererMetal.mm"
        buildoptions { "-fobjc-arc" }
    filter {}
    -- The player's conversion tables, beside the executable.
    filter "system:windows"
        postbuildcommands { '{COPYDIR} "../../conversion" "%{cfg.buildtarget.directory}/conversion"' }
    filter "system:not windows"
        postbuildcommands {
            'mkdir -p "%{cfg.buildtarget.directory}/conversion"',
            'cp -Rf ../../conversion/. "%{cfg.buildtarget.directory}/conversion"',
        }
    filter "system:macosx"
        -- "ImMidi Bridge.app" next to the executable, as ImMidi.app.
        postbuildcommands {
            'sh ../../tools/macos-app.sh bundle "%{cfg.buildtarget.abspath}" "%{cfg.buildtarget.directory}" ../.. ' .. immidiVersion,
        }
    filter {}

-- ---------------------------------------------------------------- ImMidi Conversion Editor
-- Edits the conversion tables of the emulation (conversion/*.json): sounds, drum kits, note maps and
-- controller rules. A window of its own, with the player's renderers, fonts, widgets and file dialogs.
project "ImMidiConvEditor"
    kind "WindowedApp"
    filter "system:not windows"
        kind "ConsoleApp"   -- a plain executable; the window is created by GLFW
    filter {}
    includedirs { "src", "src/ui", "src/core", "third_party/imgui", "third_party/imgui/backends",
                  "third_party/glfw/include", "third_party/stb", "third_party/rtmidi" }
    files {
        "src/editor/*.h", "src/editor/*.cpp",
        "src/ui/AppIcon.h", "src/ui/AppIcon.cpp", "src/ui/Fonts.h", "src/ui/Fonts.cpp", "src/ui/RobotoMedium.inc",
        "src/ui/NativeDialogs.h", "src/ui/NativeDialogs.cpp",
        "src/ui/Renderer.h", "src/ui/Renderer.cpp", "src/ui/RendererGL.cpp", "src/ui/Widgets.h", "src/ui/Widgets.cpp",
    }
    defines { 'IMMIDI_VERSION="' .. immidiVersion .. '"' }
    links { "imgui", "glfw" }
    linkPlayer()
    filter "system:windows"
        entrypoint "mainCRTStartup"
        files { "src/ui/RendererD3D11.cpp", "packaging/windows/ImMidiConvEditor.rc" }
        resincludedirs { "packaging/windows" }
        links { "d3d11", "dxgi", "d3dcompiler", "opengl32", "gdi32", "user32", "shell32", "ole32", "uuid" }   -- ole32/uuid: file dialogs
    filter "system:linux"
        files { "src/ui/RendererVulkan.cpp" }
        defines { "IMMIDI_VULKAN", "IMGUI_IMPL_VULKAN_USE_VOLK" }
        includedirs { "third_party/volk", "third_party/vulkan/include" }
        links { "GL", "X11", "dl", "m", "rt" }
    filter "system:macosx"
        files { "src/ui/RendererMetal.mm", "src/ui/NativeDialogs_mac.mm" }
        links { "Cocoa.framework", "IOKit.framework", "Metal.framework", "OpenGL.framework", "QuartzCore.framework" }
    filter "files:src/ui/RendererMetal.mm"
        buildoptions { "-fobjc-arc" }
    filter {}
    -- The conversion tables and the instrument definitions (for the sound names), beside the executable.
    filter "system:windows"
        postbuildcommands {
            '{COPYDIR} "../../insdef" "%{cfg.buildtarget.directory}/insdef"',
            '{COPYDIR} "../../conversion" "%{cfg.buildtarget.directory}/conversion"',
        }
    filter "system:not windows"
        postbuildcommands {
            'mkdir -p "%{cfg.buildtarget.directory}/insdef" "%{cfg.buildtarget.directory}/conversion"',
            'cp -Rf ../../insdef/. "%{cfg.buildtarget.directory}/insdef"',
            'cp -Rf ../../conversion/. "%{cfg.buildtarget.directory}/conversion"',
        }
    filter "system:macosx"
        -- "ImMidi Conversion Editor.app" next to the executable, as ImMidi.app.
        postbuildcommands {
            'sh ../../tools/macos-app.sh bundle "%{cfg.buildtarget.abspath}" "%{cfg.buildtarget.directory}" ../.. ' .. immidiVersion,
        }
    filter {}

-- ---------------------------------------------------------------- command line tool
project "immidi_cli"
    kind "ConsoleApp"
    includedirs { "src/core", "third_party/rtmidi" }
    files { "tools/immidi_cli.cpp" }
    linkPlayer()
