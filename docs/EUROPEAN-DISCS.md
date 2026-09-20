# European disc support

Version 0.1.0 recognizes SCES_023.80 and SCES_123.80 by the executable hashes listed in README.md. The runtime keeps its existing fixed 30 Hz simulation and English UI. This is data compatibility for the native port, not a claim that every PAL timing or language option is reproduced.

The main profiles map reference US Simulation addresses onto the European resident executable and overlays. Arcade UI code uses US Arcade addresses, so it has its own mapping. UI copies relocate overlay-local pointers and RAM text/state pointers; resident EXE words are not indiscriminately treated as pointers because packed palette values can resemble addresses. Gameplay loaders retain the original images and profile identity.

European Arcade's race overlay differs from European Simulation in only four words near the entry/exit routing (0x80011F88, 0x80012030, 0x80012034, 0x80012260). Both retain the Simulation-style wheel-effects tail and pad-slot logic. Their race text file is identical to the US Simulation text file; using the shorter US Arcade language stride breaks HUD strings.

Additional regional differences handled by the loaders:

- Publisher and warning images use `logo-scee.tim` and `notice_eu.tim`.
- Embedded text members are located by their gzip member names.
- European Arcade English menu/global text has different string offsets and block sizes. The offset-only maps in `arcade_eu_text.inc` and the UI profile contain no text payload. Course dimensions use the European metric format.
- GT menu resources use `gtmenu/eng` when `gtmenu/usa` is absent.
- The European music table has 21 entries. Its final marker extends two padding sectors beyond the ISO file length; playback is bounded by the actual file size.
- Movie/preview VLC tables have explicit mapped bases, checked for equality and through actual frame decoding. Invalid zero-length VLC entries are rejected instead of looping indefinitely.
- Attract playback loads the European demo resource by name.

Regenerate address tables after editing the YAML facts:

```powershell
.\build\gt2tool.exe gen-profile --build ArcadeUs11=db/arcade_us11_symbols.yaml --build SimEu=db/sim_eu_symbols.yaml --build ArcadeEu=db/arcade_eu_symbols.yaml --out src/gt2formats/exe_profiles.inc
.\build\gt2tool.exe gen-profile --build ArcadeEuUi=db/arcade_eu_ui_symbols.yaml --out src/gt2formats/arcade_eu_ui.inc
```

The menu/global string offset maps match the English blocks of the exact supported revisions. Each range stores reference offset, European offset and byte count; it copies the user's data at runtime. Changing to another revision requires reviewing these offsets and the address profiles, then rerunning disc-specific `gt2checks`, course/physics checks and UI/movie tests.