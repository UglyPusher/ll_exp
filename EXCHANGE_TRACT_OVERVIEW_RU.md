# Биржевой тракт: что это, как работает и где находится разработка

**Состояние на 21 августа 2026 года**

## 1. Что это за проект

Это прототип вычислительного ядра небольшой биржевой системы.

Его задача — не реализовать «всю биржу целиком», а собрать и проверить главный путь данных: принять клиентскую команду, надёжно записать её в историю, проверить её, обработать в matcher, сохранить результат и на основании результата восстановить внешнее состояние системы.

В конечном виде первая версия должна показывать полный путь:

```text
Client
  -> Ingress
  -> Risk
  -> Reserve
  -> Command WAL
  -> Matcher
  -> Event WAL
  -> Portfolio / Market Data
  -> Client
```

При этом система должна уметь не только работать в нормальном режиме, но и восстанавливаться после аварии:

```text
Crash
  -> validate WAL
  -> load snapshot
  -> replay WAL tail
  -> rebuild projections
  -> continue trading
```

Ключевая цель проекта — сделать этот путь:

- детерминированным;
- ограниченным по памяти;
- пригодным для low-latency обработки;
- восстанавливаемым после сбоя;
- проверяемым тестами;
- измеряемым benchmark'ами.

Это **не production-биржа**. В первой версии сознательно не планируются HA-кластер, replication, failover, production FIX gateway, полноценный clearing, cross-margin, сложные типы заявок и multi-instrument sharding. Сначала должен быть доказан простой, целостный и воспроизводимый тракт.

---

# 2. Главная идея: история команд и история результатов

В системе есть два принципиально разных потока.

## 2.1. Command stream

Это последовательность команд, которые система должна выполнить.

Например:

```text
NewLimit
SaveSnapshot
LoadSnapshot
StartReplay
StopReplay
Shutdown
```

Для торговой команды в ней находятся данные вроде:

```text
client_id
order_id
owner_id
side
price
quantity
```

Команды образуют **Command WAL** — каноническую историю входных действий.

Если система потеряла оперативное состояние, Command WAL должен позволить снова выполнить тот же набор команд в том же порядке.

## 2.2. Event stream

Matcher не просто меняет своё внутреннее состояние. Он публикует результат обработки команды в виде событий.

Например:

```text
OrderAccepted
OrderRejected
Trade
OrderRested
OrderDone
MatcherFatal
```

Эти события образуют **Event WAL**.

Event WAL нужен для двух вещей:

1. проверить, что повторное выполнение Command WAL дало тот же результат;
2. восстановить независимые downstream-проекции: портфель клиента, внешний стакан market data, бухгалтерский или другой учёт.

Отсюда основной инвариант системы:

```text
один command stream
    -> один event stream
    -> одно итоговое состояние
```

Если при replay один и тот же Command WAL породил другой Event WAL, это не «один из допустимых вариантов». Это ошибка восстановления.

---

# 3. Что такое WAL в этом проекте

**WAL — Write-Ahead Log**, последовательный журнал записей.

Но здесь важно не смешивать две разные вещи:

1. **runtime transport** — как команда быстро движется между модулями в памяти;
2. **persistent WAL file** — как команда сохраняется на диске и переживает crash.

В ранних вариантах эти задачи было легко смешать. Сейчас граница зафиксирована явно.

```text
                 RAM                         persistent storage

Ingress -> CommandPipeline -> ...       +------------------------+
               |                        |      Command WAL       |
               +---- Persistence ------>|      WAL file          |
                                        +------------------------+
```

Файл WAL — долговременная история и источник recovery.

`CommandPipeline` — быстрый in-memory транспорт между runtime-стадиями. Сам pipeline не выполняет file I/O.

Это важное архитектурное решение.

---

# 4. Как команда движется в памяти

Для Command tract реализован bounded preallocated SPMC pipeline.

Слово **bounded** означает: память имеет заранее заданную конечную ёмкость.

Слово **preallocated** означает: необходимые slots выделяются заранее. В hot path не должно происходить обычных динамических allocations.

SPMC здесь означает, что один опубликованный поток последовательно проходит через несколько читателей/стадий, причём каждая стадия имеет собственную границу готовности.

