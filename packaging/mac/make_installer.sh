#!/bin/bash
# Builds "Ember Amp <version> (Mac).dmg": an installer package (app + AU + VST3) and a Read Me.
# usage: packaging/mac/make_installer.sh <artefacts Release dir> <version> <output dir>
set -euo pipefail

ART="$1"; VERSION="$2"; OUT="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# stage each part in its own root, ad-hoc signed so macOS will load it
stage () {   # stage <source bundle> <name>
    mkdir -p "$WORK/root-$2"
    cp -R "$1" "$WORK/root-$2/"
    codesign --force --deep --sign - "$WORK/root-$2/$(basename "$1")" 2>/dev/null
}
stage "$ART/Standalone/Ember Amp.app"     app
stage "$ART/AU/Ember Amp.component"       au
stage "$ART/VST3/Ember Amp.vst3"          vst3

# component packages (bundles stay exactly where they're installed, never "relocated")
component () {   # component <name> <install location> <identifier>
    pkgbuild --analyze --root "$WORK/root-$1" "$WORK/$1.plist" >/dev/null
    /usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" "$WORK/$1.plist" 2>/dev/null || true   # only .app bundles are listed
    pkgbuild --root "$WORK/root-$1" --component-plist "$WORK/$1.plist" \
             --install-location "$2" --identifier "$3" --version "$VERSION" "$WORK/$1.pkg" >/dev/null
}
component app  "/Applications"                      com.emberaudio.emberamp.app
component au   "/Library/Audio/Plug-Ins/Components" com.emberaudio.emberamp.au
component vst3 "/Library/Audio/Plug-Ins/VST3"       com.emberaudio.emberamp.vst3

# one installer with a title and a welcome page
productbuild --synthesize --package "$WORK/app.pkg" --package "$WORK/au.pkg" --package "$WORK/vst3.pkg" "$WORK/dist.xml"
python3 - "$WORK/dist.xml" <<'EOF'
import sys
p = sys.argv[1]; x = open(p).read()
x = x.replace('<installer-gui-script minSpecVersion="1">',
              '<installer-gui-script minSpecVersion="1">\n    <title>Ember Amp</title>\n    <welcome file="welcome.txt"/>\n    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>')
open(p, 'w').write(x)
EOF
mkdir -p "$WORK/res"
cat > "$WORK/res/welcome.txt" <<EOF
Ember Amp $VERSION

This installs:
  - Ember Amp (the app) in Applications
  - The Ember Amp AU plugin (Logic, GarageBand, ...)
  - The Ember Amp VST3 plugin (Ableton, Reaper, Cubase, ...)
EOF
productbuild --distribution "$WORK/dist.xml" --resources "$WORK/res" --package-path "$WORK" "$WORK/Install Ember Amp.pkg" >/dev/null

# disk image: installer + read me
mkdir -p "$WORK/dmg"
cp "$WORK/Install Ember Amp.pkg" "$WORK/dmg/"
cp "$HERE/Read Me.txt" "$WORK/dmg/"
DMG="$OUT/Ember-Amp-$VERSION-Mac.dmg"
rm -f "$DMG"
hdiutil create -quiet -volname "Ember Amp" -srcfolder "$WORK/dmg" -fs HFS+ -format UDZO "$DMG"
echo "$DMG"
