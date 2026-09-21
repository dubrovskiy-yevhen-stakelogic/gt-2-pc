# Validation — public release 0.1.0

Checks performed on 2026-09-21. Build verification and automated PC tests are separate from headset acceptance.

## Completed checks

- Windows Release build from a new build directory using the source install target; all four CTest suites passed (core regression, VR rig, driving controls, Quest career features).
- Disc-specific regression checks: 55 on each Arcade disc and 52 on each Simulation disc. These include publisher/warning decoding, music pause/resume sample equality, Arcade menu text, matching movie/preview AC tables and frame decoding, Simulation menu/career loading and regional attract-resource resolution.
- European Arcade and Simulation were installed from the user's complete raw BIN images into an isolated test directory. The installer extracted 11,292 and 11,578 assets respectively, verified copied disc SHA-256, scanned 126 courses per disc and passed the six-car physics self-test. Additional menu/media checks passed on the installed loose-data paths.
- Automated Windows screenshots checked European Arcade title, selection menus/car reflections and a race start; European Simulation title, GT Mode home and the first licence test. The European intro decoded all 4,374 frames; a course preview decoded all 419 frames without errors.
- Android ARM64 debug and release builds completed. Public APK metadata is versionName 0.1.0, versionCode 13, non-debuggable. APK signature and alignment are checked by the release scripts. Only the game and OpenXR native libraries are included; there is no disc-data assets directory.
- Positional billboard tests cover viewer position, height independence and the coincident-position fallback. The shared stereo viewer position replaces head-right orientation for tree billboards.
- PowerShell scripts were parsed, and the source audit rejects game payloads, build outputs, keys and agent instruction files. Player packaging validates every file hash and rechecks every ZIP entry against the assembled folder.

The published source is a separate ordinary folder/repository. Private retail images, captures, profiling logs, older internal planning documents and signing keys are not part of it.

## Performance evidence

The preserved user session at 175% showed CPU scene preparation around 0.4–0.8 ms, with GPU averages commonly around 9–12 ms and higher peaks. The GPU remains the main limit in that recording. A 90 FPS frame budget is 11.11 ms including work beyond the game's measured GPU region. Selected refresh rate and average FPS do not describe isolated long-frame stalls.

Holding the virtual wheel now uses a fixed closed-fist pose, avoiding pressure-driven mesh updates while held. No new measured GPU speedup or sustained 175%/90 FPS claim is made for 0.1.0. Existing texture caching, staged uploads, mirror clipping and foveation improvements remain in place. Settings are retained; the release does not force a lower resolution onto existing installations.

## Headset acceptance and remaining coverage

The release tester accepted the latest Quest build, including the tree and held-fist fixes. Its selected settings are now the initial Quest preset (150%, 80 Hz, MSAA 2x, medium foveation). The checklist below remains useful for regression checks and broader disc coverage; acceptance is not a claim that every career branch has been played through.

- In the first braking licence, stop moving and rotate the head: trees should retain their orientation. Inspect both eyes while driving past them.
- Grab the wheel with each hand. Both fists should remain closed while alternately applying/releasing accelerator and brake; releasing a grip should restore ordinary finger poses.
- Check Motion steering in both directions with the grip held, independently of the accelerator.
- Pause a rally race: timer, music and road sound should all stop. Check the HUD/mirror, saved visibility controls and settings after restart.
- Install/test both European discs through the player installer on a compatible public-signature installation; complete races, licence/career progress, saves and cheats. Automated smoke tests do not establish every branch of a full playthrough.

The published 0.1.0 release did not include the PlayStation BIOS animation/audio; the optional HD preview below adds it. Wider VR views can expose missing original geometry; trees are still flat billboards. Two-player VR uses the theatre screen. Loading/start stalls and sustained high-resolution performance remain release limitations.
A compatible debug-signed 0.1.0 test update was installed on the connected Quest. Its installed APK hash matched the local file, and all six existing settings/backup files retained their hashes. The game was not launched and headset disc data was not copied. This verifies deployment, not the remaining headset behavior checks.

## HD media preview validation (2026-09-21)

Windows Release and Quest ARM64 debug builds passed. CTest passed all five suites, including new G2MEDIA round-trip, invalid-header/index bounds, truncation and profile/path checks. The offline installer was exercised end to end for Arcade pictures and BIOS capture, then rerun to verify replacement preserves the captured startup. Its five-file media manifest matched the generated content.

