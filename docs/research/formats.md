# GT2 (PS1): форматы ассетов и инструменты сообщества

Исследование форматов, 2026-09-17. Предположения и непроверенные сведения помечены отдельно.

Уровни уверенности: **[H]** высокая (видел первичный источник: код/шаблон/документ), **[M]** средняя (вторичный источник или пересказ fetch-модели), **[L]** низкая (по памяти / косвенно).

---

## 0. TL;DR

1. **Контейнер, машины, текстуры машин, базы параметров, меню, сейвы - закрыты сообществом.** Есть рабочий открытый код (в основном C#/.NET у pez2k и adeyblue, MIT у adeyblue; лицензия у pez2k/gt2tools не найдена - файла LICENSE нет, 404).
2. **Трассы (.tro) - главный пробел, но уже не "белое пятно".** Есть 010 Editor template от Nenkai (`GTPS_tro_GTTrackObject.bt`), который описывает header, chunk-таблицу, LOD-таблицы, road tris с `SurfaceType`, boundaries, стартовые позиции, instanced objects. Семантика многих полей unknown; AI-линии, replay-камеры, pit, checkpoints не идентифицированы. Публичного конвертера/вьюера для GT2 .tro я НЕ нашёл (GT Track Viewer от Leo2236 - только GT1, без исходников).
3. **Физика: таблицы хорошо размечены по именам полей** (SUBMANIAC сопоставил их с именами из GT6 SpecDB), но TireCompound, ASC, TCS - сырые `.dat` без полной разметки, а сами формулы физики живут только в коде игры.
4. **Реплеи = запись ввода + детерминированная физика.** Формат не документирован. Это жёсткое требование к будущему порту: без бит-точной физики оригинальные реплеи, rally-ghost'ы (`dirt/*.dgf`), лицензионные демо (`license/*.lgf`) и attract-демо (`demofile*.gmr`) работать не будут.
5. **Неожиданная находка: за последний месяц появились ДВА проекта "GT2 native PC" через статическую рекомпиляцию** - [GTTeancum/OpenGTPS1](https://github.com/GTTeancum/OpenGTPS1) (v0.9b, играбелен, свой D3D11-рендер, но *без лицензии* и "уходит в hiatus") и [SamiulH25/gt2-recomp](https://github.com/SamiulH25/gt2-recomp) (hybrid recomp + incremental decomp, OpenGL/SDL3). Плюс старый decomp-проект [ginryuoku/gt2-reversing](https://github.com/ginryuoku/gt2-reversing) (splat, CC0, последний коммит сентябрь 2024). Это меняет стратегию: см. раздел 12.

---

## 1. Главные источники

| Источник | Что это | Язык / лицензия | Активность |
|---|---|---|---|
| [pez2k/gt2tools](https://github.com/pez2k/gt2tools) | ~57 проектов: GT2ModelTool, GT2TextureEditor, GT2DataSplitter(+Rewrite), GT2CourseInfoEditor, GT2BillboardEditor, GT2CarInfoEditor*, GT2UsedCarEditor, GT2MenuSplitter, GT2OVLTool, GT2SaveEditor, StrEditGT2, GT1-инструменты и т.д. | C# (.NET 6/8). LICENSE в корне не найден (404) - **лицензия не определена** | Активен: последние коммиты 2026-02-03 (GT2BillboardEditor), 2025-09 (DataSplitter rewrite), 2025-05 (ModelTool 2.1.0) [H] |
| [adeyblue/GTVolTools](https://github.com/adeyblue/GTVolTools) | GTVolTool (VOL extract/rebuild GT2/GT2000/GT3), GT2DataExploder (C++), GMCreator (меню GT2), GTMP | C# + C++, **MIT** | Заморожен: последний коммит 2022-05-05 [H] |
| [Nenkai/GT-File-Specifications-Documentation](https://github.com/Nenkai/GT-File-Specifications-Documentation) | 010 Editor templates. Для PS1: `Formats/PS1/GTFS_VOL.bt`, `Formats/PS1/GT2/CarDayNightObject_cdo_cno.bt`, `CarDayNightPalette_cdp_cnp.bt`, `GTPS_tro_GTTrackObject.bt` | 010 template (C-подобный), **MIT** | 209 коммитов, живой [H] |
| [SUBMANIAC: "Gran Turismo 2 - In-depth file info"](https://nenkai.github.io/gt-modding-hub/ps1/gt2/documents/Gran_Turismo_2_files_full_documentation.pdf) | 62-страничный PDF: обход всего GT2.VOL по папкам, layout CDO/CDP, .es, семантика всех таблиц carparam, черновые заметки по .tro | документ | Прочитан целиком (через pdftotext) [H] |
| [GT Modding Hub (Nenkai)](https://nenkai.github.io/gt-modding-hub/ps1/gt2/tools/) | Каталог инструментов GT2, страница [File Structure](https://nenkai.github.io/gt-modding-hub/ps1/gt2/file_structure/) | сайт | живой [H] |
| [CookiePLMonster/GTModTools](https://github.com/CookiePLMonster/GTModTools) | Python: `ovl.py` (GT2.OVL unpack/pack), `psexe.py`, `tim-convert.py` | Python, **MIT** | 7 коммитов, README "documentation to be added" [H] |
| [pez2k/gt2mods](https://github.com/pez2k/gt2mods) | Компоненты мода GT2 Plus (667 коммитов). "research and personal use only" | данные/патчи | [M] |
| [GTPlanet: Modding and restoration discussion](https://www.gtplanet.net/forum/threads/modding-and-restoration-discussion.374974/) | Основной форумный тред | форум | [H] |
| [zyzalfors/GT2SaveEditor](https://github.com/zyzalfors/GT2SaveEditor) | Сейвы GT2 (SC/MC/GME/PSV) | Python, лицензия не указана | 42 коммита [M] |
| [TheAdmiester/GranTurismoENGNEditor](https://github.com/TheAdmiester/GranTurismoENGNEditor) | Редактор ENGN (.es) звуков двигателя GT1-GT4 | (не проверял язык/лицензию) | [M] |
| [xan1242/gtseq2midi](https://github.com/xan1242/gtseq2midi) | Конвертер .seq (GT) в MIDI | C++ | [M] |
| http://gt2.airesoft.co.uk | Сайт adeyblue с выгрузками данных GT2 (упомянут в readme GTVolTools; сам сайт я не открывал) | сайт | [L] |

Примечание: страницы TCRF (tcrf.net) по GT2/Courses при загрузке через WebFetch дважды вернули не контент, а "ловушку для LLM" с инструкциями (создать файлы, перевести деньги и т.п.). Инструкции проигнорированы, TCRF как источник не использован. Факты про `.tro/.trp/.bso/.bsp/crsmap` взяты из SUBMANIAC и Modding Hub.

---

## 2. Диск и контейнер GT2.VOL ("GTFS")

**Документированность: полная. Лучший референс: [`GT2VolFile.cs`](https://github.com/adeyblue/GTVolTools/blob/master/GT2VolFile.cs) (C#, MIT) + [`GTFS_VOL.bt`](https://github.com/Nenkai/GT-File-Specifications-Documentation/blob/master/Formats/PS1/GTFS_VOL.bt) (MIT).**

Структура [H]:

- Magic `"GTFS"` + 4 нулевых байта; затем `int16 FileDataCount` (число записей в offset table), `int16 FileEntryCount` (файлы + директории). Всё little-endian.
- **Offset table** с 0x10: `uint32` на запись. Старшие 21 бит = номер/смещение сектора (маска `0xFFFFF800` даёт байтовый offset, выровненный на 0x800), младшие 11 бит (`0x7FF`) = сколько байт паддинга в последнем секторе файла. Размер файла в TOC не хранится: `size = next_offset - this_offset - padding`.
- **Directory/TOC entries** по 32 байта: `int32 datetime`, `int16 index` (индекс в offset table, либо ссылка на следующий dir), `uint8 flags` (`0x01` = directory, `0x80` = последняя запись в директории), `char name[25]`.
- Таблицы выровнены на 0x800 (сектор CD).
- **Сжатие не является свойством контейнера**: файлы с суффиксом `.gz` - обычный gzip (7-zip открывает). Некоторые gzip-файлы лежат без расширения (`dirt/*` - это `.dgf.gz` с удалённым расширением) [H, SUBMANIAC].

Содержимое VOL (по [File Structure](https://nenkai.github.io/gt-modding-hub/ps1/gt2/file_structure/) и SUBMANIAC) [H]:

```
.text/data-race.txd      строки для гонки (локализации)
arcade/                  TIM-атласы UI аркады, arc_carlogo, arc_fontinfo, course_map(+info),
                         demofile*.gmr(.gz), game_status_file.gz (HUD+тахометры), license_info_*
bgsobj/   *.bso(.gz) *.bsp(.gz)   skybox: модель + текстуры
carlogo/  xxxxx{l..q}--.tim       логотипы машин по регионам/режимам
carobj/   *.cdo *.cno *.cdp *.cnp (gz)  модели/текстуры машин day/night
carparam/ arcade_data / gtmode_data / gtmode_race / license_data / *_unistrdb (.dat.gz, языковые префиксы)
carwheel/ *.tim 48x48             aftermarket-диски
crsmap/   *.tim.gz 96x96 4bpp     миникарты
crsobj/   *.tro(.gz) *.trp(.gz)   трассы: геометрия+логика / текстуры
dirt/     *.dgf (gz без расширения)  ghost-реплеи соперников в ралли
engine/   *.es                    звуки двигателя (ENGN)
font/racefont.dat                 512x256 4bpp, 3 CLUT, без заголовка
gtmenu/   commonpic.dat/.idx, <lang>/gtmenudat.dat/.idx, iconimg.dat.gz, solodata.dat.gz
license/  *.lgf.gz                демо-прохождения лицензий
replay/   scea.000/.999 и т.п.    пустые файлы-заглушки
sound/    *.ins (VAG-банки), *.seq (секвенции меню)
.carcolor .carinfoa/e/j .ccjapanese .cclatain .crsinfo .crstims.tsd.gz .usedcar* crstim.arc
```

Вне VOL на диске: основной PS-EXE (SCUS_944.55 / SCUS_944.88), **GT2.OVL**, стримы музыки/видео (XA/STR; OpenGTPS1 упоминает XA audio playback [M]; конкретный список файлов диска нужно снять самим).

Различия дисков: Arcade и Simulation имеют разные VOL (аркада: `arcade_data`, 13-машинные ростеры и т.д.; GT mode: `gtmode_*`, `gtmenu/`, `.usedcar`). OpenGTPS1 собирает "unified GT2.VOL" из двух дисков [M].

---

## 3. GT2.OVL и исполняемый код ("скрипты")

**Важно для ответа пользователю: в GT2 нет скриптового языка/VM.** Вся логика - нативный MIPS-код в главном EXE + оверлеях; "данные поведения" - это таблицы carparam и структуры меню.

- **GT2.OVL**: заголовок = массив пар `uint32 offset, uint32 size` (число записей определяется по offset первой записи), далее каждый оверлей как **gzip-поток**. Референс: [`ovl.py`](https://github.com/CookiePLMonster/GTModTools/blob/master/ovl.py) (Python, MIT) [H]. Есть также `GT2OVLTool` у pez2k (C#) [H - по списку директорий].
- В Simulation-диске **6 оверлеев** (по README gt2-recomp) [M]. gt2-reversing именует "all known overlay entrypoints" и функцию `gt2_load_overlay` [H - по заголовкам коммитов].
- Код игры частично на **C++** (gt2-reversing добавил C++-классы `ScreenViewLoop`, `psxViewLoop`, mangled symbols - "thanks to Nenkai") [H]. То есть в бинарнике есть остатки символов/RTTI-подобной информации, что облегчает reverse.
- PsyQ SDK: стандартные libgpu/libgte/libcd/libspu; оба recomp-проекта маппят PsyQ CD callbacks [M].

---

## 4. Машины

### 4.1 CDO / CNO (модель day / night)

**Документированность: высокая (геометрия, UV, LOD, колёса, shadow - в коде; остаются 2-4 unknown-блока).**
**Лучший референс: [`GT2ModelTool/Structures/*.cs`](https://github.com/pez2k/gt2tools/tree/master/GT2ModelTool/GT2ModelTool/Structures)** (CDOHeader, LOD, Model, Normal, Polygon, UVPolygon, Shadow, ShadowPolygon, ShadowVertex, WheelPosition) - C#, round-trip CDO <-> OBJ, v2.1.0 (2025-05-23). Второй референс: [`CarDayNightObject_cdo_cno.bt`](https://github.com/Nenkai/GT-File-Specifications-Documentation/blob/master/Formats/PS1/GT2/CarDayNightObject_cdo_cno.bt) (research: SUBMANIAC, commongear, pez2k).

Layout (сводка из трёх источников) [H]:

- `0x00` "GT" + version (=2). `0x07` флаг shadow(?). `0x08..0x0A` RGB цвета "wheel dish".
- `0x18..0x1F` радиус/ширина передних/задних колёс (u16, в "PD units").
- `0x20..0x3F` четыре `WheelPosition` (по 4 x s16: W/Y/X/Z; отрицательный X = перед, отрицательный Z = лево).
- `0x40..0x867` **пусто в файле, заполняется игрой в рантайме** (VRAM-координаты палитры, rim-полигоны, вершины 3 LOD'ов колёс: sidewall/tread/back). Т.е. CDO - это почти дамп рантайм-структуры; меш колеса генерируется кодом, а не хранится.
- `0x868` LOD count (всегда 3), далее по LOD: max distance (u16) + offset (u32).
- Каждый LOD: счётчики (vertices, normals, tris, quads, 2 unknown, UV tris, UV quads, 2 unknown), offsets, bounding box (8 x s16), scale.
- Vertex = 4 x s16 (XYZ + pad). Normal = packed 32 bit (3 x 10-bit signed + 2 bit). **LOD ограничен 256 вершинами** (индексы u8). Полигон без UV = 16 байт; с UV = 28 байт (те же 16 + UV на вершину + palette index). Поля полигона: 4 x u8 индексы вершин, 5-bit **render order** (ручная сортировка граней вместо Z-буфера), 9-bit индексы нормалей, flags (reflection on/off, brake light face), RGB, код примитива PsyQ (`POLY_F3/F4/G3/G4/FT3/FT4/GT3/GT4`).
- Palette index у UV-полигона закодирован со сдвигом (значения 0x00..0x03, 0x40..0x43, 0x80.., 0xC0..) - 16 CLUT'ов.
- В конце файла **Shadow**: ~0xC4 байт, заголовок 28 байт, 20 вершин XZ (без высоты), грани по 4 байта с флагом gradient/opaque.
- Лимит размера файла 0x5000 байт (буфер в игре). Масштаб: 1 м ~ 2.5 "см" в единицах моделирования SUBMANIAC (точный коэффициент к метрам надо вывести самим из wheelbase в carparam).

Unknown [H]: два блока с счётчиками на 0x24/0x28 LOD-заголовка (структуры по 6-7 s16), ещё два offset'а (0x34/0x38) помечены TODO; "normal remapping". В 2019 pez2k писал, что reflections/shadows - "mystery" ([GTPlanet p.7](https://www.gtplanet.net/forum/threads/modding-and-restoration-discussion.374974/page-7)); к 2025 shadow уже читается/пишется GT2ModelTool, reflection управляется флагом материала.

Для нашего рендера: нужно поддержать (а) order-based сортировку или заменить её Z-буфером с аккуратной обработкой decal-граней (GT2ModelTool не зря имеет опцию "split overlapping faces"), (б) per-face флаги reflection/brake light, (в) смену CLUT 14 -> 15 для стоп-сигналов, (г) генерацию колёс из параметров.

### 4.2 CDP / CNP (палитры + текстура)

**Документированность: практически полная. Референс: [`GT2TextureEditor`](https://github.com/pez2k/gt2tools/tree/master/GT2TextureEditor) (C#) + [`CarDayNightPalette_cdp_cnp.bt`](https://github.com/Nenkai/GT-File-Specifications-Documentation/blob/master/Formats/PS1/GT2/CarDayNightPalette_cdp_cnp.bt).**

- `u16 paintCount` (до 15-16), массив paint ID (байт на цвет; ID должен совпадать с `.carcolor/.carinfo`).
- На каждый paint блок 0x240 байт: 16 CLUT x 16 цветов BGR555 (0x20 байт каждый) + **illumination mask** (0x20) + **paint mask** (0x20, не используется игрой, остаток инструментов PD).
- Пиксели с фиксированного offset **0x43A0**: 256x224, 4bpp (28 672 байт). Область между палитрами и пикселями - runtime/преаллокация.
- Соглашения: CLUT0 = текстура диска (первый цвет = цвет "dish"), CLUT14/15 = стопы выкл/вкл, CLUT1 обычно LOD2, CLUT12 фары. Чёрный = прозрачный, если не выставлен alpha bit (STP). Illumination mask = какие цвета не затеняются ambient-освещением (фары и т.п.).

### 4.3 Колёса, логотипы, имена, цвета

- `carwheel/*.tim`: стандартный TIM 48x48, первый цвет CLUT = цвет dish [H].
- Таблица `Wheel` в carparam: глубина dish (0..3) [H].
- `carlogo/`: TIM, суффикс имени l/m/n/o/p/q = JP GT / JP Arcade / US GT / US Arcade / PAL GT / PAL Arcade [H].
- `.carinfoa/e/j`, `.carcolor`, `.ccjapanese`, `.cclatain`: имена кузовов для реплеев, региональные блокировки, имена и chip-цвета красок. Референс: `GT2CarInfoEditorCSV` (pez2k) [H].
- Car ID: 5-символьное имя (`a-emn`, `nv34n`; последний символ n/r = normal/race-modified по наблюдению), в базах хранится как hash/packed-значение - `GT2CarIDConverter` (pez2k) конвертирует туда-обратно [M; алгоритм не смотрел].

---

## 5. Трассы

### 5.1 .tro (Track Object) - ГЛАВНЫЙ ПРОБЕЛ

**Документированность: частичная. Единственный структурный референс: [`GTPS_tro_GTTrackObject.bt`](https://github.com/Nenkai/GT-File-Specifications-Documentation/blob/master/Formats/PS1/GT2/GTPS_tro_GTTrackObject.bt) (Nenkai, MIT).** Кода-конвертера нет. SUBMANIAC даёт только черновые заметки (instanced objects, догадки про Bezier).

Что известно из шаблона [H для структуры, M для семантики]:

- Header: magic `char[12]`, version, offsets на `TrackChunks`, `LODLookupTable`, `LODData`, два unknown-блока (0x1C, 0x20), `SectorCount` + до 8 `Sectors {s16 unk; s16 VCoord}` (VCoord = дистанция вдоль трассы, в метрах - это и есть split-сектора), origin/стартовая линия (Vec3), направление старта, **16 стартовых позиций** (Vec3[16]), `InstancedObjectOffsets int[33]`.
- **TrackChunkTable**: `VCoordFinish` (длина круга), `NumChunks`, offsets чанков, плюс массив пар Vec3 "VCoord lookup" (вероятно центральная линия/сегменты для вычисления прогресса по трассе).
- **TrackChunk**: prev/next индексы (двусвязный список вдоль трассы, указатели перешиваются в рантайме), VCoord, Center (для поиска ближайшего чанка), 22 int позиционных данных, offsets на: `ChunkShape` (ShapeData - дорожное полотно), второй ShapeData (0x94), **BoundaryCollision** (границы/стены), `UnkStruct_0x9C` (16 списков ссылок на полигоны - в шаблоне пометка, что правка ломает AI path или collision), **RenderedChunkList** (список чанков, видимых из данного = авторский PVS).
- **ShapeData**: 8 списков road-полигонов; элемент: 3 x u8 индексы вершин, 3 unknown байта, `TextureID`, **`SurfaceType`** (тип покрытия -> `TireForceVol`: tarmac/guide(поребрик)/green/sand/gravel/dirt/water), RGB, `Prim`. В комментариях шаблона есть адреса подпрограмм игры (0x80027BBC, 0x80028588, 0x80027FC4) - готовые точки входа для Ghidra.
- **LOD**: `LODLookupTable` -> `LODInfo {distance, ptr}` -> `LODData` с 8 списками полигонов (структура "как в CDO"). Примитивы: весь набор PsyQ, включая SPRT/TILE/LINE.
- **Instanced objects** (трибуны, знаки, баннеры): по SUBMANIAC запись 28 байт: rotation XYZ (s16), object ID, scale XYZ (1000 = 1.0), visibility flag, position XYZ (s32).
- `Entry_0x20`: до 7 под-записей с Type и координатами (x16) - назначение unknown (кандидаты: источники света/зоны освещения `LightingArea1..3` из `.crsinfo`, триггеры, камеры).

**Не идентифицировано публично**: AI racing line(s), replay-камеры, pit lane, checkpoints/anti-cut, зоны освещения (tunnel/shadow), формат вершин чанка (SUBMANIAC: s16 XYZ, но "странные значения на месте разделителей"), UV-маппинг road-полигонов (в `RoadVertex` UV нет - возможно, UV выводятся из TextureID/тайлинга или лежат в unknown-блоках).

Важное подтверждение со стороны OpenGTPS1 ([MODERN_RENDERER.md](https://github.com/GTTeancum/OpenGTPS1/blob/main/docs/MODERN_RENDERER.md)) [M]: игра отдаёт геометрию трассы восемью потоками примитивов `F3/F4/G3/G4/FT3/FT4/GT3/GT4`, видимость = "текущий сектор + 3 в каждую сторону" по авторскому порядку, LOD selector 0 = максимальная детализация, задник (sky) = 257 примитивов. Даже этот проект **не парсит .tro нативно**, а перехватывает GTE/GPU-команды из рекомпилированного кода - показатель того, что формат до конца никем не разобран.

### 5.2 .trp, skybox, прочее

- `.trp` = контейнер стандартных TIM (текстуры трассы). `.bsp` = то же для skybox. Формат контейнера идентичен [H, SUBMANIAC]. Парсер тривиален (скан TIM-заголовков либо таблица offset'ов).
- `.bso` = модель skybox: вершины с 0x20/0x28, 8 байт на вершину, "как в CDO"; полигоны не размечены [M].
- `crsmap/*.tim.gz`: миникарты 96x96 4bpp, стандартный TIM [H]. `arcade/course_map` + `course_mapinfo` - превью трасс для аркады, не разобраны [H - "very little info"].
- **`.crsinfo`**: полностью размечен, референс `GT2CourseInfoEditor` (pez2k, v1.1): DisplayName, Filename, флаги IsNight/IsEvening/IsDirt/Is2Player/IsReverse/IsPointToPoint/Flag7 (отключение billboard-пула для трасс, портированных из GT1), Skybox, три `LightingAreaColour + Multiplier` (тинт машины в зонах тени) [H].
- **`.crstims.tsd.gz`**: пул рекламных billboard-текстур (TIM) + таблицы пулов (General01/02, One-Make, JP, US, UK, DE, FR, IT, TUNE); событие выбирает пул через `TrackBannerPool`. Референс: `GT2BillboardEditor` v1.0.0 (pez2k, февраль 2026) [H].
- **`crstim.arc`**: 6 TIM - flare, dust, smoke, reflection map day/dusk/night [H]. Это и есть env-map для "отражений" на машинах.
- Нейминг трасс: reverse / 2-player / ночные варианты - отдельные записи в `.crsinfo` с теми же или отдельными .tro (надо проверить на своих дисках).

---

## 6. Базы параметров (carparam)

**Документированность: структура - полная; семантика - высокая для большинства таблиц, частичная для шин/ASC/TCS.**
**Лучший референс: [`GT2DataSplitterRewrite/GT2.DataSplitter.Models`](https://github.com/pez2k/gt2tools/tree/master/GT2DataSplitterRewrite/GT2.DataSplitter.Models)** (C#, по классу на таблицу: Common/*, GTMode/{Car,EnemyCars,Regulations}, Arcade/*, License/*), плюс проект `GT2.DataSplitter.GTDT` (чтение контейнера; судя по имени, magic файлов - "GTDT" [L]). Релиз: [GT2DataSplitter v0.8](https://github.com/pez2k/gt2tools/releases/tag/GT2DataSplitter08). Альтернатива: `GT2DataExploder` (adeyblue, C++, MIT; только финальная игра, не демо).

Файлы: `gtmode_data.dat.gz` (машины+детали), `gtmode_race.dat.gz` (события), `*_unistrdb.dat.gz` (строки), `arcade_data.dat.gz`, `license_data.dat.gz`; языковые/региональные префиксы `eng_ fra_ ger_ ita_ spa_ usa_ jpn_` [H].

Таблицы [H]: ActiveStabilityControl, Brake, BrakeController, Car, Chassis, Clutch, Computer, Displacement, Drivetrain, Engine, EngineBalance, Flywheel, Gear, Intercooler, Lightweight, LSD, Muffler, NATune, PortPolish, PropellerShaft, RacingModify, Steer, Suspension, TireCompound, TireForceVol, TiresFront, TireSize, TiresRear, TractionControlSystem, TurbineKit, Wheel; события: EnemyCars, Event, Regulations; строки: CarNames, PartStrings. В аркаде дополнительно CarArcadeDrift/CarArcadeRacing, EnemyCarsArcade, TireCompoundsArcade.

Ключевая семантика (SUBMANIAC сопоставил поля с именами из GT6 SpecDB) [H как "гипотеза сообщества", не как подтверждённая кодом истина]:

- **Engine**: 16 точек torque curve (kgm x100) + 16 RPM-точек (x100), idle/redline/max RPM, PowerMultiplier, ID звука (.es), clutch release RPM.
- **Chassis**: развесовка, FrontGrip/RearGrip, mass, моменты сопротивления yaw/pitch/roll, wheelbase.
- **Suspension**: диапазоны camber/toe/ride height/spring rate, damper bound/rebound (2 уровня), stabilizer, bump rubber, lever ratio.
- **Drivetrain**: тип (FR/FF/4WD/MR/RR), 4WD behaviour (0 kei-4WD, 1 стандарт, 2 Subaru, 3 ATTESA, 254 не-4WD), VCD-диапазон, инерции.
- **LSD**: тип диффа F/R (коды 102/104/109/110/116/118/121), initial/accel/decel с диапазонами.
- **Gear**, **TurbineKit** (boost1/2, peak rpm, response, wastegate), **RacingModify** (BodyID -> другой .cdo, drag, downforce min/max/default, track width, вес), **Steer** (кривая угол руля от скорости, 6 точек; AI её игнорирует), **Lightweight**, **Brake**, **BrakeController**, простые множители мощности (Computer/Muffler/NATune/PortPolish/EngineBalance/Displacement/Intercooler).
- **TireForceVol**: grip по покрытиям - tarmac 100, guide 90, green/sand/gravel 60, dirt 80, water 80.
- **TireCompound**: `.dat` 0x40 байт - таблицы-кривые (weightgrip x/y 4 точки, sideforce x/y 8 точек, slipmu/sidemu A/B по 6 точек). Имена подобраны по аналогии с GT6, **смысл не подтверждён**.
- **Event**: трасса, до 16 кандидатов-соперников, laps, rolling start speed, лицензия, AI-множители grip/acceleration/throttle-lift по типу привода, **rubber band** (leading/trailing % и дистанции), износ шин, IsRally, ограничения (Regulations до 32 car ID, drivetrain, PS limit, NA/turbo/road/race flags), призовые, призовые машины (до 4, случайный выбор), TrackBannerPool. **Список событий захардкожен в коде** - добавлять нельзя.
- **EnemyCars**: полный сетап соперника (все детали + оверрайды настроек + PowerMultiplier).
- Аркада: сложность = cap ускорения 80/90/100% + rubberband.

Прочее: `.usedcar*` - 60 недель ротации UCD, цена до 0xFFFFFF; референс `GT2UsedCarEditor` [H]. `solodata.dat.gz` - данные экранов меню; `GT2SolodataEditor` [M]. `GT2ArcadeStatsEditor`, `GT2ArcadeCarLogoTool` - для аркадного диска [H - по списку директорий].

---

## 7. Меню и 2D

**Документированность: высокая для графики, средняя для логики. Референс: [`GMCreator`](https://github.com/adeyblue/GTVolTools/tree/master/GMCreator) (C#, MIT; файлы `Archiver.cs`, `Box.cs`, `Images.cs`, `HardcodedStructs.cs`, `HardcodedData.cs`), `GT2MenuSplitter` (pez2k).**

- `gtmenu/commonpic.dat` + `.idx`: фоновые слои экранов GT mode, общие для языков. `gtmenu/<lang>/gtmenudat.dat` + `.idx`: foreground-слои + описания кнопок/виджетов ("GM files") для тысяч экранов [H].
- Формат экрана: 512x504, разбит на **тайлы 16x8**; одноцветный тайл хранится как RGB без пикселей, многоцветный - как 4bpp-спрайт с одним из (до) 16 CLUT'ов [H].
- `.idx` = список offset'ов в `.dat` [H].
- Логика переходов между экранами, покупки, гараж - **в коде оверлеев**, GM-файлы дают только boxes/ссылки. `HardcodedData.cs` в GMCreator намекает, что часть привязок экранов захардкожена в EXE [M].
- Аркадный UI: набор **нестандартных TIM** (PD переставили CLUT *после* пикселей: пиксели с 0x14, CLUT'ы в конце; по 5-52 CLUT на атлас) - arc_goodies, arc_panels, arc_maker, arc_other, arc_key_config, gt_cursor, gt_items, gtmode_font, setting, title_item, topmenu_panels, license_tim, champtim. Размеры и offset'ы CLUT перечислены у SUBMANIAC [H]. `iconimg.dat.gz` - то же, но без заголовка. `game_status_file.gz` - HUD-атлас + 10 TIM тахометров (6k-18k rpm) [H].
- Нигде не описано: **раскладка HUD, координаты спрайтов в атласах** - это в коде.

---

## 8. Шрифты, текст, локализация

**Документированность: частичная.**

- `font/racefont.dat`: 512x256 4bpp, 3 CLUT с 0x00, без TIM-заголовка; 3 размера шрифта [H]. `arcade/arc_font.tim` (стандартный TIM, 3 CLUT), `arcade/gtmode_font.tim` (нестандартный, 5 CLUT, все языки) [H].
- **Метрики глифов (ID -> XY/ширина/CLUT) не найдены** - SUBMANIAC прямо пишет, что не знает, где они; `arcade/arc_fontinfo` - "no info" (очевидный кандидат) [H].
- Строки: `.text/data-race.txd`, `*_unistrdb.dat.gz` (CarNames, PartStrings - через GT2DataSplitter), `arcade/license_info_<lang>`, `.cclatain/.ccjapanese`. Инструменты: `StrEditGT2`, `StrEditGT2_demos` (pez2k) [H - по списку].
- Значительная часть текста GT mode - это **картинки** в gtmenudat (поэтому папки по языкам).

---

## 9. Звук

- **`engine/*.es` (magic "ENGN")**: заголовок (размеры, число сэмплов, offset параметров), на сэмпл 16 байт: fade-in RPM, pitch RPM, fade-out RPM, volume, sample rate (Hz/10), offset; сэмплы - Sony **VAG** ADPCM. Набор на машину: `xxxxx.es` (intake) + `_n0.._n3` / `_t0.._t3` (exhaust по стадиям глушителя, NA/turbo). Лимит 3399 файлов. Референс: [GranTurismoENGNEditor](https://github.com/TheAdmiester/GranTurismoENGNEditor) [H для layout из SUBMANIAC, M для инструмента].
- `sound/*.ins`: банки VAG (sys.ins меню, se01.ins гонка, arcade.ins, arcseq.ins/gtmseq.ins - инструменты для секвенций). Читаются PSound. Заголовок банка не описан (вероятно, производная от PsyQ VAB) [M].
- `sound/*.seq`: секвенции меню (spu_02..10 = Car Wash, East/North/South/West City, GT Foundation, My Home, Map, License; arcade.seq). Формат общий с GT1. Референс: [gtseq2midi](https://github.com/xan1242/gtseq2midi) [M].
- Гоночная музыка и FMV: XA/STR-стримы на диске вне VOL [L - по памяти + косвенно по OpenGTPS1].

---

## 10. Реплеи, демо, лицензии, сейвы

- **Реплей = запись ввода, воспроизводимая детерминированной физикой.** Прямые признаки: ghost-соперник в ралли "must be played at the same FPS as the recording, or desync" (SUBMANIAC); pez2k (2021) - attract-демо "effectively just a replay savegame on the disc", сетап машины закодирован внутри, hex-правки не удались ([GT2 Plus thread p.56](https://www.gtplanet.net/forum/threads/mod-gran-turismo-2-plus-bug-fixes-restored-content-and-new-content-beta-7-released.378282/page-56)) [H].
- Известно про layout в памяти: 0x10 байт заголовка, затем имя события (напр. `DON0001`), список машин (в записи машины: 0xE9 grid slot, 0xEA "existence": 02 ghost / 03 player1 / 04 player2), буфер ~0x905B байт [H, SUBMANIAC; только для .dgf].
- Файлы: `dirt/*.dgf`, `license/*.lgf.gz`, `arcade/demofile*.gmr(.gz)`, реплеи на memory card. **Спецификации формата нет ни у кого.**
- **Сейвы**: [GT2SaveEditor](https://github.com/zyzalfors/GT2SaveEditor) (Python; SC/MC/GME/PSV; поля: game id, checksum, language, days, money, races, wins, rankings, prize, licenses, garage, career/arcade progress) + `GT2SaveEditor`, `GT2SaveEditorGUI`, `GT2SaveChecksumFixer` у pez2k (C#). README GT2SaveEditor ссылается на старые описания формата в web.archive.org. Документированность: высокая для garage/прогресса [M - код не читал].

---

## 11. Матрица документированности

| Ассет | Статус | Лучший код-референс | Язык / лицензия |
|---|---|---|---|
| GT2.VOL (GTFS) | **полностью** | adeyblue `GT2VolFile.cs`; Nenkai `GTFS_VOL.bt` | C# MIT; 010 MIT |
| gzip-обёртки | полностью | стандартный zlib | - |
| GT2.OVL | **полностью** (контейнер) | CookiePLMonster `ovl.py`; pez2k GT2OVLTool | Python MIT; C# ? |
| CDO/CNO | ~90% (есть unknown-блоки LOD) | pez2k GT2ModelTool; Nenkai .bt | C# ?; MIT |
| CDP/CNP | ~95% | pez2k GT2TextureEditor; Nenkai .bt | C# ?; MIT |
| carwheel, carlogo, crsmap (TIM) | полностью | любой TIM-парсер | - |
| Нестандартные TIM-атласы UI | полностью (layout), нет карты спрайтов | SUBMANIAC PDF | - |
| carparam (структура) | **полностью** | pez2k GT2DataSplitter(Rewrite); adeyblue GT2DataExploder | C# ?; C++ MIT |
| carparam (семантика физики) | частично (имена по аналогии с GT6) | SUBMANIAC PDF | - |
| TireCompound / ASC / TCS | слабо | - | - |
| .carinfo/.carcolor/.cc* | полностью | pez2k GT2CarInfoEditorCSV | C# ? |
| .crsinfo | полностью | pez2k GT2CourseInfoEditor | C# ? |
| .crstims.tsd | полностью (с 02.2026) | pez2k GT2BillboardEditor | C# ? |
| .usedcar | полностью | pez2k GT2UsedCarEditor | C# ? |
| **.tro** | **частично (структура ~50%, семантика ~25%)** | только Nenkai `GTPS_tro_GTTrackObject.bt` | 010 MIT |
| .trp / .bsp | полностью (контейнер TIM) | - | - |
| .bso (skybox mesh) | слабо | - | - |
| Collision трассы | слабо (Boundaries найдены, смысл полей unknown) | Nenkai .bt | - |
| AI lines / replay cameras / pit / checkpoints | **нет** | - | - |
| gtmenudat / commonpic (графика) | высоко | adeyblue GMCreator | C# MIT |
| Логика меню | нет (в коде) | - | - |
| Шрифты (атласы) | полностью; **метрики глифов - нет** | - | - |
| Строки | высоко | pez2k StrEditGT2, DataSplitter | C# ? |
| .es ENGN | высоко | TheAdmiester ENGNEditor | ? |
| .ins банки | слабо (читаются PSound) | - | - |
| .seq | высоко | xan1242 gtseq2midi | C++ ? |
| Реплеи / .dgf / .lgf / .gmr | **нет** | - | - |
| Сейвы | высоко | zyzalfors GT2SaveEditor; pez2k GT2SaveEditor | Python ?; C# ? |
| Физика (формулы), AI-поведение | **нет** (только в MIPS-коде) | gt2-reversing (частичный disasm) | CC0 |

"?" = лицензия не указана/не найдена. Для pez2k/gt2tools стоит спросить автора напрямую (GTPlanet/GitHub issue) до заимствования кода; читать код как документацию формата можно в любом случае, а парсеры писать свои.

---

## 12. Соседние проекты, которые меняют план

| Проект | Подход | Статус (на 2026-09-17) | Лицензия |
|---|---|---|---|
| [GTTeancum/OpenGTPS1](https://github.com/GTTeancum/OpenGTPS1) | Static recomp (RecompOne) US Arcade+Sim дисков в Windows x64; свой C++/D3D11 "modern renderer": 4x, perspective-correct, max LOD, 60 Hz, widescreen (с дефектами); геометрия берётся **перехватом GTE/GPU-команд** (формат захвата OGTWCAP v4: model-space вершины + RT/TR матрицы + identity объекта), а не парсингом файлов; unified GT2.VOL, опциональный мердж контента GT1 | v0.9b, играбельно (гонки, чемпионаты, меню, memcard, XA). Коммиты 2026-08-21..2026-09-10, 71 коммит. README: разработка "entering a hiatus" | **лицензия не выдана** ("no license granted") - код читать можно, заимствовать нельзя |
| [SamiulH25/gt2-recomp](https://github.com/SamiulH25/gt2-recomp) | "Hybrid static recomp + incremental decomp" на базе psxrecomp; Sim disc US v1.2; OpenGL + SDL3; оверлеи через интерпретатор + GTFS reader; поэтапный порт модулей на нативный C (menu, render packets, card) | грузится до гаража, рендерит гонки; коммиты 2026-09-05..2026-09-11, 64 коммита | не указана |
| [ginryuoku/gt2-reversing](https://github.com/ginryuoku/gt2-reversing) | Классический matching-decomp: splat + maspsx + old-gcc, US v1.2 (SCUS-94488), 184 коммита; названы entrypoints оверлеев, начата поддержка C++-классов | последний коммит 2024-09-14 (2 года простоя); оверлеи не пересобираются | **CC0-1.0** |

Вывод: идея "перехватывать сцену на уровне GTE до проекции" (то, что пользователь уже делал в fighting-force-vr на уровне GP0 + PGXP) для GT2 уже доказана OpenGTPS1 как рабочая. Для VR это принципиально: нужны model-space вершины + матрицы, а не экранные координаты.

---

## 13. GAPS - чего нет в публичном доступе

1. **.tro целиком**: формат вершин чанков, UV road-полигонов, смысл `UnkStruct_0x9C`, `Entry_0x1C`, `Entry_0x20`, 22 int "positional data" в чанке, второй ShapeData.
2. **Коллизия**: как именно игра использует Boundaries + road tris (heightfield по треугольникам? 2D-границы + высота?). В шаблоне есть адреса функций - отправная точка.
3. **AI**: racing line, скорость по секторам, логика обгона. Известны только множители в Event/EnemyCars и rubber band.
4. **Replay-камеры** трасс, pit lane, checkpoints, стартовая процедура.
5. **Формат реплея** (.dgf/.lgf/.gmr/memcard) и требование детерминизма физики.
6. **Формулы физики**: шины (что значат кривые TireCompound), подвеска, аэродинамика, трансмиссия, LSD-коды, ASC/TCS. Только reverse кода.
7. **Метрики шрифтов** и карта спрайтов HUD/атласов.
8. **Логика GT mode**: экономика, разблокировки, лицензии (условия gold/silver/bronze лежат в license_data, но правила в коде), генератор случайных событий ("Event Synthesizer"), призовые машины.
9. **.bso** полигоны, `arcade/course_map(+info)`, `arc_fontinfo`, `arc_topmenu`, `.ins` заголовки.
10. Эффекты: дым/пыль/flare (есть текстуры, нет параметров), освещение машин (LightingArea зоны - где они в .tro?), env-map "отражения" (алгоритм UV по нормалям - в коде).
11. Отличия **JP Disc 1 / US v1.1 / v1.2 / PAL**: форматы те же, но offsets в EXE и содержимое баз отличаются; все community-адреса даны вперемешку для PAL (SUBMANIAC, Cheat Engine + ePSXe) и US v1.2 (decomp/recomp).

---

## 14. Рекомендации для плана (по части ассетов)

1. **Этап 0 - свой extractor** (`work/` под gitignore): ISO9660 -> GT2.VOL (GTFS) -> gunzip -> дерево файлов. Формат тривиален (раздел 2); написать на C++ или C# за вечер, сверяться с выводом GTVolTool. Сразу же GT2.OVL -> 6 оверлеев (gzip).
2. **Этап 1 - машины**: порт чтения CDO/CDP по `GT2ModelTool/Structures` + Nenkai .bt. Вывести в glTF для отладки. Это самый быстрый путь к "первой картинке" в своём рендере (и сразу в VR: машина на стенде).
3. **Этап 2 - .tro**: начать с шаблона Nenkai, написать свой дампер (чанки -> OBJ/glTF с цветом по SurfaceType). Параллельно **использовать эмулятор как оракул**: у пользователя есть hacked DuckStation и опыт GTE/GP0-capture - снять model-space вершины + матрицы на известной трассе (High Speed Ring = `testline`? проверить по `.crsinfo`) и сопоставить с байтами .tro. Адреса функций из шаблона (0x80027BBC, 0x80028588, 0x80027FC4; проверить, для какой версии) разобрать в Ghidra.
4. **Этап 3 - базы**: carparam читать по моделям GT2DataSplitterRewrite (достаточно read-only). Это даст ростеры, события, сетапы AI, физические параметры.
5. **Физика/AI/реплеи**: только через reverse кода. Выбрать ОДНУ целевую версию - **US Simulation v1.2 (SCUS-94488)**: под неё заточены gt2-reversing (CC0, символы, splat-конфиг), gt2-recomp и OpenGTPS1. Arcade v1.1 - вторично.
6. **Стратегическая развилка** (решать пользователю): (a) чистый reimplementation с нуля (долго: физика+AI+меню = годы); (b) **hybrid как у gt2-recomp**: рекомпилированный игровой код + свой рендер, постепенная замена модулей нативным кодом. Для VR вариант (b) даёт играбельный результат на порядок быстрее, а нативные парсеры форматов (этапы 1-3) нужны в обоих вариантах - для HD-ассетов, free camera, стерео и корректного culling'а (авторский PVS "сектор +-3" в VR при повороте головы недостаточен - нужен доступ ко всей геометрии трассы, т.е. нативный .tro-парсер).
7. **Лицензии**: MIT-источники (adeyblue, Nenkai, CookiePLMonster) можно заимствовать с атрибуцией; pez2k/gt2tools - без лицензии, использовать как документацию и/или спросить автора; OpenGTPS1 - явно без лицензии, только читать; gt2-reversing - CC0.
8. **Сообщество**: основные живые люди - pez2k, Nenkai, submaniac93 (GTPlanet + Discord GT Modding). По .tro стоит спросить Nenkai, нет ли неопубликованных наработок, прежде чем тратить недели.
9. Python не установлен, а половина экосистемы (GTModTools, GT2SaveEditor, splat, build-скрипты recomp-проектов) на Python - поставить Python 3.12+ и Ghidra.

---

## 15. Открытые вопросы

- Какова лицензия pez2k/gt2tools (нет LICENSE)? Разрешит ли автор порт структур в наш проект?
- Для какой версии игры указаны адреса подпрограмм в `GTPS_tro_GTTrackObject.bt`?
- Где в .tro лежат UV и вершины road-полигонов; есть ли в .tro AI-линия, или она в EXE/отдельном файле?
- Совпадает ли формат .tro у трасс, портированных из GT1 (Flag7 в .crsinfo), с "родными" GT2-трассами?
- Формат реплея: чистый input log + начальное состояние, или есть ключевые кадры? Фиксирован ли шаг физики (30/60 Гц) и зависит ли от региона (PAL 25/50)?
- Совпадают ли форматы JP Disc 1 (v1.0?) с US - особенно carparam (JP-релиз известен багами и другой базой)?
- Что делает OpenGTPS1 для widescreen/culling - правит ли PVS-логику игры? (читать `PORT_PLAN.md`, `TO-DO.MD`)
- Живы ли OpenGTPS1/gt2-recomp через 2-3 месяца; готовы ли авторы к коллаборации/лицензированию?
- Содержимое gt2.airesoft.co.uk (adeyblue) и неоткрытых мной страниц GTPlanet-треда (39+ страниц) - могут содержать заметки по .tro/AI.