У pipeline есть пять монотонных абсолютных позиций — `frontier`:

```text
tail <= reserve_checked <= risk_checked <= durable <= head
head - tail <= capacity
```

Их проще понимать не как пять указателей на элементы массива, а как пять отметок: **до какого места каждая стадия гарантированно закончила свою работу**.

## 4.1. `head`

`head` показывает, до какой позиции Ingress уже опубликовал команды.

Команда ниже `head` существует в runtime pipeline.

Но это ещё не означает, что она надёжно записана на диск.

## 4.2. `durable`

`durable` показывает, до какой позиции команды уже физически сохранены.

Persistence берёт диапазон команд, записывает его в WAL, выполняет один physical sync для batch и **только после успешного sync** двигает `durable`.

Поэтому:

```text
[durable, head)
```

— это команды, уже опубликованные в памяти, но ещё не разрешённые бизнес-стадиям.

## 4.3. `risk_checked`

Risk читает только команды ниже `durable`.

Он записывает своё решение в принадлежащую ему область данных и затем двигает `risk_checked`.

Таким образом Risk никогда не принимает решение по команде, которая ещё не пересекла durable boundary.

## 4.4. `reserve_checked`

Reserve работает после Risk.

Он видит только команды ниже `risk_checked`, записывает собственный результат и затем двигает `reserve_checked`.

## 4.5. `tail`

Matcher может брать только команды ниже `reserve_checked`.

После того как Matcher полностью скопировал данные slot и закончил его потребление, он двигает `tail`.

Только после этого соответствующий slot разрешено использовать повторно.

---

# 5. Одна команда: полный live-путь

Рассмотрим одну обычную клиентскую заявку.

## Шаг 1. Ingress принимает команду

Ingress переводит внешнее сообщение клиента во внутренний `CommandEnvelope`.

Он получает следующий `CommandSequence`, заполняет slot и публикует новый `head`.

После этого команда существует в pipeline, но ещё не считается durable.

## Шаг 2. Persistence сохраняет команду

Persistence читает опубликованные immutable-команды между `durable` и `head`.

Он может объединить несколько команд в один I/O batch:

```text
append record A
append record B
append record C
physical sync
publish durable frontier
```

Batch — только способ уменьшить стоимость sync.

На диске **нет** специальной commit-записи, batch header или commit marker.

## Шаг 3. Risk принимает решение

После продвижения `durable` Risk получает доступ к команде.

В будущей полной версии он должен вернуть, например:

```text
Accept
Reject
```

и записать также версию модуля и версию ruleset, чтобы решение можно было корректно интерпретировать и воспроизводить.

Runtime-механика отдельной области `RiskResult` уже предусмотрена, но полноценная бизнес-логика Pre-Risk ещё не является готовым этапом основного roadmap.

## Шаг 4. Reserve проверяет ресурсы

После Risk команда попадает в Reserve Manager.

Его задача — проверить наличие необходимых средств/ресурсов и зарезервировать их до исполнения или отмены заявки.

Runtime frontier и отдельная область `ReserveResult` уже предусмотрены. Полная модель reserve/release и её связь с Portfolio ещё впереди.

## Шаг 5. Matcher обрабатывает заявку

Matcher получает только команду, прошедшую предыдущие readiness boundaries.

Он изменяет order book и создаёт события результата.

Простейшие варианты:

```text
пришла пассивная заявка
    -> OrderAccepted
    -> OrderRested

пришла агрессивная заявка
    -> OrderAccepted
    -> Trade
    -> OrderDone / OrderRested

команда не может быть обработана
    -> OrderRejected
```

После завершения работы Matcher освобождает runtime slot продвижением `tail`.

## Шаг 6. Результат попадает в Event WAL

События должны образовать последовательную durable-историю результатов.

Именно этот поток в дальнейшем читают Portfolio, Market Data и другие независимые projections.

---

# 6. Почему pipeline ограничен по размеру

Система сознательно не пытается «как-нибудь накопить всё в памяти».

Для pipeline выполняется:

```text
head - tail <= capacity
```

Если downstream перестал успевать, `tail` перестаёт двигаться.