PC captures verified both BIOS screens, skip to subsequent screens, the enlarged notice/title background and a 16-frame 1280x768 movie sample. Native image extraction also completed for US Simulation (461 PNGs) and European Arcade. The 1029-frame BIOS capture contains original stereo PCM and stops before the publisher screen.

The preview APK and Arcade startup/picture assets were installed on the connected Quest without uninstalling the app. Runtime file read access and transferred hashes were checked. Headset playback, audio timing, full-length upscaled movies and full Simulation HD artwork are not yet accepted. That first preview did not enable mipmaps; the follow-up below supersedes it. The published 0.1.0 archive was left unchanged.

## HD interface, full movie and mipmaps follow-up (2026-09-21)

- Windows Release and Quest ARM64 builds pass. All five CTest suites pass, including indexed 4/8-bit UI scaling and palette-preserving cache checks.
- PC playback confirms the complete 4649-frame 1280x768 / 30 FPS intro is selected, with the original 13,665,120 interleaved PCM samples retained. Native dimensions were 320x192. Movie skip remains available.
- Enter, Start and Escape were injected during BIOS playback; captures at presentation frames 400 and 900 still show the white Sony and black PlayStation screens. Firmware playback reports skip disabled.
- Prepared 4x assets load in Arcade/title, rally HUD and GT Mode pages, including captions stored in each page's artwork. These are edge-directed indexed enlargements, not neural replacement fonts. Small course preview movies and world/car artwork are unchanged.
- Vulkan validation passes dirty VRAM reuploads, cache mip regeneration, multiview, foveation and 1/2/4x MSAA transitions. The test includes opaque/STP texture layers and the separate hand texture.
- Quest 3 Adreno 740 offscreen replay, same rally capture at 150%, 2x MSAA, medium foveation: warmed reverse-order comparison averaged 6.410 ms without mips and 6.437 ms with mips (360 measured frames each). Earlier runs varied with GPU clocks. This is a static captured GPU workload; it does not establish gameplay FPS, frame pacing or temporal fence quality in the headset.

The compatible debug preview is installed separately from the published archive. Original disc bytes and saved settings are retained. Full-length headset audio/video synchronisation, UI appearance and moving-fence shimmer still need player acceptance. The public 0.1.0 archive remains unchanged.

## HD menu regression follow-up

The HD title was decoded and uploaded on every TitleView VRAM change, including course-preview frames and car switches. It now loads once per view/disc generation. In the isolated `gt2renderbench --menu-upload` replay on Quest 3, the repeated-decode path took 169.657 ms/update; the cached path took 0.083 ms/update. A captured course-menu render at its 1280x960 cinema resolution took 4.887 ms/frame (CPU plus GPU, offscreen). These are component measurements, not compositor FPS or headset acceptance.

The world decoded-texture/mipmap cache now excludes 2D draws. HD UI lookups use the installed index to avoid filesystem probes for animated pages without replacements. UI filtering resolves palette colour and transparency before interpolation, including native fallback pages; opaque UI has its own alpha-coverage blend pipeline. World mip filtering is unchanged.

All five host checks passed. Vulkan validation passed for the course menu, stereo race replay, and sample-count/foveation transitions. The movie packer also checks bounded reconstruction and rejects invalid dimensions. Video now limits neural deviations against bicubic source reconstruction; this deliberately reduces sharpness and requires visual acceptance during playback, particularly at scene cuts and fast motion.

## HUD text and minimap follow-up

A same-frame Tahiti Road capture confirmed that the former filter left source-pixel stair steps visible. Text reconstruction now spans original texels instead of enlarged atlas subpixels. Desktop before/after captures show the stronger smoothing on both HUD labels and Start Game. This is a softer bitmap reconstruction, not a new font.

The native 96x96 minimap had no isolated bright component, but whole-page Scale4x introduced a bright pixel at the sprite's top-right corner from neighbouring atlas indices. The map preparation step now scales its bounded region independently. All 119 Arcade and 120 Simulation map entries were rebuilt; the inspected Tahiti Road atlas no longer has the corner pixel. A host regression check reproduces the old corner leak and verifies that the isolated region remains unchanged by neighbouring indices. Five host suites passed, as did Vulkan validation for the live title and race captures. In-headset readability remains subject to player review.

