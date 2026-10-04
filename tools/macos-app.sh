#!/bin/sh
# The macOS build steps that the makefiles in build/gmake run after linking (see premake5.lua):
#
#   macos-app.sh bundle EXECUTABLE OUTDIR ROOT [VERSION]
#       Builds OUTDIR/ImMidi.app from EXECUTABLE: its Info.plist (packaging/macos/Info.plist.in with VERSION), PkgInfo,
#       the icon, and insdef/, conversion/ and an optional fonts/ folder from ROOT (the repository) in
#       Contents/Resources; then signs it all ad hoc. Runs after every build, Debug and Release. For the ImMidiBridge
#       program it builds "OUTDIR/ImMidi Bridge.app" (packaging/macos/ImMidiBridge.plist.in, without insdef/), for
#       ImMidiConvEditor "OUTDIR/ImMidi Conversion Editor.app" (packaging/macos/ImMidiConvEditor.plist.in).
#   macos-app.sh strip FILE
#       Strips FILE, a Release build's program just linked (built without debug information, it still holds the names
#       of its functions), of every symbol it does not need to run. Apple's linker signs what it links, and a change
#       voids that signature, so FILE is signed again ad hoc; its app is then signed as a whole by the bundle step.
#       STRIP names another strip program (e.g. llvm-strip when building for a Mac on another system).

set -e

# Apple silicon runs no unsigned code. codesign comes with Apple's command line tools; a build for a Mac made on
# another system has none, and its programs are signed when they are built on a Mac.
has_codesign() {
    command -v codesign > /dev/null 2>&1
}

case "$1" in
bundle)
    exe=$2
    outdir=$3
    root=$4
    version=${5:-1.0}
    # The program's name (ImMidi, ImMidiBridge, ImMidiConvEditor) names its executable and icon in the app.
    name=$(basename "$exe")
    case "$name" in
    ImMidiBridge)
        app_name="ImMidi Bridge"
        plist=ImMidiBridge.plist.in
        data=conversion
        ;;
    ImMidiConvEditor)
        app_name="ImMidi Conversion Editor"
        plist=ImMidiConvEditor.plist.in
        data="insdef conversion"  # the definitions name the sounds
        ;;
    *)
        app_name=ImMidi
        plist=Info.plist.in
        data="insdef conversion"
        ;;
    esac
    app=$outdir/$app_name.app
    tmp=$outdir/$app_name.app.tmp

    rm -rf "$tmp"
    mkdir -p "$tmp/Contents/MacOS" "$tmp/Contents/Resources"
    cp "$exe" "$tmp/Contents/MacOS/$name"
    sed "s/@VERSION@/$version/g" "$root/packaging/macos/$plist" > "$tmp/Contents/Info.plist"
    printf 'APPL????' > "$tmp/Contents/PkgInfo"
    cp "$root/packaging/macos/$name.icns" "$tmp/Contents/Resources/$name.icns"
    # Data files: insdef/ (the player), conversion/ and an optional fonts/ folder (found through resourceDirectory()).
    for d in $data; do cp -R "$root/$d" "$tmp/Contents/Resources/$d"; done
    if [ -d "$root/fonts" ]; then cp -R "$root/fonts" "$tmp/Contents/Resources/fonts"; fi

    if command -v plutil > /dev/null 2>&1; then plutil -lint -s "$tmp/Contents/Info.plist"; fi
    rm -rf "$app"
    mv "$tmp" "$app"
    # Ad-hoc signature, so the bundle's signature covers the Info.plist and resources (the executable's own signature,
    # from the linker or the strip step, only covers the executable).
    if has_codesign; then
        codesign --force --deep --sign - "$app" > /dev/null 2>&1 || echo "macos-app.sh: codesign failed (the app still runs locally)"
    fi
    echo "Built $app"
    ;;
strip)
    file=$2
    "${STRIP:-strip}" "$file"
    if has_codesign; then
        codesign --force --sign - "$file"  # a failure stops the build: the program would not run unsigned
    fi
    ;;
*)
    echo "usage: $0 bundle EXECUTABLE OUTDIR ROOT [VERSION] | strip FILE" >&2
    exit 2
    ;;
esac
