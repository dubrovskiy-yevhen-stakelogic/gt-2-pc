# Scout 2a - GT2.VOL (GTFS): формат, извлечение, инвентаризация

Дата: 2026-09-17. Источник данных: `work\disc_sim_us12\GT2.VOL` (SCUS-94488 v1.2) и `work\disc_arcade_us11\GT2.VOL` (SCUS-94455 v1.1).
Все факты ниже проверены на реальных байтах, догадки помечены словом **Guess**.

## 1. Инструмент `tools\Gt2Vol`

C# net8.0, console, без NuGet, сборка 0 warnings / 0 errors. VOL открывается только на чтение.

```
dotnet build tools\Gt2Vol\Gt2Vol.csproj -c Release
Gt2Vol info      <GT2.VOL>                  header, счетчики, проверки целостности
Gt2Vol toc       <GT2.VOL>                  сырые TOC records
Gt2Vol list      <GT2.VOL>                  offset / size / time / path
Gt2Vol extract   <GT2.VOL> <outDir>         полное извлечение + gunzip рядом (".gz" срезается)
Gt2Vol inventory <GT2.VOL> <report.md>      каталоги, типы, magic, 16-байтные сэмплы
Gt2Vol diff      <A.VOL> <B.VOL> <report.md> сравнение по путям и содержимому
Gt2Vol split     <x.dat> <x.idx> <outDir>   распаковка контейнеров dat+idx (gtmenudat, commonpic)
```

Файлы: `Gt2Vol.csproj`, `Program.cs`, `GtfsVolume.cs` (парсер), `Commands.cs` (info/toc/list/extract), `Inventory.cs`, `VolDiff.cs`, `IdxSplit.cs`.

Правила extract: файл с gzip magic `1F 8B 08` распаковывается рядом; если имя кончается на `.gz` - суффикс срезается, иначе пишется `<name>.ungz`. `.dat`, у которого есть соседний `.idx`, не распаковывается одним потоком (это контейнер из тысяч gzip members - для него команда `split`). Время файлов выставляется из TOC.

## 2. Формат GTFS (проверен на обоих дисках)

Источники описания формата (прочитаны при подготовке этой заметки): `adeyblue/GTVolTools` (`GT2VolFile.cs`) и QuickBMS-скрипт `gran_turismo_2.bms` из `RetingencyPlan/le_quickbms_script_compendium`. В `pez2k/gt2tools` отдельного инструмента для GT2.VOL **нет** (есть GT3VOLExtractor и GT1ArchiveTool, но не GT2) - проверено по листингу корня репозитория.

Little-endian:

| Offset | Тип | Значение |
|---|---|---|
| 0x00 | char[4] | `GTFS` |
| 0x04 | u32 | 0 |
| 0x08 | u16 | offsetCount - число записей offset table (Sim 11,581 / Arcade 10,621) |
| 0x0A | u16 | recordCount - число 32-байтных TOC records (Sim 11,620 / Arcade 10,644) |
| 0x0C | u32 | 0 |
| 0x10 | u32[offsetCount] | offset table |

Запись offset table: биты 31..11 = номер 2048-байтного блока начала, биты 10..0 = число байт padding в конце последнего блока этой записи. `size[i] = start[i+1] - start[i] - pad[i]`.

- entry 0 = сам header + offset table (Sim: size 46,340 = 0x10 + 11,581*4 - совпало точно)
- entry 1 = TOC (Sim: 371,840 = 11,620*32 - совпало точно)
- entry 2.. = данные файлов; последняя запись = sentinel, равна длине VOL (Sim 0x1D19F800, Arcade 0x0CBB3800 - совпало)

Поправка к предыдущему scout: значение 764 (Sim) / 508 (Arcade) по адресу 0x10 - это не счетчик, а padding самой таблицы (entry 0 = 0x000002FC).

TOC record (32 байта): `u32 unix timestamp`, `u16 index`, `u8 flags`, `char[25] name` (NUL padded).
- flags `0x01` = directory, `0x80` = последняя запись текущего каталога.
- Для файла `index` = номер записи в offset table. Для каталога `index` = номер первой TOC record этого каталога. Каждый каталог (кроме корня) начинается с записи `..` (flags 0x01, index = первая запись родителя). Проверено: `.text` -> 28, record 28 = `..` index 0; `arcade` -> 30.