## Shared VR settings and HD selection

Five host suites pass, including migration from the newest disc overlay, shared VR values across both discs, preservation of original per-disc audio settings, exclusion of cheat/progress keys, idempotent migration, and HD generation invalidation. The native PlayStation sequence remains available when HD is disabled or its enhanced container is absent. Windows Release and the Quest ARM64 APK build successfully.

The final four-tap text filter measured 5.737 ms average GPU time (5.741 ms p95) versus 5.721 ms (5.726 ms p95) for the previous filter on the same saved Tahiti Road workload: Quest 3 / Adreno 740, 150%, 2x MSAA, balanced foveation, 180 measured frames. This component test does not establish gameplay FPS. Desktop captures show softer HUD/title lettering, and disabling HD loads the native title with no HD UI pages.

The enhanced PlayStation sequence is 1280x960 versus the original 640x480. Both contain 1029 frames at 59940/1000 FPS and byte-identical PCM audio (1,515,850 interleaved samples). The ordinary GT2 intro movie is unchanged in this update. The updated preview APK, isolated map atlases and enhanced startup were installed without launching the game; device SHA-256 checks and application-UID file access passed. All four existing per-disc settings files retained their hashes. Headset visual acceptance and cross-disc menu testing remain for the player.

## Reconstructed menu text and optional movie hint

The broad source-pixel text blur was rejected during headset review. The title-list and selected shared menu/HUD sheets now receive a separate offline Real-ESRGAN pass. Runtime samples precomputed HD contours, retaining palette fades through a pair of original palette indices and a blend weight. The nine-pixel race-caption font stays on its original indexed fallback: neural versions changed glyphs. This update does not claim that every menu or HUD font is reconstructed.

Final preparation accepted 443 Arcade and 269 Simulation atlas/palette pairs. A source comparison rejected 27 and 41 additional low-confidence pairs. A source-derived transparency mask limits reconstruction to within one native pixel of the original foreground, avoiding faint rectangular background noise. Tests cover transparent versus opaque black, intermediate palette weights, quality rejection and the foreground-mask boundary. All five host suites pass; Windows Release and the compatible Quest ARM64 APK build successfully. Vulkan validation is clean for the title and race captures.

The single-HD-texel text path averaged 5.516 ms GPU time (5.915 ms p95) on a saved Quest 3 workload at 150%, 2x MSAA and balanced foveation. The previous released-preview shader averaged 5.736 ms on its matching earlier capture. This is a component comparison with changing text resources and GPU clocks, not a sustained gameplay FPS guarantee. The UI GPU allocation stays at 32 MiB.

HUD page 2 includes **Movie skip hint**. Its saved value is shared by Arcade and Simulation; it changes the prompt only, leaving movie-skip input and the unskippable firmware sequence unchanged. Final in-headset text readability and the saved toggle still need player review.

The compatible preview APK and both text packs were installed without launching the game. Device SHA-256 values matched the APK and all 714 font files; application-UID access passed. All seven existing settings/backup files retained their hashes. The published 0.1.0 archive and GT2 intro movie were not replaced.

## Android text-index compatibility fix

The preceding text preview was not accepted on the headset. The actual Android game log loaded only legacy indexed UI pages: the Windows-generated `fonts/index.txt` used CRLF, and Android retained the carriage return when comparing its header. Windows text-mode reads had hidden this platform difference during desktop captures.

The shared index reader now accepts LF and CRLF whitespace, validates the format and key shape, and logs the available reconstructed-font count. Regression tests exercise both newline formats in memory without Windows text-mode conversion, plus unsupported headers and malformed keys. All five host suites pass, and Windows/Quest builds pass.

The ARM64 render benchmark now supports `GT2_BENCH_HD_ROOT` to apply the real installed pack loader to native captured vertices/VRAM before drawing. On Quest 3, with the original CRLF files left unchanged, it found 443 font/palette pairs, loaded both title-list keys and reconstructed 60 vertices. The Arcade Mode screen loaded its heading and panel keys and reconstructed 138 vertices. Both saved frames were rendered on the headset GPU without starting the game. This verifies Android asset selection/rendering, not full compositor or gameplay acceptance; native fallback captions remain outside the reconstructed set.

