# CS2GLAZ + CSVILKA: всё для продолжения работы в новом чате

Состояние на 2026-10-04. CS2GLAZ 0.15.4, CSVILKA 1.2.3.

## 1. Правила работы с пользователем (обязательно)

- Общаться **на русском**.
- **Репозитории и ветки:**
  - CS2GLAZ: `glazki2/cs2pugin`; на GitHub он же открывается как `glazki2/cs2antiwh`.
    Работать в ветке `claude/wizardly-allen-uzlnya`: коммиты и push только туда.
    PR не создавать, если не попросят.
  - CSVILKA: `glazki2/cs2vilka` (на GitHub `glazki2/CS2VILKA`). Ветки
    `claude/wizardly-allen-uzlnya` и `ccr-e6702687-rued9n` (основная) держать одинаковыми:
    push в обе (fast-forward).
- **Сообщения коммитов** заканчиваются строками:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01M2hvDVehR3UHDzbKenfQ1o
  ```
  Последнюю строку заменить на ссылку новой сессии. Идентификаторы моделей в код и коммиты
  не писать.
- **Принципы плагина:** fail open (при любой неуверенности отправлять всё, как без плагина),
  никогда не ронять сервер или клиента, в CheckTransmit только **снимать** биты передачи
  и никогда их не добавлять.
- **Безопасность:**
  - Не вставлять в чат и в код пароли, GSLT, вебхуки. GSLT пользователя (начинается
    с `420767DC…`) уже утекал, его надо перевыпустить.
  - Проверять читы только с альт-аккаунта.
- **Ветки:** удаление удалённых веток блокируется классификатором. Не пытаться, пусть
  пользователь удаляет сам.
- **Перед каждым push:**
  - сборка;
  - `cs2glaz_tests`;
  - `tests/test_package.py`;
  - clang `-fsyntax-only` по изменённым файлам.
- **После push:** ждать CI (Linux и Windows) и давать пользователю ссылки на артефакты вида
  `https://github.com/glazki2/cs2antiwh/actions/runs/<run>/artifacts/<id>`.
  - Через MCP: `mcp__github__actions_list`, `list_workflow_runs` и
    `list_workflow_run_artifacts`, owner `glazki2`, repo `cs2pugin` / `cs2vilka`.
  - Логи упавшего задания: `mcp__github__get_job_logs`.

## 2. Сервер пользователя

- **Хостинг:** Shockbyte, Linux, `-maxplayers 32`. CS2 1.41.8.8 (2 окт 2026).
  Metamod 2.0.0-dev+1473.
- **Плагины в `meta list`:**
  - `[AS] Core 1.0.8f` (в статусе ERROR, чужой);
  - `Ghost 3.7.0 by glazki2` (свой плагин пользователя, перехватывает FireEvent);
  - `CSVILKA 1.2.3`;
  - `CS2GLAZ`.
- **Тесты** пользователь гоняет на de_mirage с ботами:
  `sv_cheats 1; mp_ignore_round_win_conditions 1; mp_roundtime 60; mp_freezetime 0; mp_restartgame 1; bot_stop 1`.
- **Хостинг медленный:**
  - `UNEXPECTED LONG FRAME 20ms` при работе сервера 1,5 мс — это голодание по процессору
    у хоста, не плагины.
  - Каждый `process_vm_readv` (наш `safe_read`) там стоит около 90 мкс. Поэтому безопасные
    чтения памяти надо делать крупными кусками, а не по полю.
- **Сеть окружения Claude:** Steam (api.steampowered.com, CDN) закрыт. Скачать
  `libserver.so` нельзя: либо пользователь загрузит его сам, либо в настройках окружения
  меняют Network access. GitHub работает.

## 3. CS2GLAZ: что это

Серверный анти-WH для CS2 на Metamod:Source 2.0, C++20, хуки через KHook. Основан на cs2fow-ce.

- **Карта:** запекается в BVH8 сторонним `tools/cs2glaz_baker`, нативным читателем физики
  (VRF больше нет). Файлы лежат в `addons/cs2glaz/data/maps/`, бейк запускается
  автоматически при первой загрузке карты.
- **Каждый тик:**
  - `game_state.cpp` снимает снимок: игроки, капсулы костей, дымы, двери и пропы;
  - `visibility_worker` в 2 потоках считает, кто кого видит: несколько точек обзора
    (глаз, плечи по пингу, над головой, ноги), капсулы тела, Masked Occlusion Culling,
    реальные дымы, расчистка дыма HE;
  - `transmit.cpp` в `CheckTransmit` снимает биты невидимых врагов целиком: пешка, оружие,
    надетые предметы, прикреплённые сущности.