Проверки `info` на обоих VOL: offsets монотонны, 0 unreferenced, 0 multi-referenced data entries, 0 отрицательных размеров, 0 недостижимых TOC records. 6 файлов нулевого размера (каталог `replay`).

| | Simulation US 1.2 | Arcade US 1.1 |
|---|---:|---:|
| Длина VOL | 488,241,152 | 213,596,160 |
| Каталогов | 21 | 13 |
| Файлов | 11,578 | 10,618 |
| Байт данных (stored) | 476,286,985 | 203,124,010 |
| gzip members | 5,205 (+ контейнеры gtmenudat) | 4,905 |
| Payload после gunzip (без split) | 405,745,719 | - |
| gzip magic без имени `.gz` | 91 | 3 |
| Ошибок gunzip | 0 | 0 |
| Timestamps (UTC) | 1999-07-18 .. 1999-12-28 17:39 | 1999-07-18 .. 1999-12-08 16:14 |

### Вложенный контейнер dat+idx (gtmenu)

`idx`: `u32 count`, затем `u32 offsets[count+1]`; последний offset = длина `.dat`. Биты 1..0 offset = padding в конце записи (выравнивание на 4), остальное = позиция. Проверено командой `split`: все members распакованы, 0 ошибок, весь padding нулевой.

| Контейнер | Entries | Содержимое | Payload |
|---|---:|---|---:|
| `gtmenu/commonpic.dat` | 458 | несжатые, magic `GTMP`, размеры кратны 0x1000 | 44,724,224 |
| `gtmenu/usa/gtmenudat.dat` | 3,386 | gzip, magic `47 4D 03 00` (`GM`) | 103,770,748 |
| `gtmenu/fra/gtmenudat.dat` | 3,381 | то же | 123,501,852 |
| `gtmenu/ger/gtmenudat.dat` | 3,383 | то же | 121,005,696 |
| `gtmenu/ita/gtmenudat.dat` | 3,385 | то же | 124,259,168 |
| `gtmenu/spa/gtmenudat.dat` | 3,379 | то же | 125,861,092 |

## 3. Куда извлечено

