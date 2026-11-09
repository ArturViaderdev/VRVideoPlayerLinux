#!/bin/sh
# Builds and installs VR Video Player in ~/.local (no root needed):
#   vrvideo (player), vr-play (launcher), vrvideo-gui (window to choose the video),
#   vr-bd + vr-mvcdec (3D Blu-ray), and a menu entry.
set -e
cd "$(dirname "$0")"

need=""
for c in cmake g++ gcc pkg-config python3; do command -v "$c" >/dev/null 2>&1 || need="$need $c"; done
for m in mpv openxr glfw3 vulkan; do pkg-config --exists "$m" 2>/dev/null || need="$need lib:$m"; done
if [ -n "$need" ]; then
  echo "Faltan dependencias:$need"
  echo "  sudo apt install build-essential cmake pkg-config libmpv-dev libopenxr-dev libvulkan-dev \\"
  echo "                   libglfw3-dev libgl-dev python3 python3-tk ffmpeg python3-keyring"
  echo "(son cabeceras y cargadores: no tocan el driver de NVIDIA)"
  exit 1
fi

echo "== vrvideo"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc 2>/dev/null || echo 4)"
echo "== vr-mvcdec (Blu-ray 3D)"
./bluray/build.sh

BIN="$HOME/.local/bin"
SHARE="${XDG_DATA_HOME:-$HOME/.local/share}"
mkdir -p "$BIN" "$SHARE/vrvideoplayer" "$SHARE/applications"
cp build/vrvideo bluray/build/vr-mvcdec "$BIN/"
cp vr-play vrvideo-gui bluray/vr-bd "$BIN/"
cp bluray/vr-bd.lua "$SHARE/vrvideoplayer/"
chmod +x "$BIN/vrvideo" "$BIN/vr-mvcdec" "$BIN/vr-play" "$BIN/vrvideo-gui" "$BIN/vr-bd"

cat > "$SHARE/applications/vrvideoplayer.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=VR Video Player
GenericName=3D video player for VR
Comment=Plays SBS, top/bottom and 3D Blu-ray in the VR headset (WiVRn / SteamVR)
Exec=$BIN/vrvideo-gui %f
Icon=video-display
Terminal=false
Categories=AudioVideo;Video;Player;
MimeType=video/x-matroska;video/mp4;video/quicktime;video/x-msvideo;video/mp2t;video/webm;application/x-iso9660-image;inode/directory;
EOF
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$SHARE/applications" 2>/dev/null || true

echo
echo "Instalado en $BIN: vrvideo-gui, vr-play, vrvideo, vr-bd, vr-mvcdec"
echo "Menu: VR Video Player"
python3 -c "import tkinter" 2>/dev/null || echo "Falta python3-tk para la ventana:  sudo apt install python3-tk"
python3 -c "import keyring" 2>/dev/null || command -v secret-tool >/dev/null 2>&1 || echo "Opcional, para recordar la contrasena de DeoVR:  sudo apt install python3-keyring"
command -v ffprobe >/dev/null 2>&1 || echo "Falta ffmpeg (deteccion de formato y subtitulos):  sudo apt install ffmpeg"
case ":$PATH:" in *":$BIN:"*) ;; *) echo "AVISO: $BIN no esta en el PATH (cierra sesion y vuelve a entrar)";; esac
