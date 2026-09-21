# Состояние reverse engineering / декомпиляции Gran Turismo 2 (и GT1)

Дата исследования: 2026-09-17. Все ссылки ниже реально открывались или были получены в выдаче поиска при подготовке этой заметки. Там, где факт взят только из сниппета поиска (а не с открытой страницы), это помечено.


---

## 1. Главный вывод

Идея "безумная" уже не уникальна: к сентябрю 2026 существует **минимум четыре публичных проекта по коду GT2**, и один из них (OpenGTPS1) уже играбелен на Windows с собственным 3D-рендером. При этом:

- **Debug-символов для GT1/GT2 не найдено нигде.** Ни в retail, ни в демо/preview/review-билдах. Это прямо говорит pez2k (автор GT2 Plus), это подтверждает каталог RetroReversing, и это подтверждает preface на GT Modding Hub (единственные символы во всей серии - прототип GTHD для PS3, близкий к GT4).
- Все существующие проекты берут за основу **NTSC-U Simulation v1.2 (SCUS-94488, Greatest Hits)** - ровно тот образ, который есть у пользователя.
- Полноценной matching-декомпиляции нет: gt2-reversing - это splat-каркас + ~1200 именованных символов main EXE + единичные декомпилированные функции. Физика, AI, рендер гонки не декомпилированы никем (в публичном виде).
- Для VR ни один из проектов ничего не делает; архитектура OpenGTPS1 прямо заявляет отсутствие free camera.

---

## 2. Существующие проекты по коду GT2

### 2.1 ginryuoku/gt2-reversing - splat-based decomp (фундамент)

<https://github.com/ginryuoku/gt2-reversing>

