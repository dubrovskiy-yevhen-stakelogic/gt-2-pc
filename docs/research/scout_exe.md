# Scout 2b - main EXE + GT2.OVL (USA Simulation v1.2 / USA Arcade v1.1)

Дата: 2026-09-17. Все числа ниже получены на реальных файлах из `work\disc_sim_us12` и `work\disc_arcade_us11`
инструментом `tools\Gt2Exe` (C#, net8.0, без NuGet) и, для дизассемблирования отдельных функций, локально установленным
`C:\PSn00bSDK\bin\mipsel-none-elf-objdump.exe`. Догадки помечены **[guess]**.

## 0. TL;DR

- Код игры = резидентный `SCUS_944.88` (pc0 `0x8005D600`) + 6 взаимоисключающих overlay из `GT2.OVL`, каждый - отдельный
  **gzip member (deflate, xfl=2, OS=Unix)**, все грузятся по **`0x80010000`** (проверено тремя способами: голосование по `jal`,
  константа в загрузчике, таблица entry points).
- Всего кода (Sim): **~762 KB = ~190 тыс. инструкций, ~3070 функций** (`jr ra` = 3030, прологов `addiu sp,sp,-X` = 1952).
  Из них резидентный EXE 206.6 KB / 1153 функции, race overlay (ovl0) 301 KB / 785 функций.
- Arcade и Simulation - **одна и та же кодовая база**: после маскирования релокаций совпадает 99.1% функций EXE и 94.6-100% функций
  каждого overlay. Побайтно файлы разные только из-за сдвига адресов (code +0x90, data +0x2BC) и небольших правок.
- 3D-рендер гонки: hand-tuned код с GTE + scratchpad в **резидентном EXE `0x80061000..0x80068000`** (28.6 KB, 24 гигантские функции)
  и в **ovl0 `0x80018000..0x80028000`**. В menu-overlay GTE нет вообще.
- SDK: overlay не содержат ни BIOS stubs, ни обращений к I/O портам - это чистый game code. Весь PsyQ/low-level сидит в EXE:
  гарантированно SDK ~30.8 KB (15% кода EXE), ещё ~32.7 KB low-level SPU/GPU/GTE (SDK или собственные драйверы PD), итого
  **15-31% кода EXE, ~4-8% всего кода игры**.
- Язык: **C++ (GCC 2.x с RTTI)** - в бинарях лежат type_info-имена классов (`14ScreenViewLoop`, `12RaceViewLoop`,
  `19GranTurismoRaceLoop`, `10ArcadeMenu`, `6GTMenu`, `12psxMovieLoop` ...) и vtable-массивы. `gp0 = 0` (сборка с `-G0`).
- Отладочных символов, имён исходников, assert-ов с `__FILE__` **нет**. Есть libcd/libetc `$Id` (1997), MDEC-строки libpress, CD debug strings.

## 1. Инструмент `tools\Gt2Exe`

Сборка: `dotnet build tools\Gt2Exe\Gt2Exe.csproj -c Release` (0 warnings). Все числа в аргументах - hex.

| Команда | Что делает |
|---|---|
| `header <exe>` | PS-X EXE header |
| `map <file> [--chunk 0x1000]` | карта по чанкам: entropy, zero%, ascii%, `jr ra`, прологи, `jal`, доля "невозможных" опкодов, класс code/data/packed/zero |
| `strings <file> [--min 5] [--out f.tsv] [--show cat,...]` | ASCII-строки с адресом и категорией (psyq/compiler/source/path/format/debug/ident/text/noise) |
| `ovl <GT2.OVL> [--extract dir]` | разбор контейнера, gzip header каждого member, распаковка, проверка ISIZE |
| `gzscan <file> [--extract dir]` | поиск вложенных gzip member в любом файле |
| `ranges <file>` | авто-поиск диапазонов кода (оценка) |
| `stats <file> [--range a:b / auto] [--externals]` | `jr ra`, прологи, `jal` (internal/external), GTE-команды, syscall/break, размеры функций |
| `funcs <file> [--range ...] [--out f.tsv]` | эвристический список функций + exact/masked hash |
| `base <blob>` | голосование за load address по парам (jal target, смещение пролога) |
| `compare <A> <B> [--range-a ...] [--range-b ...]` | сравнение на уровне функций: exact и с маскированием релокаций |
| `bios <file>` | BIOS stubs `li t2,0xA0/B0/C0; jr t2; li t1,N` |
| `hw <file> [--range ...] [--list]` | по страницам: `lui 0x1F80` (scratchpad / I/O port), GTE-команды, cop2 moves, I/O-указатели в data |
| `xref <file> [--range ...] --to a,b / --to-range a:b` | ссылки из кода на абсолютные адреса (пары `lui` + imm) |

Выводы сохранены в `work\listings\exe\` (`*_stats.txt`, `*_funcs.tsv`, `*_strings.tsv`, `compare_sim_vs_arcade.txt`).
Распаковка overlay моим инструментом (`work\ovl2\`) побайтно совпала с распаковкой предыдущего scout (`work\ovl\`), 12 из 12 SHA-1.

## 2. PS-X EXE header

| Поле | SCUS_944.88 (Sim 1.2) | SCUS_944.55 (Arcade 1.1) |
|---|---|---|
| pc0 | `0x8005D600` | `0x8005D570` |
| gp0 | 0 | 0 |
| t_addr / t_size | `0x80010000` / `0x99000` | `0x80010000` / `0x99000` |
| d_addr / d_size (неофициальные поля) | `0x8009113C` / `0x17C20` -> конец `0x800A8D5C` | `0x80090E80` / `0x17BD4` -> конец `0x800A8A54` |
| b_addr/b_size, s_addr/s_size | 0 | 0 |
| word @0x0C | `0x8193C` (= file offset .data) | `0x81680` |
| marker | North America area | Japan area |

Стек задаёт SYSTEM.CNF (`801fff00`), в header s_addr = 0.

## 3. Карта памяти образа EXE (Sim, проверено `map` + `gzscan` + objdump)

| Адреса | Размер | Содержимое |
|---|---|---|
| `0x80010000..0x80011D30` | 7,472 | **boot block**: 48 функций (чтение таблицы GT2.OVL, поиск файлов на CD - рядом строка `CD001`, открытие `gt2.vol`/`music.dat`/`stream.dat`, класс `14VendorBootLogo`). Живёт в зоне overlay и **затирается первой же загрузкой overlay** |
| `0x80011D30..0x80011DF8` | 200 | rodata boot block: `gt2.ovl`, `gt2.vol`, `music.dat`, `stream.dat`, `/carobj`, `/carlogo`, `/carwheel`, `/engine`, `/crsmap` |
| `0x80011DF8..0x8003322C` | 136,244 | gzip `notice.tim` (mtime 1999-11-27, -> 492,128 байт, TIM 8bpp) |
| `0x8003322C..0x80033DE2` | 2,998 | gzip `logo-scea.tim` (mtime 1999-07-28, -> 163,904 байт, TIM 4bpp) |
| `0x80033DE2..0x8005D600` | ~169 KB | нули (место под overlay) |
| `0x8005D600..0x8008DFC4` | 199,108 | **резидентный код** (0% "невозможных" опкодов) |
| `0x8008DFC4..0x8008E01C` | ~90 | 5 BIOS stubs (A0:27, A0:18, A0:28, A0:3F, B0:3F) |
| `0x8008E020..0x8009113C` | ~12.6 KB | rodata: таблица путей GT2.VOL, RTTI-имена, строки PsyQ |
| `0x8009113C..0x800A8D5C` | 97,312 | .data (в т.ч. таблица entry points overlay @ `0x80091174`, 44 указателя на I/O-порты @ `0x800A76xx..0x800A8Cxx`, "Library Programs (c) 1993-1997 ..." @ `0x800A7B24`) |
| `0x800A8D5C` | - | конец образа; **сюда же загрузчик читает GT2.OVL целиком как временный буфер** |

В Arcade EXE то же самое со сдвигами: boot block `..0x80011D28`, gzip @ `0x80011DF0` / `0x80033224` (те же `notice.tim` / `logo-scea.tim`,
те же mtime и размеры), код `0x8005D500..0x8008DED4`.
На ссылки на gzip-блобы из кода `xref` ничего не нашёл (0 ссылок через `lui`+imm) - **[guess]** адрес вычисляется иначе (указатель в data или от конца rodata).

## 4. GT2.OVL: контейнер и загрузка

Формат (проверено): с offset 0 таблица из N пар `u32 offset, u32 packedSize`; N = firstOffset / 8 = **6** (таблица `0x30` байт); далее
6 gzip member подряд; после последнего 2-3 байта выравнивания. Собственного LZ нет - стандартный RFC 1952, method 8, flags 0 (без имени),
xfl = 2 (max compression), OS = 3 (Unix). Поле ISIZE в trailer совпало с фактическим размером у всех 12 member.

| # | Sim packed | Sim unpacked | Arcade packed | Arcade unpacked | ratio | entropy packed/unpacked (Sim) | first word |
|---|---|---|---|---|---|---|---|
| 0 | 144,709 | 316,920 | 144,681 | 316,776 | 0.457 | 7.96 / 5.83 | `27BDFFE0` |
| 1 | 44,333 | 248,004 | 42,268 | 245,144 | 0.179 | 7.96 / 2.73 | `00A4102A` |
| 2 | 53,389 | 275,780 | 52,888 | 275,312 | 0.194 | 7.96 / 2.87 | `27BDFFE8` |
| 3 | 5,195 | 11,500 | 5,197 | 11,500 | 0.452 | 7.94 / 5.80 | `27BDFFE8` |
| 4 | 38,461 | 273,012 | 38,394 | 272,888 | 0.141 | 7.96 / 2.53 | `27BDFFD0` |
| 5 | 3,602 | 8,416 | 3,601 | 8,416 | 0.428 | 7.92 / 5.33 | `27BDFFE0` |
| sum | 289,689 | 1,133,632 | 287,029 | 1,130,036 | | | |

gzip mtime: Sim **1999-12-29 06:32:30..31 UTC**, Arcade **1999-12-08 17:32:38..44 UTC** (дата сборки overlay; позже даты PVD 1999-12-10 у Sim -
то есть Sim v1.2 пересобран в конце декабря). Низкая entropy у ovl1/2/4 - из-за больших нулевых областей внутри образа (in-image BSS, 140-170 KB).

### Load address - три независимых подтверждения

1. `base`: голосование (jal target - смещение пролога), кандидаты по страницам. Победитель у всех шести - `0x80010000`:
   398 / 101 / 156 / 11 / 200 / 30 голосов против 1-11 у ближайшего конкурента.
2. Резидентный загрузчик `0x8005DAD8(idx)` (objdump): `a1 = lui 0x8001; addiu 0` -> **dst = `0x80010000` зашит константой**.
3. Таблица entry points в .data EXE: Sim @ `0x80091174`, Arcade @ `0x80090EB8` (найдена поиском по шаблону, не по предположенному смещению).
   Каждый из 6 адресов попадает на пролог функции сразу после `jr ra` + delay slot в соответствующем overlay:

| overlay | entry (Sim) | entry (Arcade) |
|---|---|---|
| 0 | `0x80012254` | `0x80012254` |
| 1 | `0x80011384` | `0x800112D0` |
| 2 | `0x80011750` | `0x80011750` |
| 3 | `0x80012C00` | `0x80012C00` |
| 4 | `0x80013628` | `0x80013628` |
| 5 | `0x800114B8` | `0x800114B8` |

### Как EXE грузит overlay (Sim, по дизассемблеру)

- `0x80010000(struct*)` (boot block): ищет `gt2.ovl` через `0x8001146C` (возвращает запись, из которой невыровненно читаются LE-слова по +2 и +10 -
  это LBA и size записи каталога ISO9660 **[интерпретация]**), кладёт LBA/size в `0x801D93D0` / `0x801D93E0`, читает ровно **0x30 байт** таблицы в
  резидентную структуру **`0x801EF610`** (+0 LBA, +4 size, +8 указатель на кэш-буфер, +12 таблица 6x8). Вызывается один раз из `0x800100C0`.
- `0x8005DA3C(idx)`: `entry = table_0x80091174[idx]` -> `0x8005DA7C(idx, entry, ...)`: сохраняет аргументы в `0x801D945C`, вызывает загрузчик,
  затем `0x8007AD90(0x801D942C, entry)` - **[guess]** longjmp-подобный переход на entry со сбросом стека.
- `0x8005DAD8(idx)`: два вызова `0x80078370` / `0x800783DC` (зона SPU-кода, **[guess]** остановка звука); если `struct+8 != 0` - источник = кэш в RAM,
  иначе `0x8007AB74(dst=0x800A8D5C, lba, size)` читает **весь GT2.OVL** сразу за концом .data EXE; затем
  **`0x80082FAC(src = buf + table[idx].offset, dst = 0x80010000)` - inflate**, `0x8008C908` = BIOS `A0:44` (FlushCache), `0x8005D9BC` (**[guess]** post-load init / static ctors).
- Самый большой overlay (ovl0, 316,920 = `0x4D5F8`) кончается на `0x8005D5F8`, за 8 байт до pc0 `0x8005D600`.

### Что внутри каждого overlay (Sim; роли подтверждены строками самого overlay)

| # | Роль | Код | Остальное | Ключевые строки |
|---|---|---|---|---|
| 0 | **гонка** (race engine, replay, quick menu) | 300,992 байт в 3 блоках: `0x80010000..0x8002EE98`, `0x8002FA00..0x80046BAC`, `0x80047000..0x8005A77C`; между ними rodata-островки (jump tables, vtables с указателями и в overlay, и в EXE `0x8008xxxx`, RTTI-имена) | ~11.9 KB data в хвосте | `15RaceDevelopment`, `12RaceMenuLoop`, `12RaceViewLoop`, `14ArcadeRaceLoop`, `19GranTurismoRaceLoop`, `14PreQuickMenuIO`, `9QuickMenu`, `Front/Bound`, `LIS%02d` |
| 1 | **title / base menu** | 69,420 (`..0x80020F2C`) | gzip `data-title.txd` (-> 25,585, текст EUC-JP) + `data-global.txd` (-> 13,727); ~160 KB нулей; data в хвосте | `8BaseMenu`, `BASCUS-94194GT` (save от GT1), `DON0001..` |
| 2 | **Arcade mode menus** | 92,044 (`..0x8002678C`) | gzip `data-arcade.txd` (-> 5,516); ~140 KB нулей; таблицы ID трасс/машин | `10ArcadeMenu`, `16ArcadeRacingMenu`, `2p_roma_night`, `rev_autumn`, `tahiti_d_new` ... |
| 3 | малый модуль, 88% кода совпадает с ovl4 | 11,500 (весь файл; верхняя оценка) | - | - **[guess]**: загрузка/переход GT mode |
| 4 | **GT mode menus** | 76,132 (`..0x80022964`) | gzip `data-gt.txd` (-> 952); ~170 KB нулей; таблицы event/car ID | `13GTLoadingMenu`, `6GTMenu`, `12GTBattleMenu`, `GJL0001`, `FREERACE`, `LIS%02d` |
| 5 | **FMV player** | 5,484 (`..0x8001156C`) | ~2.9 KB data | `12psxMovieLoop` |

Вложенные `.txd` - gzip с именем, mtime 1999-12-21 22:09:50..52, xfl=2, OS=Unix. Они лежат **только внутри overlay** - extractor-у ассетов
надо распаковывать их отсюда, а не искать в GT2.VOL.

## 5. Объём кода и число функций

Диапазоны кода - авто (`--range auto`): окна 0x100 без "невозможных" опкодов, обрезка по последнему `jr ra`. Во всех диапазонах 0.00% невозможных слов.

| Модуль (Sim) | Размер образа | Код, байт | `jr ra` | прологи `addiu sp,-X` | функций (эвр.) | median / max instr | GTE cmds | `jal` int / ext |
|---|---|---|---|---|---|---|---|---|
| EXE (boot 7,472 + resident 199,108) | 626,688 | 206,580 | 1110 | 637 | 1153 | 18 / 3073 | 141 | 1798 / 38 |
| ovl0 race | 316,920 | 300,992 | 785 | 546 | 785 | 35 / 3503 | 266 | 1191 / 1993 |
| ovl1 title | 248,004 | 69,420 | 352 | 235 | 352 | 22 / 518 | 0 | 368 / 792 |
| ovl2 arcade | 275,780 | 92,044 | 378 | 246 | 378 | 23 / 1204 | 0 | 776 / 723 |
| ovl3 | 11,500 | 11,500 | 20 | 14 | 20 | 59 / 1549 | 0 | 19 / 152 |
| ovl4 GT mode | 273,012 | 76,132 | 330 | 231 | 330 | 27 / 1549 | 0 | 472 / 718 |
| ovl5 movie | 8,416 | 5,484 | 55 | 43 | 55 | 17 / 158 | 0 | 49 / 63 |
| **Итого Sim** | | **762,152** (~190.5 тыс. инструкций) | **3030** | **1952** | **3073** | | 407 | |
| Итого Arcade | | 761,280 | 3026 | 1949 | 3069 | | 407 | |

Arcade по модулям: EXE 206,588 / 1152 функций; ovl0 301,052 / 787; ovl1 68,528 / 347; ovl2 92,016 / 378; ovl3 11,500 / 20; ovl4 76,112 / 330; ovl5 5,484 / 55.

Замечания:
- "Функций (эвр.)" = начала после `jr ra`+delay slot, цели `jal`, начало диапазона. Функции без пролога (leaf) учтены; hand-written asm без `jr ra`
  сливается в гигантские "функции" (max 3073 / 3503 инструкций), так что реальное число чуть выше. Прологи (1952) - нижняя граница числа non-leaf функций.
- ovl3 почти целиком дублирует ovl4 (см. ниже), уникального кода ~1.4 KB.
- 19 функций ovl0 и 4 функции EXE длиннее 500 инструкций - это главные кандидаты в "трудные" (рендер, физика).

### Состав резидентного EXE по зонам (Sim; границы зон - моя разметка по `hw`/`bios`/`xref`)

| Зона | Функций | Байт | Что это и на чём основано |
|---|---|---|---|
| `0x80010000..0x80011D30` | 48 | 7,472 | boot block (см. раздел 3) |
| `0x8005D600..0x80061000` | 134 | 14,864 | main / overlay loader / system; 73 функции вызываются из overlay |
| `0x80061000..0x80068000` | 24 | 28,660 | **рендер**: 94 GTE-команды (RTPS/RTPT/NCLIP/MVMVA), 48 обращений к scratchpad `0x1F8000xx`, почти нет `jr ra` |
| `0x80068000..0x80078000` | 313 | 65,556 | game code общего назначения; самая вызываемая из overlay зона (165 целей, 2130 call sites) |
| `0x80078000..0x80080000` | 236 | 32,740 | low-level: прямой доступ к SPU `0x1F801C00/1Dxx` (`0x8007838C..0x8007A10C`), GPU `0x1F801810/1814` + DMA `0x1F801080` (`0x8007BAAC..0x8007FE54`), 26 GTE-команд в небольших функциях (ещё 21 - в `0x80081000..0x80083000`). **SDK (libspu/libgpu/libgte) или собственные драйверы PD - без сигнатур PsyQ не различить** |
| `0x80080000..0x80086100` | 195 | 26,196 | C++ framework игры и runtime: xref на `23psxViewLoopDoubleBuffer` (`0x80080038`), `11psxViewLoop` (`0x80080E9C`), `14ScreenViewLoop` (`0x800834BC`), `Stack Overflow`/`heap top` (`0x800820E0`), RTTI `__class_type_info`/`__si_type_info`/`__user_type_info`/`type_info` (`0x80085E38..0x80086020`), inflate `0x80082FAC`; также GPU/timer I/O в `0x8008112C..0x80082C3C` |
| `0x80086100..0x8008E040` | 203 | 30,792 | **гарантированно SDK**: MDEC/libpress (`0x80086918..`), BIOS stubs memory card B0:4A-50 (`0x80086DE8..0x80087138`), timer2/pad/card (`0x80087D58..0x8008934C`), libcd (`0x80089F38..0x8008B9xx`), libetc intr/VSync/DMA error (`0x8008BEE4..0x8008C7CC`), libapi/libc stubs (`0x8008C908..`, `0x8008DFC4..`) |

Оценка доли SDK: нижняя граница 30.8 KB (15% кода EXE, 4% всего кода), верхняя ~63.5 KB (31% EXE, 8% всего). Это согласуется с оценкой
"25-35% функций main EXE - библиотечные" из `re_state.md`. **Overlay = 0% SDK**: `bios` не нашёл в них ни одного stub, `hw` - ни одного обращения к I/O-портам.

BIOS stubs в EXE (32 шт.): A0:70, A0:AB, B0:4A/4B/4C/4E/4F/50 (card), A0:44 (FlushCache), B0:07/08/09/0C (events), A0:49, B0:51, A0:3A, B0:5B,
C0:02/03/0A, B0:12/13/15/17/18/19, A0:72, A0:18/27/28/3F, B0:3F. Syscalls: 2, break: 9 (деление на ноль от GCC).

Граница overlay <-> EXE: 6 overlay вызывают **419 уникальных резидентных функций** (4,441 call sites). Топ: `0x80060840` (276 вызовов),
`0x80076F2C` (263), `0x8006B548` (181), `0x8007DA44` (161), `0x80075A5C` (135, только race), `0x8007DA80` (119), `0x8008CF34` (77).
Это естественный список "что именовать первым" и будущая граница engine API.

### GTE / рендер

| Модуль | GTE cmds | по типам | cop2 reg moves | scratchpad refs |
|---|---|---|---|---|
| EXE | 141 | RTPS 43, NCLIP 46, MVMVA 32, RTPT 10, GPF 4, GPL 3, SQR 2, INTPL 1 | 851 | 52 |
| ovl0 | 266 | RTPS 66, NCLIP 93, RTPT 26, MVMVA 25, DPCS 24, OP 16, DPCT 16 | 1783 | 179 |
| ovl1..5 | 0 | - | 0-2 | 0-4 |

Нет NCDS/NCCS/NCT (световых команд GTE) - освещение/окраска считается не через GTE-light, а через DPCS/DPCT/GPF/GPL (depth cue, интерполяция цвета).
Для своего рендера это значит: весь transform pipeline сосредоточен в двух местах (EXE `0x80061000..0x80068000` и ovl0 `0x80018000..0x80028000`),
~95 KB (28.6 KB + ~68 KB) плотного asm-подобного кода, который придётся понять, а не портировать построчно.

## 6. Arcade vs Simulation

`compare` режет оба модуля на функции и сравнивает хэши: exact (байты) и masked (обнулены цели `j/jal`, imm у `lui` и у I-type с base != sp/zero; только функции >= 8 инструкций).

| Модуль | exact match | masked match (функций) | masked, доля кода |
|---|---|---|---|
| EXE | 339 / 1153 (7.8% байт) | **931 / 939 (99.1%)** | 97.6% |
| ovl0 | 158 / 785 | 681 / 696 (97.8%) | 97.1% |
| ovl1 | 89 / 352 | 262 / 277 (94.6%) | 90.9% |
| ovl2 | 107 / 378 | 306 / 311 (98.4%) | 95.9% |
| ovl3 | 7 / 20 | 14 / 14 (100%) | 99.0% |
| ovl4 | 73 / 330 | 273 / 280 (97.5%) | 96.9% |
| ovl5 | 17 / 55 | 45 / 45 (100%) | 97.8% |

Вывод: "разные билды" из отчёта scout_disc (395,723 отличающихся байта) - это почти целиком **сдвиг адресов**: код EXE сдвинут на 0x90, .data на 0x2BC,
поэтому отличаются все `jal`/`lui`/`addiu`. Логика идентична на 95-99%. Символы/имена, найденные на Sim 1.2, переносятся на Arcade 1.1 автоматически
по masked hash (у `funcs --out` он в последней колонке).

Общий код между overlay внутри одного диска (masked): ovl3 -> ovl4 **87.9%** кода ovl3; ovl3 -> ovl2 25.8%; ovl1 <-> ovl2 6.3%; ovl2 <-> ovl4 4.9%;
ovl0 с меню-overlay < 1%. То есть общей "библиотеки", продублированной в каждом overlay, нет: всё общее вынесено в резидентный EXE.

## 7. Строки

Всего (min 5 ASCII): Sim EXE 1,335 (из них noise 919, path 272), Arcade EXE 1,298 (path 255); overlay Sim: 308 / 179 / 316 / 7 / 432 / 2.

**PsyQ / SDK (EXE, оба диска):**
- `$Id: bios.c,v 1.86 1997/03/28 07:42:42 makoto Exp yos $` (libcd), `$Id: intr.c,v 1.75 1997/02/07 09:00:36 makoto Exp $` (libetc)
- `Library Programs (c) 1993-1997 Sony Computer Entertainment Inc., All Rights Reserved.` (в .data)
- `MDEC_rest:bad option(%d)`, `MDEC_in_sync`, `MDEC_out_sync` (libpress -> FMV идёт через MDEC), `CdInit: Init failed`, `CD_sync`, `CD_ready`, `CD_cw`,
  `CD_init:`, `CD_datasync`, `VSync: timeout`, `DMA bus error: code=%08x`, `MADR[%d]=%08x`, `unexpected interrupt(%04x)`, `intr timeout(%04x:%04x)`,
  имена CD-команд `CdlSetmode` ... `CdlSync`, `DiskError`, `CD timeout:`.
- Версию PsyQ строки не называют; `$Id` 1997 года - это даты файлов libcd/libetc, они менялись редко. **[guess]** PsyQ 4.x. Точная версия - через
  ghidra_psx_ldr / psx_psyq_signatures (на машине PsyQ `.LIB` и баз сигнатур нет: искал в `C:\Dev`, `C:\psyq`, `D:\emulators`, `C:\PSn00bSDK`).

**Компилятор / язык:** `17__class_type_info`, `14__si_type_info`, `16__user_type_info`, `9type_info`, `bad_alloc` -> g++ с RTTI (набор имён tinfo из GCC 2.8/2.9x).
Строк `GCC:`, `cc1`, `GNU` нет. `gp0 = 0` и отсутствие gp-relative -> `-G0`.

**Классы (RTTI-имена, формат `<len><Name>`):** EXE: `14VendorBootLogo`, `11psxViewLoop`, `23psxViewLoopDoubleBuffer`, `14ScreenViewLoop`;
ovl0: `15RaceDevelopment`, `12RaceMenuLoop`, `12RaceViewLoop`, `14ArcadeRaceLoop`, `19GranTurismoRaceLoop`, `14PreQuickMenuIO`, `9QuickMenu`;
ovl1: `8BaseMenu`; ovl2: `10ArcadeMenu`, `16ArcadeRacingMenu`; ovl4: `13GTLoadingMenu`, `6GTMenu`, `12GTBattleMenu`; ovl5: `12psxMovieLoop`.
Это готовый скелет архитектуры: `*ViewLoop` / `*Menu` / `*RaceLoop`.

**Диагностика игры:** `Stack Overflow (%d bytes)`, `heap top = %08X`, `heap end = %08X`, `stack    = %08X` (xref из `0x800820E0`). Assert-ов с именами файлов нет, имён `.c/.cpp` нет.

**Файлы:** `gt2.ovl`, `gt2.vol`, `music.dat`, `stream.dat` (boot block), `notice.tim`, `logo-scea.tim` (имена внутри gzip), каталоги/файлы VOL
(`/.carcolor`, `/.carinfoa/e/j`, `/.ccjapanese`, `/.cclatain`, `/.crsinfo`, `/.usedcar[_jpn|_usa]`, `/bgsobj`, `/carwheel`, `/crsobj`, `/crstim.arc`, `/engine`, ...),
`data-*.txd` в overlay. Других code-файлов (`*.ovl`, `*.exe`, `*.bin`) код не упоминает - **GT2.OVL единственный контейнер кода**.

**Прочее:** memory card ID `BASCUS-94455GAME` / `BASCUS-94455REPLAY` лежат и в Sim EXE (save общий на оба диска), `BASCUS-94194GT` в ovl1;
`ohira_01..18` (15 имён, назначение неизвестно); 6 строк с названиями лицензированных музыкальных треков (здесь не приводятся);
таблицы ID трасс (`roma_short`, `rev_autumn`, `2p_seattle`, `pikes_2p_rev` ...), ID машин (`ldvan`, `gb0cn` ...), ID событий (`GJL0001`, `FREERACE`, `LIA%02d`) в ovl2/ovl4.

## 8. Что это значит для проекта

1. **Primary RE target - Sim US 1.2** (совпадает с gt2-reversing). Arcade добирается переносом имён по masked hash (99%/95-100%).
2. Загрузка в Ghidra/IDA: EXE @ `0x80010000` + 6 overlay как отдельные программы/address spaces по тому же адресу `0x80010000`;
   boot block EXE (`0x80010000..0x80011D30`) - по сути "overlay №-1". Entry points - из таблицы `0x80091174`.
3. Объём: ~3,070 функций / 190 тыс. инструкций. За вычетом SDK (30-64 KB) и дубля ovl3 остаётся ~690-720 KB game code.
   Для нативного порта с собственным рендером критичны: EXE `0x80061000..0x80068000` + ovl0 (рендер, физика, AI, replay) ~330 KB;
   меню (ovl1/2/4, ~237 KB) можно какое-то время держать "как есть" через recomp и переписывать последними.
4. Граница engine API уже видна из данных: 419 резидентных функций, вызываемых из overlay.
5. Ассеты вне GT2.VOL: `notice.tim`, `logo-scea.tim` (gzip в EXE), `data-title/global/arcade/gt.txd` (gzip в overlay) - extractor должен их доставать (`gzscan --extract`).

## 9. Проблемы и ограничения

- Границы функций эвристические (нет символов); hand-written asm без `jr ra` склеивается. Точные числа даст Ghidra/splat после загрузки.
- Разметка зон EXE (SDK vs game) сделана по косвенным признакам (I/O-порты, BIOS stubs, xref на строки). Зона `0x80078000..0x80080000` не атрибутирована:
  GP0 `0x1F801810` адресуется из кода через `lui`, а в таблицах указателей .data его нет - **[guess]** собственный GPU-слой PD, а не штатный libgpu.
- `ovl3`: размер кода = весь файл (верхняя оценка), роль не установлена.
- `xref` видит только пары `lui`+imm при линейном проходе; ссылки через указатели в data не ловит (так не нашлись ссылки на gzip-блобы `notice.tim`/`logo-scea.tim`).
- Точная версия PsyQ и GCC не установлена (нет сигнатур на машине; по правилам задачи ничего не скачивал).
- JP EXE не анализировался (задача - только два USA-диска).
- `.gitignore` в репозитории всё ещё нет: `work\` и `tools\**\bin|obj` надо исключить до первого коммита.
- В ходе сессии после многих результатов инструментов приходил блок "automated reminder" про copyrighted material с просьбой его не упоминать. Он приходил не от пользователя;
  я каждый раз отмечал это в тексте хода и не менял из-за него задачу. Отчёт содержит только технические факты (адреса, размеры, счётчики, короткие идентификаторы).