- **Защиты от сбоев:**
  - `lifecycle_guard.h`: появление и смерть, удержание раскрытия `cs2glaz_visibility_hold_ms`;
  - быстрые переотправки;
  - тела умерших скрытыми не отправляются (иначе краш клиента `CopyExistingEntity`);
  - журнал передач (`journal.cpp`, команда `cs2glaz_entity N`).
- **Радар:** фильтр `ProcessSpottedEntityUpdate` (`radar_filter.cpp`).
- **Режимы сборки:**
  - проверенная gamedata есть только для 1.41.7.4;
  - на остальных сборках работает **limited mode**: стены и тело в форме хитбокса, без
    анимированных капсул;
  - дым в limited mode включается после проверки раскладки на первом живом дыме.
    Объём ищется в пределах ±1024 байт; на 1.41.8.8 найден сдвиг +232.
- **Менеджер игровых событий:** экспорта нет, он ищется сигнатурой `game_event_manager`
  из `cs2glaz.signatures.txt` с проверкой RTTI.
- **Обновления:** автообновление `cs2glaz_auto_update` берёт **GitHub Releases** `glazki2/cs2antiwh`.
  Релизов пока **нет**, публиковать только с согласия пользователя, поэтому
  пользователь обновляется вручную архивами из CI.

### Приманки (экспериментально, `src/plugin/decoys.cpp` и др.)

- **`cs2glaz_decoys`:** 0 выкл, 1 невидимые, 2 рисуются (для теста, **ничего не
  засчитывается**), 3 только для подозреваемых.
- **Скрытые приманки за стенами:** props, ходят и прыгают. Отчёты: прицел, выстрел,
  прицел, проследивший прыжок приманки. Каждый отчёт сравнивается с контрольными
  двойниками, которые клиенту не отправляются.
- **Доказательства:** `decoy_evidence` = реальные отчёты − ожидаемые − 3√ожидаемых.
  Кик по порогу `cs2glaz_decoy_kick`. Передача в CSVILKA через мост `anticheat_bridge.h`,
  а через неё в Discord.
- **Слепые попадания:** попадания по врагу, которого клиенту не отправляли.
- **Ghosts (`ghosts.cpp`):** боты через `CreateFakeClient`. Их события скрыты, они
  безвредны: без урона, без движения.
- **Phantoms (`phantoms.cpp`):** контроллеры в слотах выше maxplayers, у которых пешка — prop.
  **На сервере пользователя чит их не распознал.** Причины: слот выше maxplayers, пешка
  не класса игрока, нет костей.
- **Front decoys (`front_decoys.cpp`, `cs2glaz_decoy_front 1/2`):** фантом в 3–20° от
  прицела против аимбота; режим 2 добавляет коллизию против триггербота. Не сработал
  по той же причине, что и фантомы.
- **Crosshair ghosts (`crosshair_ghosts.cpp`, `cs2glaz_decoy_crosshair 1`, с 0.15.0):**
  два настоящих бота, по одному на T и CT. По очереди показываются игрокам
  противоположной команды на пинг + 350 мс.
  - **Точка:** голова на прицеле, 200–900 юнитов. Точку не видит никто другой, рядом
    нет игроков и предметов (`choose_crosshair_spot`). Показ только когда игрок держит
    огнестрел и не стрелял последние 600 мс.
  - **Отчёт:** выстрел в окне от пинг/2 до пинг + 350 мс — это `crosshair_shot`
    с `reaction_ms`. Каждый третий показ контрольный.
  - **Между показами** бот висит над картой.
  - **Бомба:** призрак с C4 зависает над союзником и выбрасывает её.
  - **Самоубийство:** перед смертью уходит под карту.
  - **Отключение:** после 3 чужих пуль, застрявших в призраке, режим выключается.
  - **Отладка:** `addons/cs2glaz/logs/ghost_trace.log` пишет каждый шаг с fsync.
- **Логи:** `addons/cs2glaz/logs/decoys.log`. Поля: tick, round, round_time, event,
  reaction_ms, счётчики, evidence.
- **Страница разбора:** `tools/decoy-review.html`, опубликована как артефакт
  https://claude.ai/artifact/8GZ2pVBwxr3TxpYpcH5MRb (приватная).

### Переменные (`cfg/cs2glaz.cfg`, файл применяется заново каждую карту)

- **Защита:**
  - `cs2glaz_enable 1`, `cs2glaz_limited_mode 1`;
  - `cs2glaz_smoke_occlusion 1`, `cs2glaz_he_clear_radius_units 180`, `cs2glaz_he_clear_seconds 3`;
  - `cs2glaz_filter_teammates 0`;
  - `cs2glaz_update_interval_ms 1`, `cs2glaz_worker_threads 2`;
  - `cs2glaz_shoulder_base_units 48`, `cs2glaz_shoulder_rtt_scale 0.4`, `cs2glaz_max_shoulder_units 128`;
  - `cs2glaz_bounds_padding_units 16`, `cs2glaz_visibility_hold_ms 150`, `cs2glaz_result_wait_ms 3`.