Свободные slots заканчиваются, и upstream получает backpressure.

То есть перегрузка должна проявляться как **контролируемое замедление**, а не как:

- перезапись ещё не обработанных данных;
- бесконечный рост памяти;
- тихая потеря сообщений.

Для low-latency системы это важнее, чем возможность временно «спрятать проблему» большой динамической очередью.

---

# 7. Что значит durable

`durable` здесь имеет очень конкретный смысл.

Команда считается пересёкшей live durable boundary только после:

```text
append records
physical sync
publish durable
```

Для текущего filesystem backend физическая синхронизация выполняется системными вызовами:

```text
Windows: WriteFile + FlushFileBuffers
POSIX:   write + fdatasync
macOS:   write + fsync
```

Важно различать два времени:

```text
команда записана в буфер/файл
```

и

```text
операционная система подтвердила physical sync
```

`durable` двигается только после второго события.

---

# 8. Что происходит после crash

Одна из самых неприятных частей WAL — определить, что считать историей после аварийного завершения процесса.

Для проекта это правило уже зафиксировано.

## 8.1. Авторитетная история

После crash WAL сканируется с начала.

Reader проверяет:

- identity файла;
- формат;
- CRC заголовка файла;
- CRC заголовка каждой записи;
- CRC payload;
- непрерывность sequence;
- границы record;
- нулевой padding.

**Максимальный непрерывный CRC-valid prefix является восстановленной историей.**

Каждая полная запись внутри этого prefix участвует в replay и rebuild.

Не имеет значения, успел клиент получить acknowledgement или нет.

## 8.2. Неполный хвост

Если crash оборвал последнюю запись физически посередине:

```text
полный valid prefix
полный valid prefix
неполный header/payload/padding <- crash
```

recovery может удалить только этот доказанно неполный trailing record.

После truncation выполняются physical sync и полный повторный validated scan.

## 8.3. Повреждённая полная запись

Если запись физически полная, но CRC неверен, recovery **не пытается догадаться**, что произошло.

То же относится к:

- corruption в середине файла;
- sequence gap;
- duplicate sequence;
- несовпадению stream/epoch/manifest;
- несовместимому формату или schema.

В этих случаях recovery отказывается изменять файл.

Принцип простой:

> Неполный хвост можно безопасно отрезать. Полностью записанные, но недостоверные данные автоматически «чинить» нельзя.

---

# 9. Почему нет commit marker после каждого batch

Это отдельное зафиксированное решение.

Физический WAL устроен как:

```text
FileHeader
Record
Record
Record
...
```

Один logical payload соответствует одной physical record.

Batch существует только во время live-записи:

```text
несколько append -> один sync
```

После crash валидный хвост определяется самими физическими записями, CRC и непрерывностью sequence.

Поэтому дополнительный протокол вида:

```text
write records
sync
write commit marker
sync again
```

не используется.

Он добавил бы ещё одну запись и ещё одну синхронизацию в самое дорогое место тракта, не являясь частью выбранной модели recovery.

---

# 10. Физический формат WAL

Текущий canonical filesystem format имеет version `3`.

Каждый файл относится к одному потоку в одной epoch.

Упрощённо:

```text
FileHeader (64 bytes)
padding
RecordHeader (24 bytes) + payload + padding
RecordHeader (24 bytes) + payload + padding
...
```

В header файла находятся, среди прочего:

```text
stream_kind
stream_id
epoch_id
manifest_id
first_sequence
payload_size
payload_schema_version
alignment
```

Runtime `capacity` туда не записывается, потому что размер in-memory ring — свойство запуска, а не identity долговременной истории.

Данные сериализуются в canonical little-endian виде. На диск не пишется «как есть» native C++ struct с его padding, указателями или ABI-зависимым layout.

---

# 11. Формат Command и Event payload

Generic WAL не знает смысла payload. Он хранит фиксированный массив bytes и отвечает за sequence, CRC и physical format.

Смысл данных задаёт matcher-level schema.

Текущая schema version — `2`.

Размеры фиксированы:

```text
Command payload = 48 bytes
Event payload   = 64 bytes
```