The compatible loader-fix APK was installed with saved preferences preserved. HD assets, movies and the public 0.1.0 archive were unchanged. The player still needs to review both menu screens in the headset.

## Source-preserving menu contours

Player review rejected the neural title lettering because it changed the shape of the letter t. The new offline font path uses an MIT xBR-lv3 contour adaptation over original palette indices. Runtime bilinear sampling is limited to the enlarged subpixels. Fully opaque palettes are now exported too: the previous transparent-only test had skipped Load Guest Garage, Road Race and Time Trial. The versioned `palette-contours4x-v1` header excludes old neural atlases; pictures and movies keep their existing preparation.

The new packs contain 976 Arcade and 465 Simulation atlas/palette pairs. Windows Vulkan captures confirm that all three reported captions load their new replacements and preserve their original text. Original bitmap shading remains visible; this is not replacement typography. Five host suites pass, including straight-edge preservation, diagonal coverage and malformed index handling. Windows Release, Quest ARM64 and the Android render benchmark build successfully.

The installed-pack loader was exercised on the Quest 3 GPU against native title, Arcade Mode and Game Select captures: 60, 144 and 174 vertices respectively used the contour atlases. At 100%, 2x MSAA, 180 measured frames, average component GPU times were 4.317, 4.454 and 4.174 ms; p95 values were 4.632, 4.458 and 4.471 ms. These offscreen menu checks do not establish sustained gameplay performance or headset visual acceptance.

The compatible preview APK and all 1,443 font files were installed and SHA-256 verified. Application-UID access passed. All seven existing settings/backup files retained their hashes; the game was not launched. Movies, saves and the published 0.1.0 ZIP were not replaced. Review Start Game, Load Guest Garage and Road Race on the headset before accepting this preview.

## Rally first-frame stack failure

A Quest crash report from 2026-09-21 11:37:36 shows Tahiti Dirt Route 3 loading successfully, followed by SIGSEGV during the first stereo submission. The captured library BuildId matches the installed preview. The faulting VR-driver instruction is `str xzr, [sp, #0x1b0]`; the recorded fault address equals SP + 0x1b0, indicating a failed stack write rather than a texture lookup. The full stack mapping was not available, so device retesting remains necessary to confirm the fix.

`RunArcadeGhostSession` kept a `MenuMusic` object for the later results screen. Its embedded mixer included 512 KiB of sample RAM, and the compiler reserved that storage throughout the race call, even before constructing the post-race music. This ownership also exists in the pre-HD 0.1.0 source. The mixer now allocates on the heap when music opens; close/reopen/failure paths stop the audio device before replacing the stream. The ARM64 function frame dropped from 568,784 bytes to 37,312 bytes, including saved registers. A compile-time size limit prevents large audio buffers from returning to this object's stack footprint. First stereo submission logs available Android thread stack space for follow-up diagnosis.

Windows Release and Quest ARM64 builds passed, along with all five host test suites. New checks cover the small music object, unopened playback/stop, failed opening and retry. A scripted desktop Arcade rally with HD disabled reached the racing state on Tahiti Dirt Route 3 without Vulkan validation errors; it was stopped after observation. This does not reproduce the Quest driver's stack usage.

The compatible preview APK was installed with its SHA-256 verified; saved data/settings hashes were unchanged. Game assets, text processing and the published 0.1.0 package were not modified by this fix. Headset checks pending: enter Rally, drive, pause/exit, then Try Again; also enter Time Trial, which shares the ghost-session path.

## Car shadows, Midfield scenery and European audio diagnostics

The player confirmed the rally fix. The next geometry checks use all car and track assets from both US and both European discs. Each disc contains 2,220 car-model files; 74 files (37 model IDs including day/night variants and auxiliary models) have different body and shadow scale exponents. Shadow vertices now use the exponent in their own header at +0x18, matching the original shadow transform, rather than the body LOD exponent. The test independently walks the packed headers and checks every emitted shadow corner. Runtime race shadows also use the ground-following pose, separate from suspension/body movement, in main views, mirrors and split screen. A banked-ground fixture verifies placement.

