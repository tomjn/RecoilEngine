#!/bin/bash
# Build a portable macOS engine archive.
#
# The result is an extractable directory in the shape a lobby expects, not an
# .app bundle and not signed. It must run on a machine with no Homebrew, so
# every non-system dylib is copied in and every install name rewritten to
# @executable_path/lib.
#
# Two things dylibbundler cannot find on its own, because they are dlopened
# rather than linked: the gallium driver, reached through LIBGL_DRIVERS_PATH,
# and the kosmickrisp ICD, reached through VK_DRIVER_FILES. Both are passed to
# it explicitly below.
#
#   package-macos.sh [outdir]
#
# Mesa is pinned to 56588ef0665, the commit before "kk: Move to Metal4 command
# encoding" (c08dba83025). Everything after that never returns the memory a
# render pass allocates, see coding-agents/upstream-kosmickrisp-issue.md. No
# released Mesa works, so this is built from source and the pin is deliberate.
set -euo pipefail

REPO=/Users/tomjn/dev/RecoilEngine
OUT=${1:-$REPO/dist/recoil-macos-arm64}
BUILD=$REPO/build-macos-legacy
MESA=${MESA_PREFIX:-$HOME/dev/mesa-install-premtl4}
VKLIB=${VK_LOADER_LIB:-/opt/homebrew/opt/vulkan-loader/lib}
BASE=${BASE_CONTENT:-$HOME/dev/spring-testdata/base}

command -v dylibbundler > /dev/null || { echo "need dylibbundler (brew install dylibbundler)"; exit 1; }
[ -x "$BUILD/spring" ] || { echo "no engine at $BUILD/spring, build engine-legacy first"; exit 1; }
[ -d "$MESA/lib" ]     || { echo "no Mesa prefix at $MESA"; exit 1; }

echo "==> staging $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/lib"

cp "$BUILD/spring" "$OUT/spring-bin"

# The build links whichever Mesa prefix it was compiled against, which is the
# post-Metal4 one. Left alone, dylibbundler follows that and pulls the leaking
# gallium into the archive, silently undoing the whole reason for the pin.
# Point it at the bundled copy before anything else runs.
OLD_EGL=$(otool -L "$OUT/spring-bin" | awk '/libEGL\.1\.dylib/ {print $1; exit}')
if [ -n "$OLD_EGL" ]; then
	echo "==> repointing libEGL from $OLD_EGL"
	install_name_tool -change "$OLD_EGL" "@executable_path/lib/libEGL.1.dylib" "$OUT/spring-bin"
fi

# -L so the versioned target is copied rather than a symlink into Homebrew
cp -L "$MESA"/lib/libEGL.1.dylib "$OUT/lib/"
cp -L "$MESA"/lib/libgallium-*.dylib "$OUT/lib/"
cp -L "$MESA"/lib/libvulkan_kosmickrisp.dylib "$OUT/lib/"
cp -L "$VKLIB"/libvulkan.1.dylib "$OUT/lib/"

# The engine links sdl2-compat, which is a shim that dlopens SDL3 rather than
# linking it, so dylibbundler cannot see it. It looks for @loader_path first,
# and the loader here is libSDL2 in lib/, so SDL3 has to sit beside it. Without
# this the packaged engine aborts before it logs anything.
SDL3=${SDL3_LIB:-/opt/homebrew/opt/sdl3/lib/libSDL3.dylib}
if [ -f "$SDL3" ]; then
	cp -L "$SDL3" "$OUT/lib/libSDL3.dylib"
else
	echo "!! no libSDL3 at $SDL3, the engine will abort on start"
fi

if [ -d "$BASE" ]; then
	echo "==> base content from $BASE"
	mkdir -p "$OUT/base"
	cp -R "$BASE"/. "$OUT/base/"
else
	echo "!! no base content at $BASE, the engine will not start without it"
fi

# The ICD sits beside the driver so its library_path can stay relative, which
# keeps the archive relocatable.
cat > "$OUT/lib/kosmickrisp_icd.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "./libvulkan_kosmickrisp.dylib",
        "api_version": "1.3.0"
    }
}
JSON

