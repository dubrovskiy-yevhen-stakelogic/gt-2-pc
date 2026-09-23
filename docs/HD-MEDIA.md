# Offline HD media and PlayStation startup

Neural HD preparation runs on Windows before copying data to Quest. The PC and Quest game load the same prepared files. Neural upscaling never runs while playing. The [Linux installer](LINUX-INSTALL.md) supports original game assets and optional BIOS capture without Wine; neural HD preparation remains Windows-only.

## Installation

Windows 0.4.0 installers default to `Menus`: HD pictures, fonts and HUD are prepared for both Arcade and Simulation. The Simulation US v1.2 disc produces 461 pictures. `-HdMedia Original` skips preparation; `MenusAndMovies` additionally processes full-screen movies.

After installing the original discs, double-click `PREPARE-HD.bat` to choose pictures, pictures plus movies, and an optional local BIOS file. The script preserves existing saves and original disc data.

Use the source installer or the player installer with `-HdMedia Menus` for pictures, or `-HdMedia MenusAndMovies` for pictures and all three full-screen Arcade movies:

```powershell
./scripts/install.ps1 -DiscImage 'D:/Games/GT2.cue' -HdMedia MenusAndMovies -Bios 'D:/BIOS/scph5501.bin'
```

For an already installed game, without extracting the discs again:

```powershell
./scripts/prepare-hd.ps1 -Runtime ./runtime -BuildDir ./build_install -HdMedia MenusAndMovies -Bios 'D:/BIOS/scph5501.bin'
```

The player package uses `-BuildDir ./tools`. `-HdMedia Original` leaves original pictures/video in use on a fresh installation. Omitting `-Bios` skips firmware capture. Existing HD data is not removed when the option is omitted. Switch **VR menu → Graphics and performance → HD textures and media** off to use original disc resources. The choice is saved and shared by Arcade and Simulation. Pictures and UI change on resume; movies change on their next playback. The separate **Texture filtering** setting still controls smoothing/mipmaps.

The installer downloads a SHA-256-pinned Real-ESRGAN NCNN Vulkan package and uses its `realesrgan-x4plus` model. A Vulkan-capable Windows GPU is required for preparation. Videos can take a long time and tens of gigabytes of temporary disk space. Intermediate files remain in `.hd-work` for inspection and resuming completed movies. Completed indexed interface atlases are also reused on later runs. Keep free space for both the old pack and its replacement. Existing packs are backed up before replacement, and saves/disc bytes are not modified.

## Coverage

- The two GT2 startup pictures, title background and GT Mode common backgrounds are reconstructed from the selected US/European disc and upscaled.
- Full-screen Arcade intro and endings (movie IDs 24, 25, 26) are decoded with GT2's native movie decoder, upscaled and repacked with the original audio. Video reconstruction is bounded against a bicubic enlargement of each original frame: neural changes are limited to 10 colour levels per channel. This trades some sharpness for fewer invented details and avoids mixing unrelated frames at cuts. Previously prepared neural frames are reused; older unbounded movie containers are rebuilt.
- Pictures/frames normally use 4x dimensions. Sources exceeding 512 pixels in either dimension are reduced after neural processing to stay within 2048x2048; 640-pixel pictures and endings therefore use 2x dimensions.
- Menu fonts/buttons, GT Mode page-specific captions, and race HUD sheets/maps are prepared as 4x indexed atlases using edge-directed Scale4x. Both 4-bit and 8-bit source pages are supported. The runtime retains native palettes, fades and transparency. These are the fallback atlases; selected shared menu and HUD sheets also receive the separate contour pass described below.
- World/car textures and the small course-selection movie previews retain their original assets. GT Mode pages with transparent car-view cutouts keep their original background rendering to preserve those holes. This is not a complete HD replacement of every game asset.
- AI processing can smooth detail or alter lettering. It cannot recover detail absent from the source.

## Original PlayStation sequence