Schema version 2 сохранила byte layout старых version-1 команд и событий и добавила `StartReplay`/`StopReplay`.

Decoder обязан отвергать:

- неизвестный tag;
- неверное enum-значение;
- неканонический boolean;
- ненулевые reserved bytes;
- ненулевой unused tail;
- неверный payload size.

Если меняется смысл поля, offset, width, tag или canonical size, требуется новая schema version.

---

# 12. Matcher и OrderBook

Matcher уже не является чистой идеей: минимальная реализация существует и имеет benchmark harness.

Внутри него используется отдельный `OrderBook`.

## 12.1. Что делает OrderBook

OrderBook хранит уже стоящие в стакане заявки для одного инструмента.

Он умеет:

```text
insert(order)
best(Bid / Ask)
set_remaining(order_id, quantity)
erase(order_id)
```

Сам OrderBook **не является matcher**. Он не решает, должна ли входящая заявка торговаться, не создаёт trade и не знает про WAL или сеть.

Matcher задаёт политику, а OrderBook предоставляет предсказуемое хранилище.

## 12.2. Почему он fixed-capacity

Основные структуры выделяются при создании:

```text
OrderPool
OrderIdIndex
Bid SideBook
Ask SideBook
```

После construction операции hot path не выполняют allocations.

Это уменьшает:

- jitter allocator'а;
- непредсказуемое движение памяти;
- риск случайного expensive allocation в критическом пути.

## 12.3. Приоритет заявок

Для одной цены заявки хранятся FIFO:

```text
раньше пришла -> раньше стоит в очереди
```

Bid выбирает максимальную активную цену.

Ask выбирает минимальную активную цену.

Для поиска лучшей цены используются price segments и битовые маски активности.

Это компромисс: прямой доступ по ограниченному диапазону цен даёт предсказуемую работу, но очень широкий и разреженный диапазон цен потребляет много памяти.

---

# 13. Replay: как система должна повторять историю

Replay — это повторное выполнение сохранённых команд для восстановления или проверки.

Критически важно, что replay не должен быть отдельным «тайным входом» прямо в Matcher.

Он должен проходить тот же тракт.

В live-режиме владельцем публикации в `head` является Ingress:

```text
Ingress -> try_publish()
```

В replay-режиме владельцем `head` становится `WalFileReader`:

```text
WalFileReader -> validated WAL command -> try_replay()
```

В один момент времени producer только один.

`try_replay()` сохраняет исходный physical `CommandSequence` из WAL. Он не создаёт новый live sequence.

Это потребовало явно разделить две координаты:

```text
ring position      -- где сейчас slot в runtime transport
CommandSequence    -- identity команды в истории
```

Они связаны в обычном live-потоке, но не являются одним и тем же понятием.

При replay ring может пройти сколько угодно runtime-позиций, а историческая sequence команды должна остаться той, которая была в WAL.

---

# 14. Replay управляется обычными командами

Для replay выбран in-band control.

То есть над трактом не появляется отдельный supervisor, который магически переключает внутренние состояния модулей.

Управляющие действия проходят через тот же поток команд:

```text
StartReplay
LoadSnapshot(NN)
... replay WAL commands ...
StopReplay
LoadSnapshot(MM)
```

В replay-режиме Persistence по-прежнему единственный владелец frontier `durable`, но он **не пишет replayed commands обратно в тот же WAL** и не выполняет sync. Он только публикует готовность уже проверенных команд для следующей стадии.

Базовая механика такой публикации уже реализована.

Полный deterministic replay protocol — **следующий текущий этап разработки**.

---

# 15. Snapshot: зачем он нужен

Если WAL содержит миллиарды команд, восстанавливать систему каждый раз с первой команды неразумно.

Поэтому периодически нужен snapshot — сохранённое состояние системы на известной sequence boundary.

Базовая модель восстановления:

```text
snapshot@N + WAL commands after N
    -> current state
```

Первая snapshot-модель относится прежде всего к matcher/order book. Общий envelope должен быть versioned и в будущем позволять добавлять sections других stateful-модулей.

При этом snapshot недостаточно просто «записать на диск». Нужно доказать несколько условий:

1. он соответствует определённым `sequence / epoch / version`;
2. в нём нет потерянного скрытого runtime-state;
3. после загрузки snapshot и replay хвоста получается то же состояние, что и у системы, работавшей без остановки;
4. все stateful-модули в будущем должны согласованно понимать одну boundary.

Полный snapshot/recovery contract ещё не закрыт Gate 1 основного roadmap.

---

# 16. Что уже реализовано

Ниже важно различать «архитектурно придумано» и «реализовано и проверено».

## 16.1. WAL infrastructure — реализована и в значительной части заморожена

Готовы:

- bounded preallocated WAL ring;
- `head / durable / tail` contract;
- физическая запись файлов;
- batch append + один physical sync;
- canonical file format version 3;
- CRC заголовков и payload;
- stream / epoch / manifest metadata;
- contiguous physical sequence;
- validated sequential reader;
- read-only scanner;
- conservative incomplete-tail recovery;
- fail-closed corruption policy;
- post-crash authoritative-tail semantics;
- compile-time physical WAL adapter seam.

Проверялись, среди прочего:

- append failure;
- sync failure;
- truncated header/payload/padding;
- bit corruption;
- sequence gap;
- duplicate sequence;
- identity/schema/format mismatch;
- concurrent producer/durability/consumer roles.

Выбранная WAL-реализация проходила тесты как с MSVC, так и с GCC 13.3.0.

## 16.2. Command SPMC Pipeline — реализован и зафиксирован

Готовы:

```text
tail <= reserve_checked <= risk_checked <= durable <= head
```

и механика:

- отдельный writer на каждый frontier;
- отдельные cache lines для frontiers;
- release/acquire publication;
- preallocated slots;
- backpressure;
- fail-closed persistence;
- drain уже durable prefix после I/O failure;
- отдельные `RiskResult` и `ReserveResult` sidecars;
- live publication;
- replay publication;
- разделение ring position и CommandSequence;
- восстановление live sequence cursor;
- five-role concurrent stress.

В stress-проверке command pipeline проходил 100 000 команд с параллельными ролями.

## 16.3. Command/Event encoding — реализован и зафиксирован

Есть canonical codecs schema versions 1 и 2.

Version 2 включает persisted `StartReplay`/`StopReplay`.

Golden-byte и round-trip tests проверяют точный layout и отказ от некорректных байтов.

## 16.4. OrderBook — реализован и глубоко протестирован

На Windows x64 / MSVC проверены:

- component tests;
- randomized tests;
- quick/stress soak;
- 100M-operation soak;
- AddressSanitizer для доступного набора проверок.

Hot-path операции после construction не выполняют allocations.

## 16.5. Minimal Matcher — существует

Есть минимальный matcher поверх публичного OrderBook API и отдельный `bench_matcher`.

Benchmark покрывает:

- passive resting orders;
- full fill;
- partial fill;
- `Matcher::process()`;
- `Matcher::run()` по потоку команд.

Это ещё не означает, что весь exchange tract готов. Matcher — только один модуль системы.

---

# 17. Что реализовано частично

## 17.1. Replay

Механика подачи validated WAL-команды обратно в CommandPipeline уже есть.

Есть persisted `StartReplay`/`StopReplay`, сохранение исходных sequence и переключение producer ownership.

Но ещё нужно доказать целиком:

```text
один Command WAL
-> одинаковый regenerated Event stream
-> одинаковый Matcher state
```

Это текущий следующий пункт.

## 17.2. Snapshot / recovery orchestration

Базовые snapshot-решения и matcher/order-book state существуют как часть инфраструктурной модели, но полный сценарий:

```text
snapshot@N + WAL tail -> exactly current state
```

ещё должен быть автоматически доказан и зафиксирован.

## 17.3. Benchmark infrastructure

Benchmark harness уже развит для OrderBook и Matcher.

WAL имеет свои тестовые/benchmark основы.

Но пока нет финальной единой performance matrix для полного тракта:

```text
Matcher only
WAL append/read
Command WAL durable
Full Command Tract
Command -> Matcher -> Event WAL
Full E2E
Market Data enabled
Replay
Recovery
```

---

# 18. Что ещё не является готовой частью системы