# dylibbundler rewrites what a file depends on, not what a file calls itself.
# The four copied in by hand still carry their build location as an install
# name, which makes the archive non relocatable.
echo "==> rewriting install names"
for f in "$OUT/lib"/*.dylib; do
	install_name_tool -id "@executable_path/lib/$(basename "$f")" "$f" 2>/dev/null || true
done

echo "==> bundling dependencies"
dylibbundler --overwrite-files --bundle-deps \
	--fix-file "$OUT/spring-bin" \
	--fix-file "$OUT/lib/libEGL.1.dylib" \
	--fix-file "$OUT/lib"/libgallium-*.dylib \
	--fix-file "$OUT/lib/libvulkan_kosmickrisp.dylib" \
	--fix-file "$OUT/lib/libvulkan.1.dylib" \
	--fix-file "$OUT/lib/libSDL3.dylib" \
	--dest-dir "$OUT/lib" \
	--install-path "@executable_path/lib" \
	> "$OUT/.bundle.log" 2>&1 || { tail -20 "$OUT/.bundle.log"; exit 1; }

# zink reaches the Vulkan loader by name, so the binary needs somewhere to look
# that is not Homebrew. DYLD_LIBRARY_PATH would also work and is the wrong tool:
# it is stripped for signed and hardened binaries later on.
# dylibbundler adds this rpath once per dependency it rewrites, and dyld refuses
# to start a process carrying a duplicate LC_RPATH. Strip every copy, then add
# exactly one.
while otool -l "$OUT/spring-bin" | grep -q "@executable_path/lib"; do
	install_name_tool -delete_rpath "@executable_path/lib/" "$OUT/spring-bin" 2>/dev/null \
		|| install_name_tool -delete_rpath "@executable_path/lib" "$OUT/spring-bin" 2>/dev/null \
		|| break
done
install_name_tool -add_rpath "@executable_path/lib" "$OUT/spring-bin"
codesign --force -s - "$OUT/spring-bin" 2>/dev/null || true

cat > "$OUT/spring" <<'SH'
#!/bin/sh
# The engine. This is a launcher, and the binary sits beside it as spring-bin.
#
# It has to be this way round because a lobby runs the file called "spring".
# Mesa picks its driver entirely through the environment, and none of
# MESA_LOADER_DRIVER_OVERRIDE, GALLIUM_DRIVER or VK_DRIVER_FILES can be baked
# into a Mach-O, so a bare binary gets no GL context and no useful error.
#
# Everything resolves relative to this file, so the archive runs from wherever
# it was extracted.
DIR=$(cd "$(dirname "$0")" && pwd)

export EGL_PLATFORM=surfaceless
export MESA_LOADER_DRIVER_OVERRIDE=zink
export GALLIUM_DRIVER=zink
export LIBGL_DRIVERS_PATH="$DIR/lib"
export VK_DRIVER_FILES="$DIR/lib/kosmickrisp_icd.json"
export DYLD_LIBRARY_PATH="$DIR/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

# Zink reports GL 2.1 without these and every context request at 3.0 or above
# fails with EGL_BAD_MATCH.
export MESA_GL_VERSION_OVERRIDE=4.6
export MESA_GLSL_VERSION_OVERRIDE=460

exec "$DIR/spring-bin" "$@"
SH
chmod +x "$OUT/spring"

echo "==> checking nothing still points outside the archive"
LEAKS=$( (otool -L "$OUT/spring-bin" "$OUT/lib"/*.dylib || true) \
	| grep -v ":$" | grep -E "/opt/homebrew|/Users/" || true)
if [ -n "$LEAKS" ]; then
	echo "!! these still reference paths outside the archive:"
	echo "$LEAKS" | sed 's/^/   /'
else
	echo "   clean, no Homebrew or home directory references"
fi

echo "==> $(du -sh "$OUT" | cut -f1) in $OUT"

# Zipped the way pr-downloader expects to find one. It extracts the archive flat
# into <springdir>/engine/<platform>/<version>/, so the archive must have no
# wrapping directory of its own: spring, spring-bin, lib and base sit at the top.
# See CFileSystem::extractEngine.
VERSION=$("$OUT/spring" --sync-version 2>/dev/null | head -1)
ZIP=$(dirname "$OUT")/recoil_$(echo "$VERSION" | tr ' /' '__')_macos_arm64.zip

echo "==> zipping as $(basename "$ZIP")"
rm -f "$ZIP"
(cd "$OUT" && zip -qr "$ZIP" . -x ".bundle.log")

echo "==> $(du -sh "$ZIP" | cut -f1) in $ZIP"
# pr-downloader replaces \/:?"<>| with _ before using the version as a directory
# name, see CFileSystem::EscapeFilename, so the branch slash becomes underscore.
ESCAPED=$(echo "$VERSION" | tr '\\/:?"<>|' '_')

echo
echo "    version: $VERSION"
echo "    install: unzip -q '$ZIP' -d ~/.spring/engine/macos_arm64/'$ESCAPED'"