This feature is optional. The game, ordinary installation and HD game media work without a BIOS; without a prepared startup file, console startup playback is skipped. Capturing the original console startup requires a user-supplied 512 KiB BIOS dump. The white Sony screen and its sound come from firmware, not the GT2 video files. `gt2bootcapture` uses a separately downloaded software Beetle PSX libretro core on the PC to capture both BIOS screens and stop before the game's publisher screen. The original `startup.gtm` is retained. HD preparation also creates `startup-hd.gtm`, normally 1280x960, with the same frame count, cadence and audio. Identical held frames are upscaled once and reused. The saved HD switch selects either version before the Quest disc selector; on PC it plays before the game's startup pictures. The complete firmware sequence is unskippable. The ordinary game intro still uses its own skip controls.

No firmware, captured intro, disc data, emulator core or generated HD artwork is included in source/player packages. The core is only an offline preparation dependency. Its current upstream download is a nightly URL protected by an exact checksum: if upstream replaces it, capture stops rather than executing changed bytes. A local trusted software core can be supplied with `-CaptureCore`. Regional BIOS/disc combinations that cannot show both complete screens are rejected; select matching firmware.

## Storage and playback

Each disc has `hd/profile.txt`, `hd/images/*.png`, `hd/ui/*.png`, `hd/fonts/*.png`, `hd/movies/*.gtm` and `hd/manifest.json`. The profile binds the pack to the disc's executable identity. `startup.gtm` and optional `startup-hd.gtm` are also placed at the runtime root for the disc selector. Quest installation transfers these optional paths with the game data.

G2MEDIA1 stores indexed JPEG frames and original decoded 44.1 kHz stereo PCM. Runtime playback has one decode worker and a three-frame queue, with the audio sample clock controlling video timing. It is CPU JPEG playback, not Android hardware H.264 decoding. All video frames are not held in RAM; audio is loaded per movie. Invalid/missing HD files fall back to native media. Full-size Quest playback still needs headset timing and visual acceptance before shipping.

The **Smooth + mipmaps** texture setting filters distant decoded world textures, including transparent fences. Mips are generated on the GPU when texture/palette content changes. They are separate from the offline HD pack; see [mipmap implementation and limits](MIPMAPS.md).

Indexed interface packs retain the source palette and can still show the original shading or coarse details. The four-times-larger contours are not a promise of newly drawn typography. Runtime keeps up to 16 mixed interface atlas slots within the same 32 MiB GPU budget. Pages load on first use. Contour font pages are keyed by source indices and palette address; fades still use the live game palette.

Contour text bilinearly samples the precomputed HD subpixels, with alpha blending for opaque UI and the original STP blending rules for translucent elements. Native UI pages without an HD replacement are filtered too. The world mip cache excludes 2D menus/HUD. The HD title picture is loaded once per disc/view, independently of animated preview VRAM uploads.

Course-map sprites are enlarged independently inside their 96x96 source rectangle, so surrounding atlas indices cannot introduce bright corner pixels. Their narrow map lines retain a half-source-pixel filter footprint. The preparation script upgrades cached map atlases without repeating neural video processing.

### Contour text atlases

HD preparation applies an offline CPU adaptation of Hyllian's MIT xBR-lv3 contour rules to title-list lettering, shared Arcade menu/font sheets, GT Mode common fonts/items, and race HUD gauge/number sheets. This replaces the former neural text pass, which could change letter shapes. Both transparent and fully opaque palettes are processed, including Load Guest Garage, Road Race and Time Trial. Pictures and movies retain their separate Real-ESRGAN path.

Each HD texel stores two original palette indices and a blend weight. Runtime filtering spans adjacent HD subpixels, retaining original colours, fades and transparency without a source-sized blur. The xBR neighbourhood search runs only during installation. The tightly packed nine-pixel race-caption font and page-specific GT Mode artwork retain their indexed fallback. Original low-resolution shading and small glyph details can remain visible; this is contour smoothing, not replacement typography.

The files are `hd/fonts/<page>-<palette>.png` and `hd/fonts/index.txt`, with format `palette-contours4x-v1`. Preparation uses a separate versioned cache; old neural packs are rejected by the runtime and fall back to indexed UI until HD preparation is rerun. Generated atlases are not supplied in the public source folder.