- **Обновления:** `cs2glaz_auto_update 0`.
- **Приманки:** `cs2glaz_decoys 0`, `cs2glaz_decoy_kick 0`, `cs2glaz_decoy_ghosts 0`,
  `cs2glaz_decoy_phantoms 0`, `cs2glaz_decoy_front 0`, `cs2glaz_decoy_crosshair 0`.
- **Без строк в cfg:** `cs2glaz_radar_filter`, `cs2glaz_filter_dead`,
  `cs2glaz_filter_full_updates`, `cs2glaz_dynamic_occluders`.
- **Важно:** cfg применяется заново на каждой карте, поэтому значение, введённое в консоли,
  после смены карты сбрасывается. Постоянные настройки — только в cfg.

### Команды

- `cs2glaz_selftest`: всё по пунктам OK/OFF/WARN/FAIL. Начинать с неё.
- `cs2glaz_status`, `cs2glaz_metrics`.
- `cs2glaz_why [ник|слот]`, `cs2glaz_entity N`, `cs2glaz_props [радиус]`.
- `cs2glaz_suspect`, `cs2glaz_reload`, `cs2glaz_check_config`, `cs2glaz_check_update`, `cs2glaz_help`.

### Файлы

- `src/core/`:
  - BVH8 и builder;
  - `capsule_visibility` (MOC), `visibility_sampling`, `smoke_occlusion`, `smoke_layout_check.h`;
  - `decoy_logic` — вся тестируемая логика приманок и выбор точек;
  - `signature_scan`, `rtti_check.h`, `lifecycle_guard.h`, `transmit_masks.h`, `transmit_journal.h`;
  - `fixed_list.h`, `vpk`, `map_source`, `dynamic_occluders`.
- `src/plugin/`:
  - `plugin.cpp/h` (Load/Unload, статус);
  - `game_state.cpp` (снимок, дым), `transmit.cpp`, `visibility_worker`;
  - `decoys`, `ghosts`, `phantoms`, `front_decoys`, `crosshair_ghosts`;
  - `bridge.cpp` (CSVILKA), `journal`, `radar_filter`, `occluders`, `diagnostics`;
  - `selftest`, `settings`, `runtime_compatibility` (схема, gamedata, limited mode),
    `updater`, `automatic_baker`.
- `src/baker/`: читатель KV3 и физики карты.
- `gamedata/`: `cs2glaz.games.txt` (смещения для проверенной сборки) и
  `cs2glaz.signatures.txt` (паттерны CreateEntityByName, DispatchSpawn, RemoveEntity,
  SetModel, game_event_manager; индекс Teleport 164/165).
- **Документация:** `docs/CODE_TOUR.md` — подробный обзор кода (англ.), `README.md` — для
  пользователя (рус.), `CHANGELOG.md`.

### Сборка и проверки (Linux, окружение Claude)

```
cd /home/user/cs2pugin/build-linux && timeout 590 env PYTHONPATH=/home/user/cs2pugin/.build-deps/linux/ambuild python3 -c "import ambuild2.run as r; r.cli_run()"
./build-linux/cs2glaz_tests/linux-x86_64/cs2glaz_tests
PYTHONPATH=. python3 tests/test_package.py
```

- **Длинная сборка:** запускать в фоне с выводом в файл. Не пропускать её через
  `| grep | head`: так она зависала.
- **Проверка clang:** взять строку g++ из лога сборки и заменить:
  - `g++` на `clang++ -fsyntax-only -Wno-unknown-warning-option`;
  - `-DCS2GLAZ_VERSION=…` на `-DCS2GLAZ_VERSION=\"x\"`;
  - убрать ` -o …`, ` -c ` и `-fno-gnu-unique`.
- **Windows:** `near` и `far` — макросы, так переменные не называть.
- **Версия** меняется в `VERSION` и `plugin-metadata.json`, плюс запись в `CHANGELOG.md`.
- **Новый .cpp** добавлять в `AMBuilder` и `CMakeLists.txt`.

## 4. CSVILKA (`/home/user/cs2vilka`)

- **Что это:** античит пользователя (детекторы аимбота, триггербота, bhop, subtick и т. д.,
  баны, Discord-вебхук). Принимает доказательства приманок CS2GLAZ как детекцию ESP
  (`csvilka_esp_enabled`).
- **1.2.2:** gamedata обновлена под обновление CS2 от 22 сентября (сигнатуры OnJumpLegacy,
  AirMove, WalkMove, Duck и SetupMove из cs2kz; Teleport 165/164, IsEntityPawn 171/170,
  IsEntityController 172/171, ClientOffset 616).