Midfield's full-distance failure reproduced with original resources: distant scenery includes coarse copies of the road and a closed tunnel surface. Their projected surfaces covered cars and the detailed tunnel when extended visibility enabled both representations. The initial size-based visibility restriction was rejected and removed. Object size no longer reduces the selected draw distance.

The replacement index matches scenery corners to detailed course vertices or triangle surfaces in world space, within the source formats' quantisation error. Three matching corners identify a coarse surface patch; a fourth coarse-quad corner can be displaced by reduced subdivision. Some distant copies have offset origins: Midfield models 124 and 125 put extra asphalt above the starting straight. For models with an empty near LOD, registration accepts a common translation within one metre plus quantisation only when at least six vertices and 60% of the mesh agree. This does not enlarge the individual surface-matching tolerance or apply to independent near-visible scenery. Recognised shared boundary vertices are joined to their detailed counterparts so surviving neighbours do not leave quantisation-sized cracks.

An opaque background polygon is replaced only when all detailed chunks identified for that patch are drawn. Unmatched polygons, billboards and independent mountains retain their full distance. Filtered meshes are prepared once at course load and batched for the fully detailed case; partial coverage retains the coarse fallback. Empty near-LOD sentinels are skipped when selecting maximum visible detail, while original-distance selection retains them. Radius extension also promotes existing lights-only render-list entries to geometry inside the selected radius, rather than only adding absent chunks. The scenery bit-30 name/documentation was corrected to farthest-corner sorting; JSON/binary flag encoding is unchanged.

`gt2assetchecks <disc1> [disc2 ...]` scans all models and course variants. The US Arcade disc has 125 course files; the other three discs have 126 each. Across the four discs, 3,218,508 chunk-centre camera samples check full-range scenery independently of masks and size. The scans identify 36,123 coarse surface patches and verify that detailed geometry must be present before replacement. They also check geometry coverage within 500 m from every chunk centre, including previously lights-only entries. Midfield regressions explicitly cover both shifted straight copies, the duplicate road, tunnel cap and independent mountain. Synthetic checks cover subdivision-interior anchors, quantisation, coherent origin offsets, exact shared boundary positions, unrelated elevated/near-visible geometry and fallback when detail is absent. The current scan passes 8,438,207 checks. This is a structural asset regression scan, not visual acceptance of every course.

The user confirmed the tunnel entrance fix but reported remaining starting-straight overlap and distant chevron loss at 500 m. Driver-view captures reproduced the overlap: a ray hit model 124's asphalt ahead of the detailed road; after registration, the road and its grid markings are exposed. The second shifted copy likewise stopped clipping other cars. Replaying the same distant-chevron capture with mipmaps disabled isolated a separate filtering problem: isotropic minification erased the arrows. Bounded four-tap anisotropic filtering now preserves the pattern while retaining mipmaps and atlas-region limits. The extra taps apply only to elongated minification footprints; Quest frame-time impact still requires headset testing.

Six host suites, Windows and Quest builds pass. Scripted 500 m driver-view captures and desktop stereo replay check the grid, straight, chevrons, hairpin and open tunnel interior; Vulkan validation reports no errors. The compatible preview APK was installed and its SHA-256 verified (`4ba3aee98d8aa3dd5e9e706785a291aaa9e94ea3379b1fc84e5712f65838b323`). US Arcade assets, including the existing HD assets, were restored from the saved device directory; European test data remain in a separate backup. Settings/save hashes were preserved, and the application was not launched after installation. Headset checks remain: the initial asphalt height, approaching chevrons, their hillside intersection, the sky seam near the tunnel, and frame pacing. The published 0.1.0 ZIP is unchanged.

The supplied video's race begins around 4:40 on Midfield. SCES-02380's XA table and channel routing were inspected, all 21 tracks were decoded, and race track 3 was compared with FFmpeg's independent XA decoder. This did not establish a defective source decoder. Music streaming now reads 32 sectors per disk operation and reuses decoding buffers instead of seeking/allocating for each sector on the callback. All 42 fifteen-second WAV exports from US/European Arcade are byte-identical before and after the change. Volume and sample reconstruction are unchanged. Track/volume and Android underrun diagnostics were added. The user subsequently confirmed normal race audio after testing the European Arcade data on Quest.

