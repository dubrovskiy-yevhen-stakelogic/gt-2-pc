# GT2: внутреннее устройство движка и различия версий

Дата исследования: 2026-09-17. Тема: как GT2 устроен внутри (рендер, симуляция, структура исполняемых файлов, меню/"скрипты", арт-пайплайн PS1) и чем отличаются ревизии.

Условные обозначения достоверности: **[H]** высокая (первичный источник, открыт при подготовке этой заметки), **[M]** средняя (вторичный источник / пересказ / вывод из кода), **[L]** низкая (по памяти, догадка, единичное утверждение на форуме).

> Данные TCRF ниже взяты только из поисковых сниппетов и помечены [M]. Содержимое PDF SUBMANIAC при подготовке этой заметки не проверено.

---

## 0. Главный вывод (TL;DR)

1. **GT2 не имеет скриптовой VM.** Вся логика - нативный MIPS-код (GCC + PsyQ, частично C++), поведение задается таблицами данных (carparam-базы, GM-страницы меню, таблицы событий). "Восстанавливать скрипты" не придется; придется (а) парсить таблицы и (б) либо декомпилировать/рекомпилировать нативный код, либо переписывать его.
2. **Мы не первые.** Уже существуют: статическая рекомпиляция GT2 под Windows x64 с собственным D3D11-рендером ([OpenGTPS1](https://github.com/GTTeancum/OpenGTPS1), v0.9b, "playable but not finished"), гибридный recomp ([gt2-recomp](https://github.com/SamiulH25/gt2-recomp)) и matching-декомпиляция ([gt2-reversing](https://github.com/ginryuoku/gt2-reversing), splat, CC0). Все три стандартизированы на **NTSC-U v1.2 Simulation (SCUS-94488)** - ровно тот образ, что есть у пользователя.
3. Форматы машин (CDO/CNO + CDP/CNP), VOL, меню (GM30/GTMP), базы данных и сейвы **хорошо задокументированы инструментами** pez2k и adeyblue. Формат трасс (.tro/.trp) публично **не документирован** - это главный пробел по ассетам.
4. Симуляция жестко привязана к кадру: штатно шаг гонки = 2 VBlank (30 Гц NTSC), переключается на 1 VBlank патчем (60 Гц), но тогда ломаются реплеи и "призраки" ралли. Для VR (90 Гц) нужен развязанный рендер с интерполяцией трансформов.

---

## 1. Структура: диски, исполняемые файлы, оверлеи

### 1.1 Два диска

- Simulation (GT mode) disc: `SCUS_944.88` + `GT2.OVL` + `GT2.VOL` + стримы. Arcade disc: `SCUS_944.55`, тоже с шестью оверлеями. [H] ([OpenGTPS1 ARCADE_INTEGRATION.md](https://raw.githubusercontent.com/GTTeancum/OpenGTPS1/main/docs/ARCADE_INTEGRATION.md))
- По данным OpenGTPS1: Arcade `GT2.VOL` содержит 10 618 именованных записей; Simulation добавляет 960 имен и является строгим надмножеством по именам, но **86 общих файлов различаются содержимым** (текстовая база гонок Arcade, панели, car data, course data). Подстановка sim-версий ломает HUD/результаты в Arcade. `MUSIC.DAT` побайтно идентичен. [H] (там же)
- Прогресс Arcade и лицензии лежат в разных диапазонах RAM у двух exe (базы `0x801C9340` Arcade против `0x801C98E0` Simulation; Arcade-прогресс `+0xB8`, 0x160 байт; лицензии `+0x1418`). [M] (там же; адреса из одного проекта)
- Существует мод "Combined Disc" от Silent (CookiePLMonster), объединяющий оба диска в один; поддерживает все версии кроме NTSC-J 1.0; требует Python 3.8+ и GTVolTool. [H] ([GT2-Combined-Disc](https://github.com/CookiePLMonster/GT2-Combined-Disc))

### 1.2 Главный exe (US 1.2 sim)

По splat-конфигу gt2-reversing ([scus_944.88.yaml](https://raw.githubusercontent.com/ginryuoku/gt2-reversing/master/config/gt2_us12_simdisk/scus_944.88.yaml)) [H]:

| Сегмент | Файл. смещение | VRAM | Комментарий |
|---|---|---|---|
| header | 0x0 | - | PS-X EXE заголовок 0x800 |
| ovr0 | 0x800 | 0x80010000 | "boot overlay": sysinit, vsync handler, инициализация GTFS/VOL, загрузчики carobj/carwheel, показ лого |
| ovr0_padding | 0x247F0 | 0x80033FF0 | bss |
| gt2main | 0x4DE00 | 0x8005D600 | резидентное ядро: гонка, меню-ядро, GT mode helpers, SPU, CD, карта памяти, gzip, replay |

- Общий размер ~0x99800; образ занимает 0x80010000-0x80099000; стек 0x801FFF00 (SYSTEM.CNF), глобальные переменные в районе 0x801Dxxxx. [M] ([gt2-recomp ARCHITECTURE.md](https://raw.githubusercontent.com/SamiulH25/gt2-recomp/main/docs/ARCHITECTURE.md); согласуется с адресом 60fps-чита 0x801D5864)
- Именованные символы (имена придуманы автором декомпиляции, но отражают назначение): `gt2_load_overlay` (0x8005DA7C), `gt2_main_gzip_decompress` (0x800847D0), `gt2_vsync_handler` (0x80010928), `gt2_sysinit_vsync_setup` (0x80010954), `gt2_main_race_timer` (0x80068B04), `gt2_main_race_func25` (0x800747D0), `gt2_main_gtmode_get_part` (0x80077D5C), `gt2_main_func51_replay` (0x800696C4), `gt2_callback_double_buffer_flip` (0x80080038), хелперы фиксированной точки `gt2_fxpoint_multi8/12/16`. [H] ([mainexe_symbol_addrs.txt](https://raw.githubusercontent.com/ginryuoku/gt2-reversing/master/config/gt2_us12_simdisk/mainexe_symbol_addrs.txt))
- Много функций названы `*_taskNNN` - код организован как набор "задач" (кооперативный планировщик/конечные автоматы). Точная семантика планировщика не описана. [L]
- Компилятор: старый GCC (decompals/old-gcc), PsyQ около 4.4, сборка через maspsx; есть следы C++ (type_info, vec new/delete). LIBPRESS (MDEC) для видео. [M] ([README gt2-reversing](https://raw.githubusercontent.com/ginryuoku/gt2-reversing/master/README.md))
- Бинарники датированы ноябрем-декабрем 1999. [H] (там же)

### 1.3 GT2.OVL - шесть оверлеев

- `GT2.OVL` = конкатенация gzip-потоков, 6 членов; все грузятся **по одному адресу 0x80010000** (поверх boot-сегмента ovr0) и сменяют друг друга. [H] ([gt2-recomp OVERLAYS.md](https://raw.githubusercontent.com/SamiulH25/gt2-recomp/main/docs/OVERLAYS.md), [gt2_01.yaml](https://raw.githubusercontent.com/ginryuoku/gt2-reversing/master/config/gt2_us12_simdisk/gt2_01.yaml))

| Член | Сжатый | Распакованный |
|---|---|---|
| gt2_01 | 144 709 | 316 920 (0x4D5F8 - ровно до начала gt2main 0x8005D600) |
| gt2_02 | 44 333 | 248 004 |
| gt2_03 | 53 389 | 275 780 |
| gt2_04 | 5 195 | 11 500 |
| gt2_05 | 38 461 | 273 012 |
| gt2_06 | 3 602 | 8 416 |

- **Какой номер за какой режим отвечает - надежно не установлено.** Silent прямо называет три функциональных оверлея: "Arcade menus overlay, GT mode menus overlay, race mode overlay" (и что метрические патчи не потребовали правок main exe). [H] ([GTPlanet, modding thread p.20](https://www.gtplanet.net/forum/threads/modding-and-restoration-discussion.374974/page-20)). По сниппету TCRF, GT-mode оверлей содержит массив из 248 ID событий, зеркалирующий `gtmode_race.dat`, из них 25 - плейсхолдеры для запуска лицензий и Machine Test из меню. [M]. По символам в gt2-reversing есть оверлей с `start_replay`, оверлей с `load_event`/`load_license`, оверлей с `load_global_menu_overlay`, но сопоставление с номерами сделано автоматическим пересказом и ненадежно. [L]
- Побайтно пересобрать GT2.OVL сложно (gzip-археология, таймстампы), но игра терпима к содержимому, если соблюдены релокации и самый большой оверлей не налезает на `start()`. [H] (README gt2-reversing). В gt2tools есть `GT2OVLTool`. [H] ([pez2k/gt2tools](https://github.com/pez2k/gt2tools))
- Практическое следствие для чит-патчей: коды Silent детектируют загруженный оверлей условными кодами по значениям в памяти; небезопасный способ патчинга ломал загрузку оверлеев. [H] ([Silent's blog, codes pack](https://silentsblog.com/2021/09/06/gran-turismo-2-codes-pack/))

### 1.4 GT2.VOL (GTFS)

- Сигнатура `GTFS`; таблица 32-битных LE-смещений с 0x10; первое смещение = 0x2FC... (описание gt2-recomp; в нем есть внутренние противоречия по адресу каталога: 0xBE7C vs 0xBB80) ; каталог из 32-байтных записей (hash?, 16-битный индекс, имя до 20 символов); выравнивание 2048; многие файлы заранее сжаты gzip. На sim-диске VOL начинается с LBA 473. [M] ([GTFS.md](https://raw.githubusercontent.com/SamiulH25/gt2-recomp/main/docs/GTFS.md)). Эталон реализации - C#-код [GTVolTools](https://github.com/adeyblue/GTVolTools) (MIT) [H].
- Дерево VOL (по [GT Modding Hub, File Structure](https://nenkai.github.io/gt-modding-hub/ps1/gt2/file_structure/)) [H]:
  - `carobj/` - `<car>.cdo.gz` (день), `.cno.gz` (ночь), `.cdp.gz` / `.cnp.gz` (палитры/текстуры день/ночь)
  - `crsobj/` - `<course>.tro.gz` (меши, камеры, стартовые позиции, данные для AI) и `.trp.gz` (текстуры)
  - `bgsobj/` - небо: `.bso.gz` (3D-объект), `.bsp.gz` (текстуры)
  - `carparam/` - `arcade_data.dat.gz`, `gtmode_data.dat.gz`, `gtmode_race.dat`, `license_data.dat`, `unistrdb.dat`
  - `gtmenu/<lang>/` - `gtmenudat.dat/.idx`, `iconimg.dat.gz`, `solodata.dat.gz`; общий `commonpic.dat/.idx`
  - `engine/` - звуки двигателей (стадии n0-n3 / t0-t3), `sound/` - .seq/.ins
  - `carwheel/`, `crsmap/` (миникарты .tim.gz), `dirt/` (данные "призраков" ралли), `license/` (.lgf.gz - демо-заезды), `replay/`, `font/racefont.dat`, `arcade/` (UI .tim), `carlogo/`
  - служебные: `.carcolor`, `.carinfo`, `.cc<locale>`, `.crsinfo`, `.usedcar`, `crstim.arc`

---

## 2. "Скрипты": есть ли байткод?

**Вывод: признаков скриптовой VM нет [M-H].** Основания:
- В символах декомпиляции (main + 6 оверлеев) нет интерпретаторов; логика - нативные функции/задачи. ([gt2-reversing config](https://github.com/ginryuoku/gt2-reversing/tree/master/config/gt2_us12_simdisk))
- Меню GT mode - **data-driven страницы**, а не скрипты (см. ниже).
- События, соперники, регламенты, призы, лицензии - таблицы в `carparam/*.dat` (инструмент [GT2DataSplitter](https://github.com/pez2k/gt2tools/tree/master/GT2DataSplitter): структуры ArcadeData, GTModeData, GTModeRace (EnemyCars, Regulations, ...), LicenseData, строковые таблицы ASCII/Unicode). [H]
- Лицензионные тесты: параметры в `license_data.dat` + демо-заезды `.lgf`; логика зачетов - нативный код. [M]

### 2.1 Как работают меню GTMenuDat (по [Manual.txt GMCreator](https://raw.githubusercontent.com/adeyblue/GTVolTools/master/GMCreator/Manual.txt)) [H]

- Экран = фон (**GTMP**, из `gtmenu/commonpic.dat`) + передний план (**GM-файл, сигнатура GM30**, из `gtmenu/<lang>/gtmenudat.dat`) + иконки из `iconimg.dat`.
- Холст 512x504; реально рисуется до y=490. Передний план - 16-битное изображение из тайлов 16x8, максимум 32 группы по 16 цветов, один тайл - одна группа.
- Интерактив = **боксы** (прямоугольники) со свойствами: `Contents` (около 200 значений: кнопки, имя/мощность/вес машины, цена, баланс, список гаража, список б/у машин, 3D-витрина `CarDisplay`, результаты лицензий IC1..S10, статистика Status и т.д.), поведение, `QueryAttributes` (условия/фильтры для гоночных экранов), `LinkToScreen` (индекс GM-файла в gtmenudat, с нуля). У экрана есть `BackLink` и флаг "назад как в браузере".
- Часть элементов **захардкожена в exe** (позиции `UsedCarList`, `GarageCarList`, `EquippedPartsList`, шрифты, цвета) - в GMCreator вынесено в `hardcoded.json`, зависит от версии/языка.
- Т.е. меню - это "гипертекст": страницы + типизированные виджеты, чью семантику реализует нативный обработчик в оверлее меню. Для порта: либо исполнять оригинальный код (recomp), либо реализовать ~200 типов содержимого заново.
- Ограничение UCD: около 364 записей б/у машин на неделю, грузится только текущая неделя. [M] (submaniac93, [GTPlanet p.20](https://www.gtplanet.net/forum/threads/modding-and-restoration-discussion.374974/page-20))

---

## 3. Рендер

### 3.1 Общая схема (по [OpenGTPS1 MODERN_RENDERER.md](https://raw.githubusercontent.com/GTTeancum/OpenGTPS1/main/docs/MODERN_RENDERER.md)) [H для факта наличия в документе, M для обобщений]

- Классический PS1-конвейер: GTE (перспективное деление, `NCLIP` для backface culling; квады проверяются как две половины), ordering tables, порядок "художника", **без Z-буфера**.
- Используются 8 типов примитивов: F3, F4, G3, G4, FT3, FT4, GT3, GT4; прозрачность и эффекты идут в авторском порядке команд GPU.
- В exe около 1 239 инструкций COP2 (GTE). [M] (gt2-recomp ARCHITECTURE.md)
- Вид в зеркале заднего вида рендерится **в том же кадре отдельным проходом со своим состоянием проекции**.
- HUD/тахометр/миникарта/текст - экранные спрайты без GTE; тонкие линии тахометра и турбо выводятся через кадр (чередование).
- 2D-слой (меню, загрузки, Results, видео MDEC) - отдельный "композитор команд".

### 3.2 Трассы

- Геометрия трассы - "резидентные" меш-объекты с 14-битными индексами; на трассу есть **таблицы секторов**, выбирающие взаимоисключающие LOD-копии и вспомогательную геометрию; сектор - ячейка 4096 единиц в локальных координатах модели. Полный каталог мешей не является валидным набором видимости (в списках есть взаимоисключающие, перекрытые и прокси-поверхности). [H] (OpenGTPS1)
- Дальность прорисовки - радиальная отсечка; **в реплеях дальность больше, чем в гонке** - это используют и чит Silent ("higher draw distance"), и OpenGTPS1. [H] ([Silent's blog](https://silentsblog.com/mods/gran-turismo-2/))
- Небо (на примере из OpenGTPS1): открытый цилиндр, 353 вершины / 257 примитивов, нетекстурированный градиент + три плоскости изображений из `bgsobj/*.bsp`; CLUT неба GT2 лежит в VRAM около (624,498..500), у GT1 - (0,488..490). [H] ([SSR11_RENDERING.md](https://raw.githubusercontent.com/GTTeancum/OpenGTPS1/main/docs/SSR11_RENDERING.md))
- Варианты файлов трассы: обычная вперед/реверс, Arcade-вариант (`_a`), вариант для 2 игроков (`2p_*`, урезанный), и пр.; OpenGTPS1 при конвертации GT1-трассы генерирует шесть вариантов. [M] (SSR11_RENDERING.md + сниппет TCRF про `2p_autumn`)
- Публичной спецификации `.tro/.trp` не найдено. В gt2tools есть только `GT2BillboardEditor` и `GT2CourseInfoEditor`; GT Track Viewer (Leo2236, 2014) работает с GT1 `COURSE.DAT`, 14 из 63 трасс, исходников нет. [H] ([GTPlanet](https://www.gtplanet.net/forum/threads/gt-track-viewer-a-tool-to-view-gran-turismo-tracks.317691/))
- Освещение трасс: запеченные vertex colors (Gouraud-примитивы G*/GT*), ночные трассы - отдельные ассеты/палитры. [L] (по памяти + косвенно из набора примитивов)

### 3.3 Машины: CDO/CNO + CDP/CNP

По исходникам [GT2ModelTool](https://github.com/pez2k/gt2tools/tree/master/GT2ModelTool/GT2ModelTool/Structures) [H] и постам pez2k/submaniac93 ([GTPlanet p.15](https://www.gtplanet.net/forum/threads/modding-and-restoration-discussion.374974/page-15)) [H]:

- Сигнатура `GT\x02`; в заголовке радиус/ширина передних и задних колес, 4 позиции колес; по смещению 0x868 - число LOD (обычно **3**) с порогами дистанции и смещениями; затем LOD-ы, затем **меш тени**.
- LOD: 16-битные счетчики (вершины, нормали, треугольники, квады, UV-треугольники, UV-квады), bbox (int16), поле scale. Вершины адресуются **одним байтом** - не более ~255 вершин на LOD. Нормали - 9-битные индексы на вершину. У полигона: 5 бит render order, флаги рендера, байт типа грани со значениями 0x20/0x25/0x28/0x2D (совпадают с кодами GPU-примитивов PS1), цвет грани BGR.
- **Весь CDO не может превышать 0x5000 (20 480) байт**: в RAM машины лежат в фиксированных слотах, 20 481-й байт - уже начало следующей машины (AI1). [H] (submaniac93)
- Колеса не входят в модель - генерируются игрой, текстуры из `carwheel/`. В рендере: кузов + 4 независимые группы колес со своими трансформами. [H]
- Текстура машины: **256x224, 4bpp, до 16 палитр по 16 цветов**; каждая грань хранит ID палитры; несколько деталей могут делить палитру. Цветовые варианты машины = наборы палитр в CDP/CNP. Ночные версии - отдельные `.cno/.cnp`. [H]
- Порядок отрисовки граней - иерархия в файле + байт приоритета (нет Z-буфера). [H]
- Отражения/блики: "на поверхностях без нормалей отражений нет" (GTPlanet p.15) - т.е. эффект строится от нормалей вершин. Конкретная техника (env-map-проход полупрозрачными текстурированными полигонами с UV из нормалей) - **по памяти, не подтверждено** [L].
- Полигонаж: "в среднем ~300 полигонов" на машину GT1/GT2 (Cam De Bastiani, [X](https://x.com/camdebastiani/status/1476193462580039686)); на форумах встречается "~500" для GT2. [M/L]
- LOD-поведение: машина игрока всегда в высшем LOD, AI агрессивно переключаются между тремя моделями по дистанции; принудительный максимум LOD переполняет штатные буферы полигонов, Silent удваивает их за счет 8 МБ dev-RAM. [H] ([Silent's blog](https://silentsblog.com/2021/09/06/gran-turismo-2-codes-pack/)). OpenGTPS1 расширил geometry buffer до 0x70000 байт. [H]
- Лимит числа кузовов: GT2+ упирается в предел движка, "700 машин не будет". [M] ([GT2+ thread](https://www.gtplanet.net/forum/threads/mod-gran-turismo-2-plus-bug-fixes-restored-content-and-new-content-beta-7-released.378282/page-57))

### 3.4 Широкий экран

Куллинг и вьюпорты заданы отдельно для меню, гонки, зеркала, экранов Arcade/GT mode/2P - единого масштаба нет, Silent патчит каждый контекст. [H] ([21:9 cheat](https://github.com/CookiePLMonster/Console-Cheat-Codes/blob/master/PS1/Gran%20Turismo%202/21x9%20Widescreen/NTSC-U%201.2.cht))

---

## 4. Симуляция

### 4.1 Тик и кадр

- Штатно гонка идет в 30 fps: шаг времени гонки и ожидание планировщика = 2 поля NTSC. 60 fps-чит (asasega, доработан Silent) меняет оба значения на 1; ключевая переменная - `0x801D5864` (2 -> 1) для US 1.2. [H] ([60 FPS cht](https://github.com/CookiePLMonster/Console-Cheat-Codes/blob/master/PS1/Gran%20Turismo%202/60%20FPS/NTSC-U%201.2.cht), OpenGTPS1 MODERN_RENDERER.md)
- В режиме "1 поле" игра сама отключает дым из-под колес, небо в зеркале и зеркало - в коде есть ветки под 60 Гц (вероятно, наследие Hi-Fi режима GT1); чит включает их обратно. [H]
- При 60 fps **ломаются сохраненные реплеи и призраки ралли** - т.е. реплей = детерминированное воспроизведение записанных данных с фиксированным шагом. [H] (Silent)
- Физика - целочисленная/фиксированная точка (хелперы 8/12/16 бит дробной части); OpenGTPS1 перечисляет подсистемы: подвеска, контакт шин, трансмиссия, столкновения, тайминг, круги. [M]
- GT1 Hi-Fi (60 fps) - только в GT1: 3 трассы с урезанной детализацией, одна машина, без зеркала и дыма. В GT2 отдельного Hi-Fi режима нет. [M] ([GT Wiki / GTPlanet](https://www.gtplanet.net/forum/threads/there-was-is-a-60fps-mode-in-gt1-lets-take-a-look-at-it.394768/))

### 4.2 AI

- Два вида rubber banding: (1) по дистанции игрока до каждого соперника, (2) масштабирование между соперниками; submaniac93 убрал оба в своем моде. [M] ([GT2+ thread p.53](https://www.gtplanet.net/forum/threads/mod-gran-turismo-2-plus-bug-fixes-restored-content-and-new-content-beta-7-released.378282/page-53))
- Соперники в ванили заданы не реальными деталями, а множителями мощности + шинами (таблицы EnemyCars в `gtmode_race.dat`). [M]
- Данные для AI (траектория и пр.) лежат в `.tro`. [M] (сниппет TCRF)
- **Соперник в ралли - не AI, а записанный призрак** из каталога `dirt/` VOL; данные призрака не содержат параметров деталей, игра подгружает соперника из `gtmode_race.dat` (отсюда баг рассинхрона на Tahiti Maze); призраки несовместимы между регионами. [M] (сниппет TCRF Bugs)

### 4.3 Повреждения, шины, пит-стопы, 2P

- Повреждения (сбитые углы колес/рулевое) - опция только на Arcade-диске; на Simulation-диске опции нет, код, возможно, присутствует. [M] ([GTPlanet](https://www.gtplanet.net/forum/threads/can-someone-mod-gt2-and-enable-damage-on-suspension-even-in-simulation-mode.397026/))
- Износ шин: только endurance и опция в Arcade; 4 индикатора над спидометром (синий -> ... -> красный); по наблюдениям игроков скорость износа не зависит от компаунда. [L-M] (GameFAQs/GTPlanet)
- 2 игрока: split-screen, отдельные урезанные файлы трасс `2p_*`, часть трасс/реверсов недоступна. [M]

---

## 5. Данные

- **Количество машин** (по данным, извлеченным adeyblue из v1.0 каждого региона): JP 613, US 618, PAL 621. "~650" - это записи с учетом дублей/спец-версий; GT2+ на пределе числа кузовов. [H] ([gt2.airesoft.co.uk](http://www.gt2.airesoft.co.uk/))
- carparam-таблицы деталей (по документу SUBMANIAC, из сниппета): Brake, BrakeController, Car, Chassis, Clutch, Computer, Displacement, Drivetrain, Engine, EngineBalance, Flywheel, Gear, Intercooler, Lightweight, LSD, Muffler, NATune, PortPolish, PropellerShaft, RacingModify, Steer, Suspension, TireCompound, TireForceVol, TiresFront, TireSize, TiresRear, TCS, ASC, TurbineKit, Wheel. [M] ([PDF](https://nenkai.github.io/gt-modding-hub/ps1/gt2/documents/Gran_Turismo_2_files_full_documentation.pdf))
- Региональные особенности данных: JP - мощность в PS, цены x100; US - вес в фунтах. [H] (airesoft)
- **Сейв**: гараж - 164 байта на машину; для NTSC упоминаются смещения: дни 0x378, деньги 0x7F08, число машин 0x3EF4, контрольная сумма гаража 0x7F1C. [L-M] (сниппет старого GT2 Editor). Инструменты: `GT2SaveChecksumFixer`, `GT2SaveEditor(GUI)` у pez2k; [zyzalfors/GT2SaveEditor](https://github.com/zyzalfors/GT2SaveEditor) (форматы SC/MC/GME/PSV). [H]
- Музыка: длины треков захардкожены "где-то неочевидно" (pez2k). [M]

---

## 6. Версии и ревизии

| Версия | Примечания | Достоверность |
|---|---|---|
| NTSC-J 1.0 | Arcade-диск собран 28.11.1999; максимум 98.2%; битый символ шрифта; Mark Martin Taurus | M |
| NTSC-J 1.1 | можно 100%; мелкие правки моделей/текстур; исправлен символ | M |
| NTSC-U 1.0 | сборка 8.12.1999; 98.2%; баг отображения требований лицензии у Muscle Car Cup | M |
| NTSC-U 1.1 | сборка 11.12.1999 (через 3 дня), выдавалась как замена; все еще 98.2%; исправлен LOD1 Vauxhall Astra RM | M |
| NTSC-U 1.2 | сборка конца декабря 1999 (Greatest Hits); 100%; Taurus RM заменен на generic-ливрею; "почти идентична PAL" | M |
| PAL | вышла последней, большинства ошибок нет; 621 машина | M |

Источники: [GTPlanet: JP 1.0 vs 1.1](https://www.gtplanet.net/forum/threads/differences-between-japanese-1-0-and-japanese-1-1-releases.383723/), сниппеты [TCRF Revisional Differences](https://tcrf.net/Gran_Turismo_2/Revisional_Differences), [GT2+ thread](https://www.gtplanet.net/forum/threads/mod-gran-turismo-2-plus-bug-fixes-restored-content-and-new-content-beta-7-released.378282/page-34).

**На чем стандартизировано сообщество: NTSC-U 1.2 (и PAL).** GT2+ (US) базируется на 1.2; gt2-reversing, gt2-recomp и OpenGTPS1 - на SCUS-94488 v1.2. Чит-коды Silent покрывают US 1.0-1.2, JP 1.0-1.1, PAL. [H]

У пользователя: Simulation v1.2 (идеально) и **Arcade v1.1**. OpenGTPS1 ожидает Arcade SCUS-94455 размером 729 423 408 байт - нужно сверить хеш/размер нашего образа; ревизия Arcade-диска, на которую рассчитаны адреса в сторонних проектах, может отличаться. [M]

---

## 7. Арт-пайплайн PS1: что влияет на модернизацию

- Машины: 4bpp + 16 CLUT по 16 цветов, 256x224, палитра на грань. Для нового рендера: при загрузке разворачивать в RGBA-атлас на каждую цветовую схему (или шейдерный CLUT-lookup, чтобы сохранить палитровые эффекты вроде стоп-сигналов). Есть нормали вершин -> можно честное освещение/кубмапы вместо фейка.
- Геометрия машин: int16-координаты + scale на LOD, 3 LOD + тень; для PC всегда LOD0.
- Трассы: int-координаты, сектора 4096 ед., запеченные цвета вершин, 2D-биллборды (есть `GT2BillboardEditor`), небо - цилиндр + плоскости. Единицы/масштаб "в метрах" публично не задокументированы - вывести из радиуса колес в CDO и длины трасс. [L]
- PS1-артефакты, которые уберет собственный рендер: аффинное текстурирование (OpenGTPS1 восстанавливает перспективу из состояния GTE на вершину), целочисленное дрожание вершин, сортировка OT (понадобится Z-буфер + сохранение авторского порядка для прозрачностей/декалей), pop-in LOD и дальности.
- Экран меню 512x504 (interlaced hi-res), гонка - в низком разрешении; меню - пререндеренные 2D-страницы, в VR их придется показывать на виртуальном экране.

---

## 8. Следствия для нашего плана

1. **Базовая версия проекта: SCUS-94488 v1.2.** Все адреса/символы/коды сообщества под нее. Arcade v1.1 проверить на совместимость; при необходимости ориентироваться на Combined Disc.
2. **Не писать симуляцию с нуля на старте.** Физика/AI нигде не описаны, зато нативный код доступен тремя путями: (a) статическая рекомпиляция (доказано OpenGTPS1 - играбельно от меню до реплеев), (b) matching-декомпиляция (gt2-reversing, медленно, но CC0), (c) гибрид. Рекомендуемая стратегия: recomp/гибрид для логики + **свой рендер, подключенный выше GP0** - на уровне "рисуем модель X с матрицей M" (у OpenGTPS1 это "provenance": указатель модели, состояние GTE, мировые координаты до проекции). Это принципиально лучше GP0-захвата из fighting-force-vr: для VR нужны мировые координаты, стерео и head-tracking, а не экранные треугольники.
3. **OpenGTPS1 нельзя форкать как код**: лицензия не выдана ("No license has yet been granted"), проект на паузе. Использовать как источник знаний (docs) и доказательство осуществимости. gt2-reversing (CC0) и GTVolTools (MIT) - можно использовать.
4. **Ассет-конвейер**: VOL (GTVolTools) -> gunzip -> CDO/CDP (GT2ModelTool/GT2TextureEditor, форматы известны) -> свой загрузчик оригинальных файлов в рантайме (данные игры не распространять, читать с образа пользователя). Трассы `.tro/.trp` - первоочередная RE-задача (Ghidra + символы gt2-reversing + поведение, описанное в MODERN_RENDERER.md).
5. **Тайминг для VR**: симуляция 30 Гц (или 60 Гц патчем ценой реплеев/призраков, записанных на 30) + рендер 90 Гц с интерполяцией трансформов машин/камеры. OpenGTPS1 уже наткнулся на нюансы интерполяции колес (ограничение шага > 45 градусов).
6. **Меню**: GM30/GTMP полностью редактируемы (GMCreator). В режиме recomp меню работают "как есть" через 2D-композитор; для VR - виртуальный экран.
7. Инструменты, которые надо поставить: Python 3 (splat, скрипты OpenGTPS1/Combined Disc), Ghidra + PSX-загрузчик, .NET (gt2tools - C#), 010 Editor (шаблоны Nenkai) по желанию.

---

## 9. Открытые вопросы

- Точное соответствие gt2_01..06 режимам (гонка / меню GT / меню Arcade / replay theatre / лицензии / ...). Проверить в DuckStation: брейкпойнт на `gt2_load_overlay` (0x8005DA7C).
- Полная спецификация `.tro/.trp` (меши, сектора, LOD-селекторы, коллизия, AI-линия, камеры реплеев, стартовая решетка).
- Как именно сделаны отражения/блики на кузовах и стоп-сигналы (палитровый трюк?).
- Формат реплея и `.lgf`/`dirt` призраков (ввод или трансформы?).
- Внутреннее устройство физики: шаг, модель шин, где лежат коэффициенты (TireForceVol и др.).
- Прочитать PDF SUBMANIAC (нужен инструмент для PDF) и TCRF-страницы вручную в браузере.
- Совпадает ли наш Arcade v1.1 с тем образом, на который рассчитан OpenGTPS1/Combined Disc.
- Единицы измерения мира (сколько единиц в метре) у машин и трасс.

---

## 10. Ресурсы (все открыты/увидены при подготовке этой заметки)

| Ресурс | Что это | Лицензия / статус |
|---|---|---|
| [GTTeancum/OpenGTPS1](https://github.com/GTTeancum/OpenGTPS1) | static recomp GT2 -> Win x64, D3D11, 60 Гц, widescreen; docs: PORT_PLAN, MODERN_RENDERER, ARCADE_INTEGRATION, SSR11_RENDERING | лицензии нет; v0.9b, пауза; 63 звезды, 71 коммит |
| [ginryuoku/gt2-reversing](https://github.com/ginryuoku/gt2-reversing) | splat-декомпиляция US 1.2 sim, символы main + 6 оверлеев | CC0-1.0; 184 коммита; Linux-сборка |
| [SamiulH25/gt2-recomp](https://github.com/SamiulH25/gt2-recomp) | гибрид recomp + интерпретатор оверлеев; docs GTFS/OVERLAYS/ARCHITECTURE | лицензия не указана; 1 звезда, 64 коммита; документы местами гипотетичны |
| [pez2k/gt2tools](https://github.com/pez2k/gt2tools) | 57 утилит C#: GT2ModelTool, GT2TextureEditor, GT2DataSplitter, GT2OVLTool, GT2MenuSplitter, GT2SaveEditor, GT2BillboardEditor... | "research and personal use", просит связаться при переиспользовании |
| [pez2k/gt2mods](https://github.com/pez2k/gt2mods) | компоненты GT2+ | некоммерческое, 667 коммитов |
| [adeyblue/GTVolTools](https://github.com/adeyblue/GTVolTools) | VOL extract/rebuild, GMCreator, GTMP, GT2DataExploder | MIT |
| [GT Modding Hub (Nenkai)](https://nenkai.github.io/gt-modding-hub/ps1/gt2/file_structure/) | структура файлов, списки инструментов, PDF SUBMANIAC | - |
| [Nenkai/010GameTemplates](https://github.com/Nenkai/010GameTemplates) | 010-шаблоны (в т.ч. CDO/CNO/CDP по словам Hub) | не проверено |
| [Silent's blog: GT2](https://silentsblog.com/mods/gran-turismo-2/) + [Console-Cheat-Codes](https://github.com/CookiePLMonster/Console-Cheat-Codes) | widescreen, 60 fps, draw distance, max LOD, метрика; адреса для всех версий | - |
| [CookiePLMonster/GT2-Combined-Disc](https://github.com/CookiePLMonster/GT2-Combined-Disc) | объединение двух дисков | Python 3.8+ |
| [gt2.airesoft.co.uk](http://www.gt2.airesoft.co.uk/) | данные машин/гонок, извлеченные из файлов (JP/US/PAL 1.0) | - |
| [zyzalfors/GT2SaveEditor](https://github.com/zyzalfors/GT2SaveEditor) | CLI-редактор сейвов | - |
| GTPlanet: [Modding and restoration discussion](https://www.gtplanet.net/forum/threads/modding-and-restoration-discussion.374974/page-15), [GT2+ thread](https://www.gtplanet.net/forum/threads/mod-gran-turismo-2-plus-bug-fixes-restored-content-and-new-content-beta-7-released.378282/) | первичные посты pez2k, submaniac93, SilentPL | - |
