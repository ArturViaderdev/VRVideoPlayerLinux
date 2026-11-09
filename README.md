<a id="english"></a>

# VR Video Player — 3D movies in a VR headset on Linux

**English** · [Español](#español)

Plays **side-by-side (SBS)**, **top-bottom** and decrypted **3D Blu-ray** (folder or ISO) movies in real 3D
on a virtual cinema screen in your VR headset, through **WiVRn** or **SteamVR** (any OpenXR runtime).
VR version of **[3dvisionvideoplayer](https://github.com/ArturViaderdev/3dvisionvideoplayer)**
(the NVIDIA 3D Vision player): same mpv engine, same formats, same 3D Blu-ray decoder.

- **Full SBS** 3840x1080 (each eye 1920x1080), 5120x1440, **7680x2160 (4K per eye)**...
- **Half SBS**, **full / half top-bottom**, and plain **2D** videos on the big screen.
- **VR videos 180° and 360°** (equirectangular) and fisheye, 2D or 3D (side by side / top-bottom), up to 8K per eye.
- **DeoVR** (and DeoVR-compatible sites): catalogue, video links and login for your premium videos.
- **3D Blu-ray (MVC)** without re-encoding: both eyes at full 1920x1080, the disc's original audio.
- Folders: plays all the videos in a folder; online videos (YouTube & co. via yt-dlp).
- A **window to choose** the file, folder, ISO or Blu-ray (`vrvideo-gui`), plus a command line (`vr-play`).

## How it works

`vrvideo` uses **libmpv** (NVDEC hardware decoding, every format, audio, seeking, filters) and **OpenXR**
on **Vulkan** (supported by WiVRn, SteamVR and Monado). mpv renders each frame with OpenGL into memory shared
with Vulkan (no CPU copies) and each eye's half is shown on the virtual screen:

- **Per-eye layers (default)**: two OpenXR quad layers, one only visible to the left eye and one to the right
  eye. The runtime samples the video directly onto the screen: one resample, the sharpest image.
- **Own rendering** (`--mode render`): the screen is drawn by the player into the eye images. Use it if a
  runtime shows the same image in both eyes (it ignores per-eye layers).

Resolution: mpv renders each eye at the pixel density the screen actually has in the headset (×1.25), with its
high-quality scalers, up to 4096 px per eye (`--vr-max-eye`), never more than twice the source. A 4K-per-eye
movie on a big screen is downscaled by mpv (much better than the compositor's bilinear filter); `--vr-scale=native`
keeps the source resolution.

## Installation

```sh
sudo apt install build-essential cmake pkg-config libmpv-dev libopenxr-dev libvulkan-dev \
                 libglfw3-dev libgl-dev python3 python3-tk ffmpeg python3-keyring
./install.sh
```

Those packages are headers and loaders only (they don't replace a manually installed NVIDIA driver).
Everything goes to `~/.local/bin` and there is a menu entry, **VR Video Player**.

## Usage

Start **wivrn-server** and connect the headset (or start SteamVR), then:

- **Window**: `vrvideo-gui` (or the menu). Choose a file, a folder or an ISO; a folder shows its videos,
  ISOs and Blu-ray folders. Double click or **Play in the headset**.
- **Command line**:

```sh
vr-play movie.mkv                  # automatic format (file name, then frame size)
vr-play --full / --half movie.mkv  # full / half SBS
vr-play --tab / --htab movie.mkv   # full / half top-bottom
vr-play --2d movie.mkv             # normal video on the virtual screen
vr-play trip_360_TB.mp4            # VR video: detected from its metadata or name (_180_LR, _360_TB, VR180...)
vr-play --180 / --360 video.mp4    # force VR 180 / 360 (with --full / --tab / --2d for the eye layout)
vr-play --flat video.mp4           # treat it as a normal video on the screen
vr-play /path/MOVIE                # decrypted 3D Blu-ray folder (with BDMV)
vr-play /path/DISC.iso             # 3D Blu-ray ISO (mounted read-only by itself, needs udisks2)
vr-play /path/FOLDER               # every video in the folder, in order
vr-play "https://www.youtube.com/watch?v=..."
vr-play --swap movie.mkv           # swap eyes
vr-play --sub movie.en.srt movie.mkv   # text subtitles inside each eye (also --sub-track N)
vr-play --runtime wivrn movie.mkv  # wivrn | steamvr | /path/runtime.json (default: the system's active one)
vr-play --screen 4 --distance 3 movie.mkv   # screen width / distance in meters
vr-play --alang=eng /path/MOVIE    # Blu-ray audio language (default: your system language)
vr-play --start=1:02:30 --aid=2 movie.mkv   # any other option goes to mpv
```

Automatic format: first the file name (`HSBS`, `H-SBS`, `Half-SBS`, `HOU`, `FSBS`...), otherwise the frame size:
32:9 or wider → full SBS; nearly square or taller → full top-bottom; 16:9 → half SBS.

## VR videos (180° / 360°)

Equirectangular videos surround you: 180° (the usual 3D format of VR cameras, `VR180`) and 360°, in 2D, side by
side (`LR`, `3DH`) or top-bottom (`TB`, `3DV`). Detected from the file's spherical / stereo metadata (VR cameras,
YouTube downloads) or from the name (`_180_LR`, `_360_TB`, `180x180_3dh`, `VR180`, `360`); otherwise the frame
shape decides (180: 2:1 → side by side, 1:1 → 2D; 360: 1:1 → top-bottom, 2:1 → 2D). Force it with `--180`, `--360`
and `--full` / `--tab` / `--2d` (or the **Projection** and **3D format** lists in the window).

Each headset pixel looks up exactly the direction it sees in the video (no sphere mesh, no seams); only your head's
rotation counts, at the source resolution (up to 8192 px per eye). The front of the video is where you were looking
when it started: **stick click** makes the current direction the front (long press: also tilted, for lying down).

## Online videos (YouTube 3D / VR)

In the window, **Web link…** (it takes the link from the clipboard) or paste it in the path field: the player
looks it up and shows the title, resolution and what it detected (3D SBS / top-bottom, VR 180° / 360°, or 2D)
before playing; the **Projection** and **3D format** lists correct it if needed. Command line: `vr-play "URL"`.

- Detection: the stream's own spherical / stereo metadata (YouTube adds it to its 360 and 3D videos), then the
  title and tags (`360`, `VR180`, `3D`, `SBS`, `OU`...), then the frame shape (32:9 → full SBS). An online video with no
  sign of 3D plays in 2D on the screen (choose the format if it actually is 3D).
- **Online quality**: up to 8K by default (VR videos need it), or 4K / 1440p / 1080p (`vr-play --quality 2160`).
  8K on YouTube is AV1: NVDEC decodes it on RTX 30 and newer.
- YouTube needs a **recent yt-dlp** in `~/.local/bin` (the distribution's one is usually too old). The window's
  **Update yt-dlp** button downloads it the first time and updates it later (`yt-dlp -U`).

## DeoVR

In the window, **DeoVR…** opens the catalogue of **deovr.com** (or of any site with the DeoVR API: write its
address in **Site**). Choose a section and double click a video. To see your premium / purchased videos, enter
your user and password and **Log in**; tick **Remember** to keep the password in the desktop keyring (GNOME
Keyring / KWallet through `python3-keyring` or `secret-tool`), never in a plain file. You can also paste the link
of a DeoVR video in the main window.

The player reads the same JSON the DeoVR app uses (`/deovr`, `/deovr/video/id/N`, `deovr://` links): it picks the
best source up to the **Online quality** and takes the projection and 3D layout from the video
(`flat`, `dome` = 180°, `sphere` = 360°, `fisheye` / `mkx200` / `rf52` = fisheye 180° / 200° / 190°; `sbs`, `tb`).
Log in works as in the DeoVR app: user and password go by HTTPS POST to the site. Command line:

```sh
vr-play --deovr-list https://deovr.com                 # catalogue as JSON
vr-play "https://deovr.com/deovr/video/id/12345"       # a video (also deovr:// links and video pages)
VR_DEOVR_PASSWORD=... vr-play --deovr-login USER URL   # logged in (without the variable: keyring, then it asks)
vr-play --deovr URL                                    # a page of another DeoVR-compatible site
```

If a DeoVR video is shown with the wrong format, the log says where it was taken from (`DeoVR formato: ...`), and
`vr-play --deovr-dump URL` prints what the site sends (source links with tokens hidden). The format can always be
forced with the **Projection** and **3D format** lists.

## Controls

| Headset controllers | |
|---|---|
| Trigger | play / pause |
| Stick right / left | +10 s / −10 s (hold: repeats) |
| Stick up / down | +1 min / −1 min |
| Grip + stick up/down | screen bigger / smaller |
| Grip + stick left/right | screen closer / farther |
| Stick click | recenter the screen in front of you (long press: also follow your head's pitch, for lying down) |
| B / Y | next audio track |
| A / X, menu | show / hide the info bar (time, progress) |
| Hold menu (1.5 s) | quit |

Desktop window (mirror of the left eye): every mpv key (space, arrows, `#` audio, `9`/`0` volume, `q` quit...),
plus **Ctrl+↑/↓** screen size, **Alt+↑/↓** distance, **F5** recenter, **F7** swap eyes, **o** info bar.

The screen size and distance you set are remembered (`~/.config/vrvideoplayer/screen.conf`). The video pauses
by itself when the headset leaves the VR session.

## WiVRn and SteamVR

- **WiVRn** (recommended): start `wivrn-server`, open WiVRn in the headset and connect. If the player starts
  before the headset connects, it waits. Sound: choose the **WiVRn** audio output in your desktop's sound
  settings (or `vr-play --audio-device=...`) to hear it in the headset.
- **SteamVR**: start SteamVR and use `--runtime steamvr` (or make SteamVR the active OpenXR runtime).
- `--runtime` finds the usual paths (`/usr/share/openxr/1/openxr_wivrn.json`, WiVRn Flatpak,
  `~/.steam/steam/steamapps/common/SteamVR/steamxr_linux64.json`); any `.json` path also works.

## Troubleshooting

- **Both eyes show the same image** (no 3D): the runtime ignores per-eye layers → choose **Own rendering** in
  the window, or `vr-play --mode render`.
- **Eyes reversed**: `--swap` (or **F7**).
- **Washed-out colors**: the runtime didn't offer an sRGB format (the log says so).
- Logs: the window shows them; the 3D Blu-ray player also writes `~/.cache/vrvideoplayer/vr-bd-*.log`.

## 3D Blu-ray

Same player as in 3dvisionvideoplayer (see [bluray/](bluray/)): finds the main 3D title (skips anti-rip decoy
playlists), handles movies split into several clips and discs whose base view is the right eye, original audio
(DTS-HD MA, TrueHD...) in sync, fast seeking with the disc's index (left/right ±10 s, up/down ±1 min,
PgUp/PgDn ±10 min — from the controllers too). No menus, extras or PGS subtitles. The disc must be decrypted.

## Windows (planned)

The code is ready to be ported: C++17 + CMake, OpenXR on Vulkan, libmpv and GLFW all exist on Windows, the
OpenGL ⇄ Vulkan sharing already has its Win32 path (`GL_EXT_memory_object_win32`), and the launcher and the
window are Python/Tkinter. Missing: a Windows build (vcpkg), named pipes and ISO mounting in `vr-bd`.

## Credits / license

GPLv3. The MVC decoder (an extended edge264, BSD-3, by Thibault Raffaillac) and the 3D Blu-ray demuxers come
from **SyLC 3D Player** by 5ymph0en1x (https://github.com/5ymph0en1x/SyLC, GPLv3), ported to Linux in
3dvisionvideoplayer.

---

<a id="español"></a>

# VR Video Player — películas 3D en un visor de realidad virtual en Linux

[English](#english) · **Español**

Reproduce películas **lado a lado (SBS)**, **arriba/abajo** y **Blu-ray 3D** desencriptados (carpeta o ISO) en 3D
real, en una pantalla de cine virtual dentro del visor, con **WiVRn** o **SteamVR** (cualquier runtime OpenXR).
Es la versión VR de **[3dvisionvideoplayer](https://github.com/ArturViaderdev/3dvisionvideoplayer)**
(el reproductor para NVIDIA 3D Vision): el mismo motor mpv, los mismos formatos y el mismo decodificador de
Blu-ray 3D.

- **SBS completo** 3840x1080 (cada ojo 1920x1080), 5120x1440, **7680x2160 (4K por ojo)**...
- **Half SBS**, **arriba/abajo completo y half**, y vídeos **2D** normales en la pantalla grande.
- **Vídeos VR de 180° y 360°** (equirectangulares) y ojo de pez, en 2D o 3D (lado a lado / arriba-abajo), hasta 8K por ojo.
- **DeoVR** (y webs compatibles): catálogo, enlaces de vídeos e inicio de sesión para tus vídeos premium.
- **Blu-ray 3D (MVC)** sin recodificar: los dos ojos a 1920x1080 completos, con el audio original del disco.
- Carpetas: reproduce todos los vídeos de una carpeta; vídeos online (YouTube y demás con yt-dlp).
- Una **ventana para elegir** archivo, carpeta, ISO o Blu-ray (`vrvideo-gui`), y línea de comandos (`vr-play`).

## Cómo funciona

`vrvideo` usa **libmpv** (decodificación por hardware NVDEC, todos los formatos, audio, avance, filtros) y
**OpenXR** sobre **Vulkan** (lo admiten WiVRn, SteamVR y Monado). mpv dibuja cada fotograma con OpenGL en una
memoria compartida con Vulkan (sin copias por CPU) y la mitad de cada ojo se muestra en la pantalla virtual:

- **Capas por ojo (por defecto)**: dos capas quad de OpenXR, una visible solo para el ojo izquierdo y otra solo
  para el derecho. El runtime pinta el vídeo directamente en la pantalla: un solo reescalado, la imagen más nítida.
- **Dibujado propio** (`--mode render`): el reproductor dibuja la pantalla en la imagen de cada ojo. Úsalo si un
  runtime enseña la misma imagen a los dos ojos (no respeta las capas por ojo).

Resolución: mpv dibuja cada ojo a la densidad de píxeles que la pantalla tiene realmente en el visor (×1,25), con
sus escaladores de alta calidad, hasta 4096 px por ojo (`--vr-max-eye`) y nunca más del doble del original. Una
película 4K por ojo en una pantalla grande la reduce mpv (mucho mejor que el filtro bilineal del compositor);
`--vr-scale=native` mantiene la resolución original.

## Instalación

```sh
sudo apt install build-essential cmake pkg-config libmpv-dev libopenxr-dev libvulkan-dev \
                 libglfw3-dev libgl-dev python3 python3-tk ffmpeg python3-keyring
./install.sh
```

Esos paquetes son solo cabeceras y cargadores (no sustituyen un driver de NVIDIA instalado a mano). Todo se
instala en `~/.local/bin` y aparece en el menú como **VR Video Player**.

## Uso

Arranca **wivrn-server** y conecta el visor (o arranca SteamVR), y luego:

- **Ventana**: `vrvideo-gui` (o desde el menú). Elige un archivo, una carpeta o una ISO; una carpeta muestra sus
  vídeos, ISOs y carpetas de Blu-ray. Doble clic o **Reproducir en el visor**.
- **Línea de comandos**:

```sh
vr-play pelicula.mkv                  # formato automático (nombre del archivo y tamaño)
vr-play --full / --half pelicula.mkv  # SBS completo / half
vr-play --tab / --htab pelicula.mkv   # arriba/abajo completo / half
vr-play --2d pelicula.mkv             # vídeo normal en la pantalla virtual
vr-play viaje_360_TB.mp4              # vídeo VR: se detecta por metadatos o por el nombre (_180_LR, _360_TB, VR180...)
vr-play --180 / --360 video.mp4       # forzar VR 180 / 360 (con --full / --tab / --2d para los ojos)
vr-play --flat video.mp4              # tratarlo como vídeo normal en la pantalla
vr-play /ruta/PELICULA                # carpeta de Blu-ray 3D desencriptado (con BDMV)
vr-play /ruta/DISCO.iso               # ISO de Blu-ray 3D (se monta sola en solo lectura, necesita udisks2)
vr-play /ruta/CARPETA                 # todos los vídeos de la carpeta, en orden
vr-play "https://www.youtube.com/watch?v=..."
vr-play --swap pelicula.mkv           # invertir ojos
vr-play --sub peli.es.srt peli.mkv    # subtítulos de texto dentro de cada ojo (también --sub-track N)
vr-play --runtime wivrn pelicula.mkv  # wivrn | steamvr | /ruta/runtime.json (por defecto el activo)
vr-play --screen 4 --distance 3 pelicula.mkv   # ancho y distancia de la pantalla en metros
vr-play --alang=spa /ruta/PELICULA    # idioma del audio del Blu-ray (por defecto el de tu sistema)
vr-play --start=1:02:30 --aid=2 peli.mkv      # cualquier otra opción va a mpv
```

Formato automático: primero por el nombre (`HSBS`, `H-SBS`, `Half-SBS`, `HOU`, `FSBS`...) y si no, por el tamaño:
32:9 o más ancho → SBS completo; casi cuadrado o más alto → arriba/abajo completo; 16:9 → half SBS.

## Vídeos VR (180° / 360°)

Los vídeos equirectangulares te rodean: 180° (el formato 3D habitual de las cámaras VR, `VR180`) y 360°, en 2D, lado a
lado (`LR`, `3DH`) o arriba/abajo (`TB`, `3DV`). Se detectan por los metadatos esféricos / estéreo del archivo
(cámaras VR, descargas de YouTube) o por el nombre (`_180_LR`, `_360_TB`, `180x180_3dh`, `VR180`, `360`); si no, decide
la forma del fotograma (180: 2:1 → lado a lado, 1:1 → 2D; 360: 1:1 → arriba/abajo, 2:1 → 2D). Se puede forzar con
`--180`, `--360` y `--full` / `--tab` / `--2d` (o con las listas **Proyección** y **Formato 3D** de la ventana).

Cada píxel del visor lee exactamente la dirección que ve en el vídeo (sin malla de esfera ni costuras); solo cuenta
el giro de la cabeza, a la resolución original (hasta 8192 px por ojo). El frente del vídeo es hacia donde mirabas al
empezar: **clic del stick** hace frente la dirección actual (pulsación larga: también inclinado, para verlo tumbado).

## Vídeos online (YouTube 3D / VR)

En la ventana, **Enlace web…** (coge el enlace del portapapeles) o pégalo en el campo de la ruta: el reproductor lo
consulta y muestra el título, la resolución y lo que ha detectado (3D SBS / arriba-abajo, VR 180° / 360°, o 2D) antes
de reproducir; las listas **Proyección** y **Formato 3D** lo corrigen si hace falta. Línea de comandos: `vr-play "URL"`.

- Detección: los metadatos esféricos / estéreo del propio vídeo (YouTube los pone en sus vídeos 360 y 3D), luego el
  título y las etiquetas (`360`, `VR180`, `3D`, `SBS`, `OU`...), y luego la forma del fotograma (32:9 → SBS completo). Un
  vídeo online sin indicios de 3D se ve en 2D en la pantalla (elige el formato si en realidad es 3D).
- **Calidad online**: hasta 8K por defecto (los vídeos VR lo necesitan), o 4K / 1440p / 1080p (`vr-play --quality 2160`).
  El 8K de YouTube es AV1: NVDEC lo decodifica en las RTX 30 y posteriores.
- YouTube necesita un **yt-dlp reciente** en `~/.local/bin` (el de la distribución suele estar viejo). El botón
  **Actualizar yt-dlp** de la ventana lo descarga la primera vez y lo actualiza después (`yt-dlp -U`).

## DeoVR

En la ventana, **DeoVR…** abre el catálogo de **deovr.com** (o de cualquier web con la API de DeoVR: escribe su
dirección en **Web**). Elige una sección y haz doble clic en un vídeo. Para ver tus vídeos premium / comprados, escribe
usuario y contraseña e **Iniciar sesión**; marca **Recordar** para guardar la contraseña en el llavero del escritorio
(GNOME Keyring / KWallet mediante `python3-keyring` o `secret-tool`), nunca en un archivo de texto. También puedes pegar
el enlace de un vídeo de DeoVR en la ventana principal.

El reproductor lee el mismo JSON que usa la app de DeoVR (`/deovr`, `/deovr/video/id/N`, enlaces `deovr://`): elige la
mejor fuente hasta la **Calidad online** y toma la proyección y el formato 3D del propio vídeo (`flat`, `dome` = 180°,
`sphere` = 360°, `fisheye` / `mkx200` / `rf52` = ojo de pez 180° / 200° / 190°; `sbs`, `tb`). El inicio de sesión
funciona como en la app de DeoVR: usuario y contraseña van por POST HTTPS a la web. Línea de comandos:

```sh
vr-play --deovr-list https://deovr.com                 # catálogo en JSON
vr-play "https://deovr.com/deovr/video/id/12345"       # un vídeo (también enlaces deovr:// y páginas de vídeo)
VR_DEOVR_PASSWORD=... vr-play --deovr-login USUARIO URL   # con sesión (sin la variable: llavero, y si no, la pregunta)
vr-play --deovr URL                                    # una página de otra web compatible con DeoVR
```

Si un vídeo de DeoVR sale con el formato equivocado, el registro dice de dónde lo ha sacado (`DeoVR formato: ...`), y
`vr-play --deovr-dump URL` muestra lo que envía la web (los enlaces con token, ocultos). El formato siempre se puede
forzar con las listas **Proyección** y **Formato 3D**.

## Controles

| Mandos del visor | |
|---|---|
| Gatillo | reproducir / pausa |
| Stick derecha / izquierda | +10 s / −10 s (mantenido: repite) |
| Stick arriba / abajo | +1 min / −1 min |
| Agarre + stick arriba/abajo | pantalla más grande / más pequeña |
| Agarre + stick izq./der. | pantalla más cerca / más lejos |
| Clic del stick | centrar la pantalla delante (pulsación larga: sigue también la inclinación de la cabeza, para verla tumbado) |
| B / Y | siguiente pista de audio |
| A / X, menú | mostrar / ocultar la barra de información (tiempo, progreso) |
| Menú mantenido (1,5 s) | salir |

Ventana del escritorio (espejo del ojo izquierdo): todas las teclas de mpv (espacio, flechas, `#` audio,
`9`/`0` volumen, `q` salir...), y además **Ctrl+↑/↓** tamaño de pantalla, **Alt+↑/↓** distancia,
**F5** centrar, **F7** invertir ojos, **o** barra de información.

El tamaño y la distancia de la pantalla se recuerdan (`~/.config/vrvideoplayer/screen.conf`). El vídeo se pausa
solo cuando el visor sale de la sesión VR.

## WiVRn y SteamVR

- **WiVRn** (recomendado): arranca `wivrn-server`, abre WiVRn en el visor y conecta. Si el reproductor arranca
  antes de que se conecte el visor, espera. Sonido: elige la salida de audio **WiVRn** en el sonido del escritorio
  (o `vr-play --audio-device=...`) para oírlo en el visor.
- **SteamVR**: arranca SteamVR y usa `--runtime steamvr` (o pon SteamVR como runtime OpenXR activo).
- `--runtime` busca las rutas habituales (`/usr/share/openxr/1/openxr_wivrn.json`, el Flatpak de WiVRn,
  `~/.steam/steam/steamapps/common/SteamVR/steamxr_linux64.json`); también vale cualquier ruta a un `.json`.

## Problemas

- **Los dos ojos ven lo mismo** (sin 3D): el runtime no respeta las capas por ojo → elige **Dibujado propio** en
  la ventana, o `vr-play --mode render`.
- **Ojos al revés**: `--swap` (o **F7**).
- **Colores lavados**: el runtime no ofreció un formato sRGB (lo dice el registro).
- Registros: la ventana los muestra; el reproductor de Blu-ray también escribe `~/.cache/vrvideoplayer/vr-bd-*.log`.

## Blu-ray 3D

El mismo reproductor que en 3dvisionvideoplayer (ver [bluray/](bluray/)): encuentra solo el título 3D principal
(descarta las playlists señuelo anticopia), admite películas partidas en varios clips y discos cuya vista base es
el ojo derecho, audio original (DTS-HD MA, TrueHD...) sincronizado, y saltos rápidos con el índice del disco
(izq/der ±10 s, arriba/abajo ±1 min, RePág/AvPág ±10 min; también desde los mandos). Sin menús, extras ni
subtítulos PGS. El disco tiene que estar desencriptado.

## Windows (previsto)

El código está preparado para portarlo: C++17 + CMake, OpenXR sobre Vulkan, libmpv y GLFW existen en Windows,
la compartición OpenGL ⇄ Vulkan ya tiene su camino Win32 (`GL_EXT_memory_object_win32`), y el lanzador y la
ventana son Python/Tkinter. Falta: compilarlo en Windows (vcpkg), y tuberías con nombre y montaje de ISO en `vr-bd`.

## Créditos / licencia

GPLv3. El decodificador MVC (un edge264 ampliado, BSD-3, de Thibault Raffaillac) y los demultiplexores de Blu-ray
3D vienen de **SyLC 3D Player** de 5ymph0en1x (https://github.com/5ymph0en1x/SyLC, GPLv3), portados a Linux en
3dvisionvideoplayer.