## 18.1. Настоящий Pre-Risk Manager

Pipeline умеет дать Risk отдельную область результата и собственную frontier.

Но ещё требуется сама минимальная бизнес-логика:

```text
accept / reject
module version
ruleset version
```

и правила, какие данные Risk имеет право читать.

## 18.2. Reserve Manager

Нужны:

```text
reserve
reject
release
```

а также чёткий lifecycle:

```text
reserve
  -> execution / partial execution / cancel
  -> release / portfolio update
```

## 18.3. Event Manager и Portfolio

Portfolio должен строиться **из Event WAL**, а не читать внутреннее состояние Matcher.

Это позволит полностью перестраивать его replay'ем.

Такой downstream tract ещё не собран.

## 18.4. Market Data

Планируется отдельный внешний образ стакана:

```text
Event WAL
  -> Market Data Book
  -> snapshot + deltas
  -> UDP publisher
  -> clients
```

Он не должен читать внутренние структуры Matcher напрямую.

## 18.5. End-to-End server

Пока ещё нет готового единого демонстрационного процесса с несколькими автономными клиентами, market-data listener и crash/restart сценарием.

---

# 19. Где разработка находится сейчас

По зафиксированному состоянию на **21 августа 2026 года** подготовительный этап выглядит так:

```text
[done] runtime ring / physical WAL boundary
[done] physical WAL metadata
[done] validated reader / scanner
[done] corruption policy
[done] post-crash durable-tail semantics
[done] base Command SPMC pipeline
[done] canonical Command/Event encoding v1/v2

[NEXT] deterministic replay

[later] remaining preparation items
[later] main EXCHANGE_TRACT_ROADMAP
```

То есть проект уже прошёл значительную работу по фундаменту, но **основной roadmap формально ещё не начат**.

Это не противоречие.

До начала основного roadmap специально закрывается подготовительный набор архитектурных вопросов, чтобы затем Risk, Reserve, Portfolio и Market Data не строились поверх постоянно меняющегося WAL/replay контракта.

Текущая ближайшая цель:

> доказать deterministic replay на уже зафиксированной механике WAL + CommandPipeline + canonical encoding.

После этого нужно завершить snapshot/recovery verification и baseline infrastructure benchmarks. Только затем инфраструктурный Gate 1 можно считать закрытым и переходить к полноценному Command Tract.

---

# 20. Главные технические трудности

Не все трудности проекта одинаковы. Часть уже решена архитектурно, часть остаётся впереди.

## 20.1. Детерминизм

Самая важная текущая проблема.

Для одного Command WAL повторное выполнение должно давать тот же результат независимо от запуска программы.

Нельзя незаметно зависеть от:

- wall clock;
- случайных чисел;
- порядка выполнения посторонних threads;
- адресов памяти;
- незафиксированных версий правил;
- неявного runtime-state.

Нужно сравнивать не «похоже ли состояние», а определённый reproducible result: Event stream, state hashes/checkpoints и итоговое состояние Matcher.

## 20.2. Разделение runtime position и historical sequence

В обычном live-потоке легко случайно считать, что «номер элемента в ring» и «номер команды» — одно и то же.

Replay показывает, что это неверно.

Runtime ring продолжает монотонно двигаться, а replayed команда обязана сохранить исторический `CommandSequence`.

Эта граница уже реализована, но она остаётся одним из ключевых мест, которые deterministic replay должен доказать тестами.

## 20.3. Snapshot consistency

Когда stateful-модулей станет несколько, snapshot должен означать согласованное состояние на одной логической boundary.

Недостаточно отдельно сохранить Matcher, отдельно Portfolio и отдельно Reserve «примерно в одно время».

Нужно определить, какие данные относятся к snapshot N, какие WAL sequences уже включены в него и с какой команды должен продолжаться replay.

## 20.4. Версии логики

Если Risk сегодня работает по ruleset A, а через месяц по ruleset B, старый WAL нельзя replay'ить так, будто всегда существовало правило B.

Поэтому version/epoch/schema metadata — не косметика. Это часть воспроизводимости истории.

## 20.5. Ownership состояния между Reserve и Portfolio