The shared Graphics menu now includes **PlayStation intro**, default ON, applied on the next launch. OFF bypasses the whole console startup; enabled startup remains unskippable. The preview APK was installed without starting the game. Existing saved settings and progress retained their hashes; game data and the published 0.1.0 ZIP were not replaced.


## Full-course detail and Midfield texture seam (2026-09-21)

The next headset report confirmed only the near starting-grid improvement; distant asphalt, grass crossing chevrons and the cliff seam remained at 500 m. Fixed-camera desktop captures reproduced the hillside overlap. Entire-course mode now selects each scenery list's authored first entry, including empty near entries for distant-only geometry, while drawing every detailed road chunk. Previously, skipping empty entries resurrected lifted road and hillside fallback meshes. Independent scenery remains at full range. Entire course is the new Quest first-launch distance; other defaults and existing preferences remain unchanged.

The cliff seam was separately reproduced with nearest filtering, without mipmaps and with both texture paths. An untextured draw-ID replay covered the seam continuously. The source cliff tile contained 41 transparent texels in its first row and three in its second row. Course loading now insets a fringe of at most two texels only on a shared opaque course edge, for tiles with less than 2% transparency confined to that fringe. Free silhouettes, interior cutouts, translucent polygons, billboards and source disc data are unchanged. This is static UV preparation, with no extra shader samples or per-frame work.

Six host suites passed. The four US/European Arcade/Simulation corpus scans passed 8,441,841 checks, including all course instances and texture-seam invariants. Same-camera desktop Vulkan captures show the clear starting straight, exposed chevrons and closed cliff seam. This is desktop reproduction and structural corpus coverage, not headset acceptance or visual inspection of every track. Quest frame pacing with full-course geometry still requires headset testing.


## Release 0.2.0

The full-course Midfield geometry and cliff-seam corrections above were accepted in the user's subsequent headset test. Release 0.2.0 changes the Quest first-launch refresh preference from 80 to 72 Hz; existing saved refresh preferences remain unchanged. No new performance guarantee is implied.

Windows Release and Android Release builds passed, as did all six host CTest suites. The unchanged geometry implementation retains the four-disc corpus result of 8,441,841 checks. PowerShell sources parse successfully. The ARM64 public APK is non-debuggable, versionName 0.2.0 / versionCode 14, and its signing certificate matches 0.1.0 (SHA-256 `69d9f4ddc4fa8706f6df7ab9bce2288c3c5bbc3e87ed08f1855cdcf5052ad4f9`). The APK SHA-256 is `cfb1e99bc351010134ee1fe7cc8fb2745b7d55b6426a81c5a7b3f9c5db35a327`.

Documentation attribution tags referring to development agents and tool/session narratives were removed; technical findings, evidence limitations and third-party notices were retained. Player and source packaging exclude game data, BIOS, startup recordings, private keys and temporary diagnostics. The optional BIOS-capture workflow is documented separately from normal installation, which needs no BIOS. The public release build is packaged without replacing the differently signed development installation on the headset.


## Linux installer and optional BIOS choice (0.2.0)

The Linux installer uses a Python-standard-library ISO9660/GTFS reader, without Wine or ISO conversion. Fresh native `gt2install extract` outputs for all four supported disc profiles were compared against it: 45,066 asset paths and payloads match byte-for-byte. Concatenated gzip entries consume only the first member, matching the native reader, and CRC validation remains enabled. `tests/check_linux_disc_parity.py` repeats this comparison against fresh reference directories.

Eleven installer regression tests pass, covering unsafe paths, CUE handling, truncated input, profile-table parity, gzip member/CRC behavior, package corruption, save retention, publish rollback, unauthorized devices, incompatible signing and the ADB verification flow. PowerShell sources parse, and the Linux shell entry point passes Bash syntax validation. The portable ctypes capture host captured both complete BIOS screens through the Windows software core: 1,029 frames at 59.94 Hz with stereo audio, accepted by the native G2MEDIA decoder. Neither firmware nor this recording is packaged. Linux dependencies use pinned downloads; the optional software core depends only on standard glibc libraries.

The game APK is unchanged by this installer update: version 0.2.0 / code 14 and the public signing identity are retained. Linux kernel execution and a physical Steam Deck USB install have not been tested on this Windows host; portable reader/capture checks and mocked ADB are not substitutes for that hardware check. Neural HD preparation remains Windows-only.