- `work\disc_sim_us12\vol\` - 11,578 файлов + 5,205 распакованных (702,930,937 байт всего)
- `work\disc_arcade_us11\vol\` - 10,618 файлов + 4,905 распакованных (413,829,770 байт)
- `work\disc_sim_us12\vol_split\gtmenu\{commonpic,usa,fra,ger,ita,spa}\...\NNNN.bin` + `_manifest.tsv` (643,799,884 байт)
- `work\listings\`: `sim_us12_vol_toc.tsv`, `arcade_us11_vol_toc.tsv`, `*_vol_list.tsv`, `*_vol_inventory.md`, `vol_diff_arcade_vs_sim.md`

## 4. Дерево каталогов (Simulation; только прямые потомки)

| Каталог | Файлов | Stored | Payload (gunzip) | Что это |
|---|---:|---:|---:|---|
| `/` | 12 | 504,639 | 774,975 | таблицы: `.carcolor .carinfoa/e/j .ccjapanese .cclatain .crsinfo .crstims.tsd.gz .usedcar[_jpn/_usa] crstim.arc` |
| `/.text` | 1 | 44,947 | 44,947 | `data-race.txd` - строки HUD/гонки |
| `/arcade` | 66 | 4,566,304 | 5,893,919 | UI Arcade mode, title screens, demo replays |
| `/bgsobj` | 68 | 853,464 | 1,830,232 | небо/фон: 34 `.bso` + 34 `.bsp` |
| `/carlogo` | 3,346 | 21,146,706 | = | TIM-логотипы/названия машин |
| `/carobj` | 4,440 | 40,585,430 | 140,419,552 | модели и текстуры машин |
| `/carparam` | 29 | 1,784,777 | 7,310,940 | таблицы параметров (`GTDT`) и строковые БД (`WSDB`) |
| `/carwheel` | 192 | 233,472 | = | TIM дисков, все по 1,216 байт |
| `/crsmap` | 120 | 45,349 | 560,640 | TIM карты трасс |
| `/crsobj` | 252 | 31,419,855 | 63,980,852 | трассы: 126 `.tro` + 126 `.trp` |
| `/dirt` | 81 | 89,316 | 2,993,436 | 81 файл по 36,956 байт (тот же magic, что `.lgf`) |
| `/engine` | 2,747 | 107,609,232 | = | звук двигателей `ENGN` |
| `/font` | 1 | 65,536 | = | `racefont.dat` |
| `/gtmenu` (+5 языков) | 22 | ~266 MB | см. раздел 2 | весь GT Mode UI |
| `/license` | 180 | 159,332 | 6,652,080 | `.lgf.gz`, все по 36,956 байт |
| `/replay` | 6 | 0 | 0 | пустые placeholder: `scea/scee/scei .000/.999` |
| `/sound` | 15 | 986,212 | = | 5 `INST` + 10 `SEQG` |

## 5. Типы файлов и fingerprints (payload = после gunzip)

| Тип | Кол-во (Sim) | Первые 16 байт (пример) | Наблюдение |
|---|---:|---|---|
| `.cdo.gz` / `.cno.gz` | 1,110 + 1,110 | `47 54 02 00 00 00 ...` (`GT\x02\0`) | модель машины, 11,048..20,000 байт. **Guess:** d = day, n = night |
| `.cdp.gz` / `.cnp.gz` | 1,110 + 1,110 | `06 00 31 34 36 62 6C 71 00 ...` | всегда ровно 45,984 байт; u16 число цветов + однобайтные коды цветов. **Guess:** палитры + текстура |
| `.tro.gz` | 126 | `40 28 23 29 47 54 2D 50 53 00 ...` (`@(#)GT-PS`) | геометрия трассы, 77,768..696,792; дальше таблица offsets |
| `.trp.gz` | 126 | `9A 00 00 00 10 00 00 00 08 00 00 00 2C 00 00 00` | u32, затем байты TIM header (0x10, flags 8, CLUT len 0x2C). **Guess:** texture pack трассы |
| `.bso.gz` | 34 | `42 47 00 00 ...` (`BG`) | объект неба |
| `.bsp.gz` | 34 | `28 00 00 00 10 00 00 00 08 00 00 00 2C 00 00 00` | как `.trp` - текстуры неба |
| `.tim` / `.tim.gz` | 3,576 / 129 | `10 00 00 00 08/09 00 00 00 ...` | стандартный PS1 TIM (4/8 bpp с CLUT) |
| `.es` | 2,747 | `45 4E 47 4E 00 00 00 00 50 00 00 00` (`ENGN`) | 4,080..90,080 байт |
| `.ins` | 5 | `49 4E 53 54 ...` (`INST`) | банки инструментов/сэмплов |
| `.seq` | 10 | `53 45 51 47 ...` (`SEQG`) | секвенции |
| `.dat.gz` (carparam) | 22 из 39 | `47 54 44 54 6C 00 3E 00 F8 01 00 00 ...` (`GTDT`) | u16, u16 число таблиц, затем пары (offset, size) |
| `*_unistrdb.dat.gz` | 7 | `0E 54 00 00 57 53 44 42 53 03 ...` (`WSDB`) | UTF-16LE строки с u16 длиной; usa: 0x353 = 851 строка |
| `.lgf.gz` и `dirt/*` | 180 + 81 | `00 00 02 01 01 02 00 01 02 02 ...` | фиксированные 36,956 байт. **Guess:** ghost/demo replay для лицензий и rally |
| `.gmr` / `.gmr.gz` | 1 / 4 | `53 43 13 0F 82 66 82 73 82 51 ...` | `SC` + Shift-JIS текст - формат memory card save (demo replays) |
| `.carinfo*` | 3 | `43 41 52 00 56 04 00 00 ...` (`CAR\0`) | u32 count = 0x456 = 1,110 (= числу машин), 8-байтные записи, затем строки |
| `.crsinfo` | 1 | `43 52 53 00 02 00 7E 00 ...` (`CRS\0`) | u16 count = 0x7E = 126 (= числу `.tro`), в конце string table |
| `.carcolor` | 1 | `43 43 4F 4C 30 30 00 00` (`CCOL00`) | таблица цветов |
| `.usedcar*` | 3 | `55 43 41 52 00 00 00 00` (`UCAR`, внутри gzip) | used car dealer |
| `crstim.arc` | 1 | `40 28 23 29 47 54 2D 41 52 43` (`@(#)GT-ARC`) | u16 1, u16 6 entries, тройки (offset, size, size), внутри TIM |
| `.crstims.tsd.gz` | 1 | `5A 00 0A 00 2F 00 0F 00 ...` | 159,072 байт |
| `.idx` + `.dat` | 6 пар | см. раздел 2 | `GTMP` картинки и `GM` экраны меню |
| `font/racefont.dat` | 1 | `00 00 00 80 94 D2 FF FF ...` | 65,536 байт |
| `.txd` | 1 | `4C 61 70 00 ...` | 2,296 NUL-separated ASCII строк, max 47 символов |

## 6. Счетчики по категориям (Simulation / Arcade)

| Категория | Simulation | Arcade |
|---|---:|---:|
| Машины: уникальных ID (5 символов) | 1,110 | 1,110 |
| Машины: файлы модели (`cdo`+`cno`) / текстуры (`cdp`+`cnp`) | 2,220 / 2,220 | 2,220 / 2,220 |
| `carlogo` TIM | 3,346 (1,114 ID-префиксов) | 2,708 |
| `carwheel` TIM | 192 | 192 |
| Трассы `.tro` / `.trp` | 126 / 126 | 125 / 125 |
| `crsmap` TIM | 120 | 119 |
| Небо `.bso` / `.bsp` | 34 / 34 | 34 / 34 |
| Меню: `arcade/` | 66 | 49 |
| Меню: `gtmenu/` (файлов / экранов `GM` usa / `GTMP` картинок) | 22 / 3,386 / 458 | нет |
| Таблицы `carparam` | 29 | 12 |
| Таблицы в корне | 12 | 10 |
| Звук: `engine` `.es` | 2,747 | 2,747 |
| Звук: `sound` (`INST` + `SEQG`) | 5 + 10 | 5 + 10 |
| `license` `.lgf` | 180 (60 x `a_`/`e_`/`i_`) | нет |
| `dirt` | 81 (9 x 9 префиксов `aa_..ic_`) | нет |

Имена: суффиксы `carlogo` - `l--`/`m--` по 1,085, `p--`/`q--` по 312, `n--`/`o--` по 276. `engine` - 305 файлов `NNNNN.es`, 1,220 `NNNNN_tN.es`, 1,220 прочих `NNNNN_*`, плюс `ene_n.es`, `ene_t.es`. Последний символ car ID: `n` = 564, `r` = 506, `s` = 33, `t` = 4 (**Guess:** normal / racing modification / special).

## 7. Diff Arcade vs Simulation

- Общих путей 10,618; **только в Arcade: 0**; только в Simulation: 960.
- Идентичны побайтно: 10,532; идентичны только после gunzip (другой gzip header): 28; реально разное содержимое: 58.
- Arcade VOL по путям - строгое подмножество Simulation VOL. Для реимплементации достаточно брать ассеты с Simulation диска (плюс STREAM.DAT с Arcade).

| Каталог | Только Sim | Идентичны | Payload идентичен | Разные |
|---|---:|---:|---:|---:|
| `/` | 2 | 2 | 0 | 8 |
| `/.text` | 0 | 0 | 0 | 1 |
| `/arcade` | 17 | 36 | 6 | 7 |
| `/bgsobj` | 0 | 66 | 1 | 1 |
| `/carlogo` | 638 | 2,690 | 0 | 18 |
| `/carobj` | 0 | 4,424 | 4 | 12 |
| `/carparam` | 17 | 8 | 1 | 3 |
| `/carwheel`, `/engine`, `/font`, `/sound`, `/replay` | 0 | все | 0 | 0 |
| `/crsmap` | 1 | 111 | 8 | 0 |
| `/crsobj` | 2 | 234 | 8 | 8 |
| `/dirt`, `/license`, `/gtmenu/*` | 81 / 180 / 22 | - | - | - |

Только в Simulation (кроме bulk): `.usedcar_jpn`, `.usedcar_usa`, европейские UI-текстуры `arc_goodies_*`, `gt_cur_*`, `l-sel`, `langsel`, `license_info_*`, локализованные `carparam/{eng,fra,ger,ita,spa}_*`, трасса `checker`, весь `gtmenu`.
Разное содержимое: все корневые таблицы, `data-race.txd`, `arc_panels_*.tim`, `demofile_eu.gmr.gz`, 18 `carlogo` (`gv48*`, `gv4g*`, `n2sk*`), 4 машины (`bvasr`, `effwr`, `ufmun`, `uftar`), 8 трасс (`lic_seattle`, `speed`, `tahiti_d_new*`, `tahiti_test7`, `test_s2`, `test_z`), `usa_gtmode_race`, `usa_license_data`, `usa_unistrdb`. Полный список - `work\listings\vol_diff_arcade_vs_sim.md`.

## 8. Файлы, раскрывающие внутренние имена

- `.crsinfo` - string table содержит и display names, и внутренние ID трасс (`dart_test2`, `l_20`, `spL1..3`, `test_20/30/40`, `test_hs2`, `billboard-check`).
- Имена `crsobj/*.tro.gz` - 126 внутренних имен трасс, включая тестовые (`test_*`, `Gtest`, `cloudtest`, `circle30/80`, `maxspeed`, `bill_checker`, `checker`) и варианты `2p_*`, `rev_*`, `l_*`/`L_*` (license).
- `.carinfoe/a/j` - 1,110 записей: ID, коды цветов, название машины; ключ для сопоставления 5-символьных имен `carobj`.
- `carparam/*_unistrdb.dat` - UTF-16 строки названий деталей и пр.
- `carparam/gtmode_data_dev.dat.gz` (payload 315,006) и `gtmode_race_dev.dat.gz` (payload 32 байта) - dev-остатки; присутствуют в VOL (вопрос предыдущего scout закрыт).
- `.text/data-race.txd` - 2,296 строк HUD.
- `arcade/demofile*.gmr` - header формата memory card save с Shift-JIS заголовком.

## 9. Внешние ресурсы, увиденные при подготовке этой заметки

- `github.com/adeyblue/GTVolTools` - C# парсер GT2 VOL + `GT2DataExploder` (прочитан `GT2VolFile.cs`).
- `github.com/RetingencyPlan/le_quickbms_script_compendium` - `gran_turismo_2.bms`.
- `github.com/pez2k/gt2tools` - листинг корня: `GT2ModelTool`, `GT2TextureEditor`, `GT2TextureConverter`, `GT2DataSplitter`, `GT2DataSplitterRewrite`, `GT2MenuSplitter`, `GT2OVLTool`, `GT2CarInfoEditor`, `GT2CourseInfoEditor`, `GT2BillboardEditor`, `GT2UsedCarEditor`, `GT2SolodataEditor`, `StrEditGT2` и др. (содержимое не читал).
- Только из поисковой выдачи, **не проверено**: `GTTeancum/OpenGTPS1` (заявлен как static-recompilation port GT2 для Windows x64), `ginryuoku/gt2-reversing`, `SamiulH25/gt2-recomp`. Стоит отдельно изучить - прямо пересекается с целью проекта.

## 10. Проблемы и открытые вопросы

- В `work\disc_sim_us12\vol\gtmenu\<lang>\` остались 5 файлов `gtmenudat.dat.ungz` от первого прогона extract - это только первый gzip member; правильные данные в `vol_split`. Инструмент уже исправлен (такие контейнеры пропускает), файлы я не удалял.
- Внутренние форматы (`GT\x02` модель, `GT-PS` трасса, `GTDT`, `ENGN`, `INST`/`SEQG`, `GM`/`GTMP`) только отпечатаны по header - это задача следующих scouts.
- JP VOL не извлекался (в `work\disc_arcade_jp` нет GT2.VOL).
- `.gitignore` для `work\` и `tools\**\bin|obj` по-прежнему отсутствует.
- На протяжении сессии к результатам инструментов был приписан блок "automated reminder" про copyrighted material с просьбой его не упоминать. Он пришел не от пользователя; я отмечал его в тексте и не менял из-за него задачу. Отчет содержит только технические факты.