Reserve отвечает за временно занятые ресурсы.

Portfolio — за учётное состояние клиента.

Если оба начнут считать себя владельцами одной и той же величины, появятся двойные списания, потерянные release и трудно воспроизводимые рассогласования.

Граница между «резервом» и «учётным фактом» должна быть определена до интеграции.

## 20.6. Достаточность Event WAL

Portfolio и Market Data должны уметь восстановиться, читая события.

Следовательно Event WAL обязан содержать достаточно семантики, чтобы downstream-модулю не пришлось лезть во внутренний Matcher.

Это будет реальной проверкой качества event contract.

## 20.7. Backpressure без потери предсказуемости

Bounded pipeline специально не скрывает перегрузку.

Но затем нужно измерить, как ведёт себя весь тракт при:

- burst ingress;
- медленном consumer;
- почти заполненном ring;
- медленном storage;
- включённом Market Data.

Задача не в том, чтобы система «никогда не замедлялась», а в том, чтобы её деградация была контролируемой и измеряемой.

## 20.8. Стоимость physical sync

Physical sync — одно из самых дорогих действий в command path.

Поэтому batch size будет напрямую влиять на компромисс:

```text
меньше batch
-> меньше задержка ожидания batch
-> больше sync на единицу трафика

больше batch
-> выше throughput storage
-> потенциально больше waiting latency
```

Этот выбор нельзя делать по ощущениям; он требует benchmark'ов.

## 20.9. Производительность трудно измерять на обычной машине

Уже наблюдалось, что частота CPU, фоновые процессы и scheduler могут сдвигать wall-time сильнее, чем отдельные оптимизации.

Поэтому benchmark discipline требует:

- Release build;
- warmup;
- одинаковую affinity;
- повторные runs;
- control scenarios;
- p50/p99/p99.9 только при достаточной sample population;
- не переносить проценты с одной машины на другую как универсальную истину.

## 20.10. Portability

OrderBook хорошо проверен на Windows x64 / MSVC, но его Linux GCC/Clang matrix в соответствующем документе ещё отмечена как pending.

WAL уже проходил часть Release-тестов и на MSVC, и на GCC 13.3.0.

Это значит, что «проект в целом переносим» пока нельзя считать доказанным одним общим утверждением: разные компоненты имеют разную глубину platform verification.

---

# 21. Какие проблемы уже можно считать закрытыми

Несколько вопросов больше не должны заново проектироваться без конкретной причины.

## Durable boundary

Закрыто:

```text
append -> physical sync -> durable publication
```

## Post-crash tail

Закрыто:

```text
max contiguous CRC-valid prefix = recovered history
```

## Commit markers

Закрыто: их нет и они не нужны в выбранном physical format.

## Corruption policy

Закрыто: только доказанно incomplete tail можно автоматически truncate; corruption не «чинится» эвристически.

## Runtime transport vs persistent storage

Закрыто: CommandPipeline — in-memory transport, physical WAL — долговременная история.

## Physical backend selection

Закрыто: concrete adapter выбирается compile-time. Runtime polymorphism, virtual dispatch и CRTP для этой границы не используются.

## Базовая SPMC publication chain

Закрыта и протестирована механика frontiers, ownership и backpressure.

## Canonical persisted encoding

Зафиксированы file format v3 и matcher payload schemas v1/v2.

---

# 22. Следующие инженерные шаги

Ближайшая разумная последовательность сейчас такая.

## 1. Deterministic replay

Проверить автоматически:

```text
same Command WAL
-> same Event stream
-> same Matcher state
```

Включить сценарии replay с `StartReplay / LoadSnapshot / StopReplay` и убедиться, что historical sequence не смешивается с runtime ring position.

## 2. Snapshot + WAL tail recovery

Доказать:

```text
continuous execution state
==
load snapshot@N + replay tail state
```

Проверить epoch/version mismatch и отсутствие скрытого state.

## 3. Baseline infrastructure benchmark

До добавления бизнес-модулей зафиксировать отдельные характеристики:

```text
Matcher
WAL append/read
Replay
Recovery
```

Это даст точку отсчёта.

## 4. Freeze Gate 1