- **Без подтверждения:** смещение ProcessRespondCvarValue. Хук сверяет vtable сообщения
  и при несовпадении сам отключается с предупреждением.
- **1.2.3:** SDK hl2sdk-cs2 обновлён до 394a726, исправлен ConVar_Unregister.
- **Сборка:**
  ```
  git submodule update --init --recursive metamod-source
  rm -rf build-local
  PYTHONPATH=/home/user/cs2pugin/.build-deps/linux/ambuild python3 configure.py --enable-optimize --out build-local
  ```
  Затем сборка в `build-local`.
- **Проверки CI локально:**
  - clang-format 22.1.8: `--dry-run --Werror --style=file` по `src` (без `clientcvar` и `vendor`);
  - `python3 check-translations.py`.
- **Артефакты 1.2.3:**
  https://github.com/glazki2/CS2VILKA/actions/runs/37141174697 (Linux artifact 11280402325).

## 5. Что уже проверено на сервере пользователя

- **Ок:**
  - 0.14.0: защита работает (limited mode, de_mirage, 117 983 треугольника);
  - дым включился после проверки на живом дыме;
  - игровые события, радар, `mp_playerid 1` — OK;
  - нагрузка: worker p99 0,5–0,9 мс, игровой поток 0,33–0,49 мс за тик;
  - CSVILKA 1.2.3 загружается.
- **Найдено и исправлено в 0.14.1:**
  - подвисание 91 мс на первом дыме (тысяча `process_vm_readv`);
  - подвисание 36 мс от `cs2glaz_selftest`.
- **Приманки:**
  - фронтальные фантомы чит пользователя не видит ни ESP, ни триггерботом (см. выше);
  - система не обвинила честную игру: evidence 0.0.
- **Призраки на прицеле:** включение `cs2glaz_decoy_crosshair 1` **ронял сервер** сразу
  после `"Egor<10><BOT>" connected`.
  - Причина: `khook_fire_event` вызывал оригинальный FireEvent (он освобождает событие)
    и делал Supersede; плагин Ghost в цепочке получал освобождённое событие.
  - Исправлено в **0.15.4**: `KHook::Recall(&IGameEventManager2::FireEvent, {Ignore,false}, manager, event, true)`,
    как в CounterStrikeSharp.
  - **0.15.4 ещё не проверена пользователем.**

## 6. Открытые задачи

1. **Проверить 0.15.4 на сервере** с `cs2glaz_decoys 2`, `cs2glaz_decoy_crosshair 1`,
   `mp_autoteambalance 0`, `mp_limitteams 0`.
   - Ждать `crosshair ghost "…" created on team 2`, вход бота в T и показы на прицеле
     каждые 2,5–5 с.
   - Если упадёт, взять последние строки `addons/cs2glaz/logs/ghost_trace.log` и лог консоли.
   - Запасной вариант: создавать призраков через `bot_add_t` / `bot_add_ct` вместо
     `CreateFakeClient`.
2. **Узнать у пользователя по своему читу:** рисует ли ESP призрака, стреляет ли
   триггербот, внешний это чит или внутренний.
3. **Проверенная gamedata под 1.41.8.8:** нужен `libserver.so`, загрузка от пользователя
   или доступ к Steam.
4. **GitHub Releases** для автообновления CS2GLAZ и CSVILKA: опубликовать только с согласия
   пользователя.
5. **Проверка выгрузки CSVILKA:** `meta unload <id>`, затем `cvarlist csvilka` должен
   показать 0.
6. **Строки `prop_dynamic … has no model name!`** — безвредный шум. Приманки спавнятся
   без модели, чтобы не стать твёрдыми.

## 7. Последние ссылки на скачивание

- **CS2GLAZ 0.15.4:**
  [Linux](https://github.com/glazki2/cs2antiwh/actions/runs/37201040624/artifacts/11303405254),
  [Windows](https://github.com/glazki2/cs2antiwh/actions/runs/37201040624/artifacts/11303350481).
- **CSVILKA 1.2.3:**
  [Linux](https://github.com/glazki2/CS2VILKA/actions/runs/37141174697/artifacts/11280402325),
  [Windows](https://github.com/glazki2/CS2VILKA/actions/runs/37141174697/artifacts/11280796330).
- **Установка вручную:** остановить сервер, заменить в `game/csgo/` папку
  `addons/cs2glaz/`, файлы `addons/metamod/cs2glaz.vdf` и `tools/cs2glaz_baker` (права 755).
  Свой `cfg/cs2glaz/cs2glaz.cfg` не трогать. После запуска: `meta list` и `cs2glaz_selftest`.