- Цель: matching-декомпиляция GT2 PS1. База: `SCUS_944.88` + `GT2.OVL` с **US 1.2 (Greatest Hits) Simulation disc**; в README сказано, что эта версия "почти идентична PAL-релизу".
- Toolchain: [splat](https://github.com/ethteck/splat), старые GCC из decompals/old-gcc, mipsel binutils, maspsx, Ninja + python-генератор (`build_gen.py`, `build_post.py`). Submodule: [CookiePLMonster/GTModTools](https://github.com/CookiePLMonster/GTModTools) (`ovl.py`, `psexe.py`). Только Linux/WSL2.
- Лицензия: CC0-1.0 (инструменты - свои лицензии).
- Статистика: 184 commits, 34 stars. **Последние коммиты - сентябрь 2024** (самый свежий: "added all mangled symbols for ScreenViewLoop base class", 14 Sep 2024). Проект фактически заморожен.
- Сборка выдаёт 7 файлов: `scus_944.88` и `gt2_01..06.exe`; обратно в `GT2.OVL` оверлеи пока не пересобираются (проблема gzip + timestamps + ограничение: самый большой overlay не должен налезать на `start()` игры).
- Инфраструктура предусматривает версии: jpbeta, jp10, jp11, us10, us11, us12, eubeta, eu10 - но рабочая только us12 simdisk (есть папка `config/gt2_eu10_simdisk`).
- Конфиги: `config/gt2_us12_simdisk/` - `scus_944.88.yaml`, `gt2_01..06.yaml`, `mainexe_symbol_addrs.txt`, `ovr1..6_symbol_addrs.txt`, `*_undefined_syms.txt`.

Что из этого следует про layout (по yaml-конфигам; детали пересказаны моделью-суммаризатором, перед использованием сверить с файлами):

| Часть | File offset | VRAM | Комментарий |
|---|---|---|---|
| `ovr0` (внутри SCUS_944.88) | 0x800 | 0x80010000 | init, GTFS/VOL loader, музыка - резидентная часть в "оверлейной" области |
| `gt2main` (внутри SCUS_944.88) | 0x4DE00 | 0x8005D600 | основной код: `gt2_main` @ 0x8005D6E0, `gt2_load_overlay_default` @ 0x8005DA3C |
| `gt2_01..06` (из GT2.OVL) | - | 0x80010000 | все шесть оверлеев грузятся по одному адресу, взаимоисключающие |

- `mainexe_symbol_addrs.txt`: ~1200 символов в диапазоне 0x80010000-0x801E35F0. Именование в основном "полу-осмысленное": `gt2_main_race_func1..49`, `gt2_main_arcade_func0..61`, `gt2_main_shared_racemenu_func*`, но есть и содержательные: `gt2_ovr0_vol_gtfs_init`, `gt2_main_gtmode_get_part`, `gt2_main_shared_arcaderace_load_car_parts`, `gt2_fxpoint_multi8/12/16` (fixed-point умножения 8/12/16 бит дробной части), `gt2_main_gzip_decompress*`, `gt2_main_spu_*`, плюс идентифицированные PsyQ-функции (`CdInit`, `CdControl*`, `PadInitDirect`, `InitCARD`, `VSync`, ...).
- **В GT2 есть C++**: mangled-имена `__11psxViewLoop`, `__14ScreenViewLoop`, `begin__14ScreenViewLoop`, type_info, `__builtin_delete`; в build добавлен `-fno-exceptions`. Заголовки классов добавлены "thanks to Nenkai" - т.е. имена классов, вероятно, перенесены из более поздних GT (это моя интерпретация, в репо не объяснено).
- Оверлеи проименованы слабо: в `ovr1_symbol_addrs.txt` всего ~9 символов, в `gt2_04.yaml` декомпилированы только `memset_caller`, `max3`, `min3`.

### 2.2 vairacing-tech/GT2-DECOMP - активный форк-продолжение

<https://github.com/vairacing-tech/GT2-DECOMP>

- "Independent GT2 Simulation Mode decompilation, based on ginryuoku/gt2-reversing; future Windows/Android native runtime."
- База: NTSC-U 1.2 (SCUS_944.88). 212 commits, 0 stars, последние коммиты **5-7 июня 2026** ("Decompile ovr1 camera offset selector", "Decompile ovr1 menu state transition", "Decompile ovr5 license load wrapper" и т.п.).
- Native runtime существует только как архитектура (не реализован). Есть `ingest_disc.py` (валидация пользовательского диска), `docs/DECOMP_BUILD.md`.
- Лицензия: заявлена open-source с атрибуцией upstream; точное название лицензии я не увидел.

### 2.3 GTTeancum/OpenGTPS1 - играбельный static-recomp порт с собственным рендером

<https://github.com/GTTeancum/OpenGTPS1>

- Экспериментальный Windows x64 порт **US Simulation (SCUS-94488, rev 2 = v1.2) + US Arcade (SCUS-94455)**, опционально подмешивает контент GT1 (SCUS-94194): SSR11, 13 эксклюзивных машин.
- Технология: static recompilation через [RecompOne](https://github.com/BlackLabelHQ/RecompOne) (MIT, 524 stars, генерирует C#, нужен .NET SDK 10) + собственный D3D11 "modern world renderer" (`native/` - portable C++17).
- Состояние (v0.9b, 10 Sep 2026): полные гонки, чемпионаты, replays, memory card, XA + внешние OGG, геймпад/клавиатура, 59.94/60 Hz, perspective-correct texturing, max LOD, полная draw distance. 71 commits, 63 stars, первый видимый коммит 21 Aug 2026 - проекту меньше месяца, в README отмечен "hiatus".
- **Лицензии на собственный код нет** ("No license currently granted") - код можно читать, но нельзя легально переиспользовать.
- Документация: `docs/PORT_PLAN.md`, `docs/MODERN_RENDERER.md`, `ARCADE_INTEGRATION.md`, `ARCADE_VISUAL_COVERAGE.md`, `GT1_CONTENT_AUDIT.md`, `SSR11_RENDERING.md`, `TO-DO.MD`.
- Ключевое для нас из [MODERN_RENDERER.md](https://raw.githubusercontent.com/GTTeancum/OpenGTPS1/main/docs/MODERN_RENDERER.md):
  - Захват на уровне **GTE**, а не GPU: формат OGTWCAP v4 - на каждый треугольник 224 байта: model-space вершины на входе в RTPS/RTPT, полные fixed-point GTE rotation/translation, camera-space результат, screen coords, плюс provenance (submission index, draw state, model pointer, object identity, transform identity). Репроекция даёт 0.0 RMS error.
  - Камера кадра = "dominant track transform", мировые координаты получаются обратным fixed-point преобразованием.
  - Depth: D32F reversed infinite projection, homogeneous near-clipping вместо culling; кузов+колёса сохраняют авторский порядок слоёв.
  - Хуки по адресам (для их билда): backdrop/track visibility `func_800298E8` (Arcade) / `func_8002993C` (Sim); race loop `0x8001545C` передаёт car records в `0x800140A4`; bounding-box gates `0x8007B550` (Arcade) / `0x8007B640` (Sim).
  - Прямо заявлено: нет free camera, mirror-геометрия исключена из основного буфера, HUD - screen-space треугольники. Т.е. это "идеальный" GTE-capture, а не владение сценой.
- Из [PORT_PLAN.md](https://raw.githubusercontent.com/GTTeancum/OpenGTPS1/main/docs/PORT_PLAN.md): адреса `0x80010954` (установка VBlank callback), `0x8007D23C` (VSync wrapper), `0x8008A088/0x8008A0A8` (CD callbacks), `0x8007C550` (ожидание CD idle); XA-стриминг должен идти с точным NTSC-отношением `75 / (60000/1001)` секторов на VBlank; релокации рабочих областей в расширенную память (`0x80200000`, `0x80400000`, `0x80500000`), geometry buffer увеличен до 0x70000.
- Open issues (15-17 Sep 2026): поддержка A-Spec ROM hack, lighting/texture improvements, dust spray, Xbox port. VR/widescreen не упоминаются.

### 2.4 SamiulH25/gt2-recomp - hybrid recomp + incremental decomp

<https://github.com/SamiulH25/gt2-recomp>

- База: GT2 (USA) Simulation v1.2 (MD5 bin: e697a485b661a12fa6c327186c336a31). 64 commits, 1 star. Грузится до garage menu, рендерит гонки, 26000+ кадров без падений. OpenGL + software renderer, SDL3. Использует psxrecomp-экосистему ([mstan/psxrecomp](https://github.com/mstan/psxrecomp), PolyForm Noncommercial).
- Самое ценное - документация:
  - [docs/OVERLAYS.md](https://raw.githubusercontent.com/SamiulH25/gt2-recomp/main/docs/OVERLAYS.md) - точная структура `GT2.OVL` (см. раздел 3).
  - [docs/WIDESCREEN_RE.md](https://raw.githubusercontent.com/SamiulH25/gt2-recomp/main/docs/WIDESCREEN_RE.md) - 4 "projection funnels" в `gt2_01`: `0x8001C17C` (RTPS @ 0x8001C1E4), `0x800234F8` (RTPS, дубликат/LOD), `0x80019B58` (RTPT @ 0x80019BFC), `0x8002106C` (RTPT @ 0x80021128); всего 92 projection sites (66 RTPS + 26 RTPT); scratchpad slot `0x1F800070`.
  - [docs/PLAN_FULL_DECOMP.md](https://raw.githubusercontent.com/SamiulH25/gt2-recomp/main/docs/PLAN_FULL_DECOMP.md) - main EXE: **1593 функции**, 31535 блоков; `gt2_01` - 1039 функций; GTE-операции: 1314 в `gt2_01` против 51/8/0/1/2 в остальных -> `gt2_01` это гоночный 3D-движок. 19 "clean-room C11" модулей (`gt2_cd`, `gt2_vol`, `gt2_iso`, `gt2_ovl`, `gt2_boot`, `gt2_task`, `gt2_spu`...) - только библиотечный/системный слой, не игровая логика.
  - `docs/GTFS.md`, `docs/ARCHITECTURE.md` (fopen GTFS @ `0x8001146C`, stack @ 0x801FFF00, globals в 0x801Dxxxx), `tools/` со `split_ovl.py` и Ghidra-скриптом `LoadGT2Overlays.py`.
- Лицензия не указана.

### 2.5 Чего НЕ найдено

- Публичных Ghidra/IDA баз (`.gzf`, `.idb`) по GT2 - нет.
- Декомпиляции GT1 - нет (только инструменты для ассетов: [pez2k/gt2tools](https://github.com/pez2k/gt2tools) GT1-часть, [JeevesGB/GTExplorer](https://github.com/JeevesGB/GTExplorer)).
- Утечек исходников Polyphony PS1-эпохи - не найдено.

---

## 3. Executable layout: SCUS_944.88 + GT2.OVL

По [gt2-recomp OVERLAYS.md](https://raw.githubusercontent.com/SamiulH25/gt2-recomp/main/docs/OVERLAYS.md) и конфигам gt2-reversing (US Sim v1.2):

`GT2.OVL` = заголовок + **6 подряд идущих gzip-членов**, 289 689 байт сжато -> 1 133 632 распаковано. Все грузятся на **0x80010000** (взаимоисключающие), поверх них в памяти лежит резидентный `gt2main` с 0x8005D600.

| Member | Offset | Compressed | Decompressed | Роль (степень уверенности) |
|---|---|---|---|---|
| gt2_01 | 0x30 | 144 709 | 316 920 | **гонка/3D-движок** (1314 GTE ops; высокая) |
| gt2_02 | 0x23578 | 44 333 | 248 004 | меню/GT mode? (не установлено) |
| gt2_03 | 0x2E2A8 | 53 389 | 275 780 | не установлено |
| gt2_04 | 0x3B338 | 5 195 | 11 500 | маленький служебный |
| gt2_05 | 0x3C784 | 38 461 | 273 012 | содержит license load (по коммитам GT2-DECOMP; средняя) |
| gt2_06 | 0x45DC4 | 3 602 | 8 416 | маленький служебный |

Следствия:
- Самый большой overlay (316 920 = 0x4D5F8) заканчивается на 0x8005D5F8 - вплотную к `gt2main` @ 0x8005D600. Отсюда ограничение на размер при модинге.
- Декомпрессор gzip находится в main EXE (`gt2_main_gzip_decompress*`), загрузчик - `gt2_load_overlay[_default]`.
- По TCRF (сниппет поиска, [Gran Turismo 2/Gran Turismo Mode Disc](https://tcrf.net/Gran_Turismo_2/Gran_Turismo_Mode_Disc)): GT-mode код в GT2.OVL содержит расчёт процента прохождения и массив из 248 event ID, зеркалящий `gtmode_race.dat`; 25 записей - placeholder'ы (license/Machine Test), которые никогда не помечаются "complete".
- Silent (CookiePLMonster) в [посте про codes pack](https://silentsblog.com/2021/09/06/gran-turismo-2-codes-pack/) отмечает, что из-за загрузки оверлеев патчи кода надо накладывать аккуратно (условные коды), иначе краши.
- Инструменты для OVL: `GT2OVLTool` (C#, [pez2k/gt2tools](https://github.com/pez2k/gt2tools/tree/master/GT2OVLTool)), `ovl.py` ([CookiePLMonster/GTModTools](https://github.com/CookiePLMonster/GTModTools), MIT), `split_ovl.py` (gt2-recomp).
- Arcade disc (SCUS-94455) имеет собственный EXE/OVL с другими адресами (видно по парным Arcade/Sim адресам в OpenGTPS1: `0x800298E8` vs `0x8002993C`, `0x8007B550` vs `0x8007B640`). Публичного splat-конфига для Arcade disc нет.

**GT1 для сравнения** ([GTPlanet, GT1 Modding discussion](https://www.gtplanet.net/forum/threads/gt1-modding-discussion.424443/), pez2k): исполняемые файлы GT1 частично сжаты кастомным LZSS и распаковывают себя сами (загрузчик релоцирует себя и распаковывает с конца назад). В Ghidra без дампа памяти видно только блок сжатых данных + loader. Практика: анализировать RAM-дамп из эмулятора. Видео-серия NDR008 "Gran Turismo Racing AI (PSX)" ([Part 5 - unpack GTMAIN.EXE](https://www.youtube.com/watch?v=6tuSvjPUQ6o), [Part 6](https://www.youtube.com/watch?v=vy98V4HlIS0), [7a camera](https://www.youtube.com/watch?v=0gJSjnD4WvE), [7b acceleration](https://www.youtube.com/watch?v=wIGjK0zMzGo), [7c](https://www.youtube.com/watch?v=Hn1XoVq59Bc)) + репо [NDR008/TensorFlowPSX](https://github.com/NDR008/TensorFlowPSX) (папка `ReduxLua`, диссертация PDF) - RL-агент для GT1 через PCSX-Redux, с RE-находками по памяти GT1. Конкретные адреса я в README не увидел - они в Lua-скриптах/PDF.

---

## 4. Debug symbols / прототипы (ключевой вопрос)

**Результат отрицательный.**

- pez2k, 22 Aug 2021, [GTPlanet, GT2 Plus thread p.56](https://www.gtplanet.net/forum/threads/mod-gran-turismo-2-plus-bug-fixes-restored-content-and-new-content-beta-7-released.378282/page-56): хотел бы декомпилировать GT2, но для GT2 нет debug symbols - в отличие от проектов вроде REDRIVER2.
- [RetroReversing: PS1 games with debug symbols](https://www.retroreversing.com/ps1-debug-symbols) - 40+ игр с `.SYM`/`.MAP`; ни Gran Turismo, ни GT2, ни Motor Toon GP, ни Omega Boost в списке нет.
- [GT Modding Hub, builds preface](https://nenkai.github.io/gt-modding-hub/builds/game_builds/): единственные executable debug symbols во всей серии - прототип GTHD (PS3), "close enough to GT4" ([archive.org](https://archive.org/details/gthd-ps3-debug-binaries)). Для PS1-эпохи бесполезно напрямую, но даёт имена классов/терминологию Polyphony (вероятный источник `ScreenViewLoop`).

Известные билды GT2 ([GT Modding Hub: GT2 builds](https://nenkai.github.io/gt-modding-hub/builds/gt2/)):

| Билд | Дата | ID | Dump |
|---|---|---|---|
| Demo Build #1 (OPM US #27, Euro Demo 87, McDonald's) | 29 Jul 1999 | SCUS-94435 | есть |
| Demo Build #2 | 30 Aug 1999 | SCUS-94668 | есть |
| Demo Build #3 (EU) | 7 Sep 1999 | SCED-01827 | есть |
| Demo Build #4 | 21 Sep 1999 | PAPX-90054 | есть |
| NTSC Preview | 30 Nov 1999 | SCPS-10117 | есть |
| Arcade Mode Review Version (late EU) | 22 Dec 1999 | - | есть |
| "Making of", E3 1999, TGS 1999, Dec 1999 unknown | - | - | не сдамплены |

Также Pizza Hut Demo Disc 2 (SCUS-94481) по [GTPlanet list](https://www.gtplanet.net/forum/threads/list-of-all-gran-turismo-demo-beta-preview-builds.391187/).

GT1-прототипы на Hidden Palace: [Jul 29 1997](https://hiddenpalace.org/Gran_Turismo_(Jul_29,_1997_prototype)), [Aug 2 1997 "Test Drive Disc"](https://hiddenpalace.org/Gran_Turismo_(Aug_2,_1997_prototype)), [Sep 1997](https://hiddenpalace.org/Gran_Turismo_(Sep,_1997_prototype)), [Nov 13 1997 "Preview V2, ACCESS ALL MODES"](https://hiddenpalace.org/Gran_Turismo_(Nov_13,_1997_prototype)). Страницы Hidden Palace про символы ничего не говорят. Сниппет поиска по TCRF упоминает, что в Nov 13 билде "debug information is present" - по контексту это отладочный вывод/меню, а не `.SYM`; **не подтверждено**.

Что всё же стоит проверить самим (дёшево, на своих образах и на демо, если пользователь их легально достанет): поиск по всем файлам дисков строк `.c`, `.cpp`, `assert`, `__FILE__`-подобных путей, `printf`-форматов; сравнение демо-EXE с retail через function-hash matching (демо собраны тем же компилятором, но на 3-5 месяцев раньше - полезны для понимания эволюции, не для имён).

Реальные источники имён вместо символов:
1. PsyQ library signatures (FLIRT/ghidra_psx_ldr) - снимают ~25-35% функций main EXE как библиотечные.
2. Именование Polyphony из GTHD/GT4 символов - для классов верхнего уровня.
3. Имена файлов в `GT2.VOL` и поля carparam из [gt2tools](https://github.com/pez2k/gt2tools) / [Nenkai/GT-File-Specifications] (последнее - по упоминанию на Modding Hub, сам репо не открывал) / [SUBMANIAC PDF](https://nenkai.github.io/gt-modding-hub/ps1/gt2/documents/Gran_Turismo_2_files_full_documentation.pdf) - позволяют называть функции-загрузчики и поля структур физики.
4. Готовые symbol_addrs из gt2-reversing (CC0) и GT2-DECOMP.

---

## 5. Cheat/RAM-адреса и ASM-патчи моддинг-сцены

- **Silent / CookiePLMonster** - самый технически глубокий источник по коду GT2:
  - [Console-Cheat-Codes](https://github.com/CookiePLMonster/Console-Cheat-Codes) - `PS1/Gran Turismo 2/` : 60 FPS, 16:9 widescreen (и [генератор под любой aspect](https://github.com/CookiePLMonster/Console-Cheat-Codes/blob/master/PS1/Tools/gt2-widescreen-gen.py) для NTSC-U 1.2/1.1, NTSC-J 1.1/1.0, PAL), full-detail AI cars, replay draw distance в гонке, 8MB RAM polygon buffers, HUD/mirror toggle, replay cameras в гонке, True Endurance, metric units. Обзор: <https://silentsblog.com/mods/gran-turismo-2/>.
  - 60 FPS для NTSC-U 1.2 ([файл](https://raw.githubusercontent.com/CookiePLMonster/Console-Cheat-Codes/master/PS1/Gran%20Turismo%202/60%20FPS/NTSC-U%201.2.cht)): условная запись по `0x801D5864`, патчи `0x800168C8` (tire smoke), `0x80019644` (небо в зеркале), `0x8003EC74` + `0x80029550` (зеркало), `0x8001F888`. Адреса 0x8001xxxx-0x8003xxxx лежат в оверлее `gt2_01`, поэтому коды условные.
  - По TCRF (сниппет): в GT2 остался неиспользуемый код, повышающий framerate cap и отключающий дым/зеркало, включаемый байтом в race parameters - т.е. **60 fps режим заложен самими разработчиками** (наследие GT1 Hi-Fi mode), физика от этого не ломается.
  - Из поста Silent: игрок всегда на максимальном LOD, AI-машины переключаются между 3 LOD; draw distance задаётся данными трассы; в replay она выше; polygon buffers переполняются при full-LOD AI.
- **Xenn's codes** - [GTPlanet thread](https://www.gtplanet.net/forum/threads/xenns-cheat-device-codes-includes-demo-codes.187354/): коды для PAL, NTSC 1.0/1.1/1.2, NTSC-J и демо: camera changer (66+ видов), track loader, число машин, AI-скорость, autopilot, drag race enable, tyre wear toggle. Пример адреса: `801C30F0` (цены). Полной RAM map (позиция/скорость/матрица машины) в открытом виде не найдено.
- Прочие базы: [GTPlanet GT1/GT2 v1.2 codes (tempar)](https://www.gtplanet.net/forum/threads/gt1-and-gt2-v1-2-gameshark-codes-tempar.232498/), [almarsguides v1.2](https://almarsguides.com/retro/walkthroughs/PS1/Games/GranTurismo2/Gameshark/Version1.2/), [Pupik's GT2 Spot](https://pupik-gt2.tripod.com/codes/index.htm) - в основном деньги/машины, мало полезного для RE.
- **pez2k GT2 Plus** - [gt2mods](https://github.com/pez2k/gt2mods) (667 commits, некоммерческое использование) + [gt2tools](https://github.com/pez2k/gt2tools) (593 commits, 60+ инструментов, в т.ч. `GT2OVLTool`). Патчи кода лежат как папки `executable/` и `overlays/` внутри компонентов (`newsaveslot_complete/executable`, `metrichorsepower/overlays`, `bmw_wheelshop/overlays`, `arcade_rallycarstats/overlays`, `aspirationdisplay_todo/overlays/pal_sim`). Релиз - xdelta для всех дисков (NTSC-J 1.0/1.1, NTSC-U 1.0/1.1/1.2, PAL), контент выравнивается по финальному PAL. Code-level фиксы: random track никогда не выбирал Apricot Hill, недоступная brake balance у racecars, отдельный save slot. Метод pez2k: IDA/Ghidra, поиск багованной логики среди безымянных функций и точечная правка инструкций.
- **GT2 Combined Disc** - [CookiePLMonster/GT2-Combined-Disc](https://github.com/CookiePLMonster/GT2-Combined-Disc) (MIT, Python 3.8+, GTVolTool, 35 stars, 2022): все версии кроме NTSC-J 1.0.
- **Project A-Spec** - [archive.org](https://archive.org/details/gran-turismo-2-project-a-spec-v-1.2): база US NTSC v1.2 Simulation disc; combined-сборка менее стабильна.

---

## 6. Физика и известные баги

Глубокого публичного RE физики GT2 **нет** - это главный пробел. Что известно:

- Данные физики - в carparam (GT2DataSplitter из gt2tools конвертирует в CSV; описание полей - SUBMANIAC PDF). Апгрейды ссылаются по позиции в списке, а не по ID (pez2k, 2018).
- Fixed-point: в main EXE идентифицированы `gt2_fxpoint_multi8/12/16` (gt2-reversing). PORT_PLAN OpenGTPS1 подтверждает, что suspension/tire contact/drivetrain/collision/timing - целочисленная fixed-point логика.
- Tick rate: точного публичного подтверждения шага физики я не нашёл. Косвенно: игра идёт в 30 fps, 60 fps включается штатным флагом без поломки симуляции -> шаг привязан к VSync-счётчику, а не к кадру рендера. **Нужно установить самим** (трассировка в DuckStation/PCSX-Redux). Уверенность низкая.
- Wheelie / ride height bug: [GT2 Plus thread p.2](https://gtplanet.net/forum/threads/mod-gran-turismo-2-plus-bug-fixes-restored-content-and-new-content.378282/page-2) (submaniac93, 8 Jun 2018): wheelie возникает при max rear / min front downforce и перекосе подвески; GT2 плохо моделирует рост drag с downforce. Видео-разбор: ["How Ride Height Broke Gran Turismo 2's Physics"](https://www.youtube.com/watch?v=SlocmIbafW0) (не смотрел, только заголовок из выдачи). По сниппету: перекос ride height влияет на ускорение, т.к. игра считает машину стоящей на уклоне.
- 98.2% bug (сниппет TCRF [Bugs:Gran Turismo 2](https://tcrf.net/Bugs:Gran_Turismo_2) + [speedrun.com](https://www.speedrun.com/post/io7gp)): игра ожидает 223 события, доступно 219 (2 гонки только для EU + 2 вырезанных). Затронуты NTSC-J 1.0, NTSC-U 1.0, NTSC-U 1.1; исправлено в NTSC-U 1.2 / PAL.
- Для нашей цели это значит: физику придётся либо декомпилировать функция-в-функцию из `gt2_01` (bit-exact integer C, сохраняя все баги как опцию), либо на первом этапе исполнять оригинальный MIPS-код (recomp/интерпретатор) и верифицировать декомп против него покадрово - как делает gt2-recomp (`mips_emu.py`, cross-validation).

---

## 7. Какая версия каноническая

**NTSC-U v1.2 Simulation (SCUS-94488, Greatest Hits) + NTSC-U Arcade (SCUS-94455).**

Причины:
1. Все четыре code-проекта (gt2-reversing, GT2-DECOMP, OpenGTPS1, gt2-recomp) и Project A-Spec используют именно её -> все опубликованные адреса, symbol_addrs, splat-конфиги применимы напрямую.
2. Последняя ревизия: исправлены 98.2% bug и прочие ошибки 1.0/1.1; по словам ginryuoku почти идентична PAL по коду.
3. NTSC = 60 Hz и один язык (PAL - 50 Hz и мультиязычность, сложнее).
4. Cheat-коды Silent есть под 1.2.

У пользователя именно эти образы (Sim v1.2 + Arcade v1.1). JP Disc 1 полезен только для сравнения/контента. Адреса из источников для Arcade disc нужно перепроверять: публичной привязки "какая ревизия Arcade" у OpenGTPS1 я не видел.

---

## 8. Что это значит для нашего плана

1. **Не начинать с нуля.** Взять CC0 symbol_addrs/splat-конфиги gt2-reversing как стартовую базу Ghidra-проекта (US 1.2). Просмотреть GT2-DECOMP на предмет дополнительных имён (проверить лицензию).
2. **OpenGTPS1 - читать, но не копировать** (нет лицензии). Его MODERN_RENDERER.md - готовое доказательство, что GTE-level capture с provenance даёт bit-exact мировую геометрию. Это тот же подход, что у пользователя в fighting-force-vr, только на уровень выше (GTE вместо GP0). Для VR этого мало: нужен контроль камеры и culling, т.е. владение сценой.
3. **Рекомендуемая стратегия - гибрид в 2 слоя:**
   - Слой A (быстрый результат): оригинальный код гонки исполняется (recomp или встроенный MIPS-интерпретатор/ядро эмулятора), а рендер-функции `gt2_01` (4 projection funnels + submit машин/трассы) заменяются нативными, которые читают **исходные структуры игры** (модели CDO/трассы из VOL, матрицы машин, камеру) и рисуют своим рендером со своей камерой -> сразу widescreen/VR/отключение culling.
   - Слой B (долгий): постепенная декомпиляция физики/AI/игровой логики с покадровой верификацией против слоя A.
4. **Первые RE-задачи**: (a) структура car state (позиция, ориентация, колёса) и camera state в `gt2_01`; (b) track visibility/culling (`func_8002993C`, bbox gate `0x8007B640`) - для VR надо рисовать всё вокруг; (c) шаг физики и его связь с VSync; (d) replay-формат (детерминизм = бесплатный regression test; OpenGTPS1 это уже использует).
5. **Символов не будет** - закладывать время на ручное именование; использовать PsyQ signatures, терминологию GTHD/GT4, имена файлов VOL.
6. Arcade disc оставить на потом: публичной базы по нему нет, а Sim disc содержит тот же гоночный движок.