После успешной верификации считать WAL / Matcher / Replay / Snapshot / Recovery инфраструктурой, которую дальше меняют только по доказанной причине.

## 5. Начать полноценный Command Tract

После этого последовательно:

```text
Command Manager
-> Pre-Risk
-> Reserve
-> minimal command vocabulary
```

Не перепрыгивая сразу к Portfolio или Market Data.

---

# 23. Как организован сам процесс разработки

Для проекта введено правило freeze-by-roadmap.

Перед каждым пунктом проверяются prerequisites.

После завершения пункта:

1. результат проверяется тестами;
2. состояние записывается в `RESULTS.md`;
3. решения и контракты пункта считаются frozen.

Если следующий этап требует изменить frozen-решение, изменение нельзя вносить молча. Нужно явно назвать затронутый контракт, объяснить причину, разморозить пункт, изменить его, повторно провести verification и снова зафиксировать.

Это особенно важно для такого проекта, потому что локально удобное изменение WAL или sequence semantics может незаметно сломать replay, snapshot или downstream recovery через несколько этапов.

---

# 24. Каким должен стать результат первой версии

Первая публичная версия считается функционально законченной не тогда, когда «написано много классов», а когда можно воспроизвести один целый сценарий.

Пример:

```text
1. Запустить Exchange Server.
2. Подключить несколько клиентов.
3. Отправить заявки.
4. Пройти Risk и Reserve.
5. Записать Command WAL.
6. Выполнить matching.
7. Записать Event WAL.
8. Обновить Portfolio.
9. Опубликовать Market Data.
10. Сделать snapshot.
11. Продолжить торговлю.
12. Аварийно завершить процесс.
13. Проверить WAL.
14. Загрузить snapshot.
15. Replay WAL tail.
16. Восстановить projections.
17. Продолжить торговлю.
```

После того как этот сценарий работает, первая версия входит в feature freeze.

Дальше до release разрешены только:

- верификация;
- stress tests;
- benchmark;
- исправление дефектов;
- cleanup;
- документация;
- CI и release packaging.

---

# 25. Короткий итог

Проект строит не просто Matcher, а **воспроизводимый биржевой вычислительный тракт**.

Его центральная идея:

```text
Command WAL = что система должна была выполнить
Event WAL   = что система получила в результате
Snapshot    = сохранённое состояние на известной boundary
Replay      = повторное выполнение истории
```

Runtime-команды движутся через bounded preallocated SPMC pipeline с последовательными readiness frontiers:

```text
tail <= reserve_checked <= risk_checked <= durable <= head
```

Persistence отделён от runtime transport. Команда становится доступна бизнес-стадиям только после physical sync. После crash история определяется максимальным непрерывным CRC-valid prefix, без дополнительных commit markers.

На текущий момент уже достаточно глубоко реализованы и проверены:

- OrderBook;
- minimal Matcher;
- WAL physical format и persistence;
- reader/scanner/recovery;
- post-crash tail semantics;
- Command SPMC pipeline;
- canonical Command/Event encoding;
- базовая replay publication mechanics.

Следующая непосредственная задача — **deterministic replay**.

После него предстоит закрыть snapshot/recovery proof и baseline benchmarks, заморозить инфраструктурный Gate 1 и перейти к ещё не реализованной основной бизнес-части: Pre-Risk, Reserve, Event consumers, Portfolio, Market Data и E2E demo.

Именно поэтому проект сейчас находится в важной промежуточной точке: фундамент уже перестал быть экспериментальным набором идей, но весь биржевой тракт ещё не собран.

---

# Исходные документы

Этот обзор собран из текущих проектных материалов:

- `EXCHANGE_TRACT_ROADMAP.md`;
- `RESULTS.md`;
- `COMMAND_PIPELINE.md`;
- `WAL_PAYLOAD_FORMAT.md`;
- WAL `CONTRACT.md`, `DESIGN.md`, `FILE_FORMAT.md`, `INVARIANTS.md`, `BUILDING.md`;
- OrderBook `README.md`, `DESIGN.md`, `BENCHMARKS.md`, `PORTABILITY.md`;
- `AGENTS.md` с правилами выполнения roadmap и freeze уже принятых решений.
