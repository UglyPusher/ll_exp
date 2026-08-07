# OrderBook: контракт, тесты, benchmarks и план развития

## 1. Цель компонента

`OrderBook` — однопоточное фиксированное хранилище уже отдыхающих заявок одного инструмента.

Он отвечает за:

- хранение Bid/Ask заявок;
- ценовой приоритет и FIFO внутри одной цены;
- поиск лучшей заявки выбранной стороны;
- поиск заявки по `OrderId` внутри операций изменения и удаления;
- согласованность индекса, ценовых уровней, агрегатов и пула;
- отсутствие аллокаций после конструктора.

Он не отвечает за:

- пересечение цен и matching policy;
- выбор execution price/quantity;
- типы заявок, TIF, STP и auction policy;
- события, WAL, резервы, аккаунты и сессии;
- блокировки, сетевой ввод и многопоточность;
- NUMA affinity и OS memory locking.

Архитектурная граница:

```text
Matcher -> OrderBook -> detail::{OrderPool, OrderIdIndex, SideBook, ...}
```

Текущая реализация уже не наивная: есть фиксированный пул, intrusive FIFO, `OrderId`-индекс, сегменты по 64 цены и bitmask активных уровней. Откатывать production-код к `std::map` не нужно. Наивная реализация нужна только как простой проверяемый oracle для differential tests.

## 2. Целевой внешний интерфейс

```cpp
class OrderBook final {
public:
  explicit OrderBook(const OrderBookConfig& config);

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) noexcept = default;
  OrderBook& operator=(OrderBook&&) noexcept = default;

  [[nodiscard]] InsertResult insert(const RestingOrderData& order) noexcept;

  [[nodiscard]] std::optional<OrderView> best(Side side) const noexcept;

  [[nodiscard]] SetRemainingResult
  set_remaining(OrderId id, Quantity new_remaining) noexcept;

  [[nodiscard]] EraseResult erase(OrderId id) noexcept;

  [[nodiscard]] bool validate_invariants() const noexcept;
};
```

Кроме конструктора и специальных членов, внешний контракт содержит пять операций:

1. `insert`
2. `best`
3. `set_remaining`
4. `erase`
5. `validate_invariants`

Наружу не выходят `OrderIndex`, pool slots, price levels, segments, FIFO links, выбранная заявка, generation counters и способ поиска по ID.

### 2.1 Конфигурация

```cpp
struct OrderBookConfig {
  PriceTick min_price_tick;
  PriceTick max_price_tick;
  OrderCapacity max_orders;
};
```

Контракт:

- диапазон цен включительный;
- `min_price_tick <= max_price_tick`;
- `max_orders` должен быть представим внутренними индексами и размерами аллокаций;
- конструктор может бросить `std::invalid_argument` или `std::bad_alloc`;
- конструктор выделяет и последовательно инициализирует всю память;
- отдельного `warm_up()` и `page_size` нет;
- книгу создают на окончательном owner/matcher thread после настройки affinity/NUMA policy;
- memory locking выполняется внешним runtime-слоем.

### 2.2 Значения заявок

```cpp
struct RestingOrderData {
  OrderId id;
  OwnerId owner_id;
  Side side;
  PriceTick price;
  Quantity quantity;
};

struct OrderView {
  OrderId id;
  OwnerId owner_id;
  Side side;
  PriceTick price;
  Quantity remaining;
};
```

`OrderView` возвращается по значению. Он не является ссылкой или handle на внутреннюю память.

### 2.3 `insert`

```cpp
enum class InsertStatus : std::uint8_t {
  Ok,
  DuplicateOrderId,
  CapacityExhausted,
  PriceOutOfRange,
  InvalidQuantity
};

struct InsertResult {
  InsertStatus status;
  [[nodiscard]] bool ok() const noexcept;
};
```

Семантика:

- `quantity > 0`;
- `price` находится в настроенном диапазоне;
- среди активных заявок `OrderId` уникален;
- заявка добавляется в хвост FIFO своего ценового уровня;
- при ошибке состояние книги не меняется;
- внутренний `IndexFull` не должен быть публичным статусом: правильно рассчитанный индекс обязан вмещать `max_orders`.

Сложность: expected O(1), ограниченная длиной probe chain фиксированного индекса.

### 2.4 `best`

```cpp
[[nodiscard]] std::optional<OrderView> best(Side side) const noexcept;
```

Семантика аргумента:

- `best(Side::Bid)` возвращает Bid с максимальной ценой;
- `best(Side::Ask)` возвращает Ask с минимальной ценой;
- при одинаковой цене возвращается голова FIFO;
- для пустой стороны возвращается `std::nullopt`;
- операция не изменяет книгу и не создаёт скрытого selected state.

Целевая сложность: O(1) для текущей структуры с кэшированным лучшим сегментом и bitmask уровня.

### 2.5 `set_remaining`

```cpp
enum class SetRemainingStatus : std::uint8_t {
  Ok,
  NotFound,
  InvalidQuantity
};

struct SetRemainingResult {
  SetRemainingStatus status;
  Quantity previous_remaining;
  [[nodiscard]] bool ok() const noexcept;
};
```

Семантика:

- поиск выполняется по `OrderId` внутри книги;
- `new_remaining > 0`; для удаления используется `erase`;
- изменяются remaining заявки и агрегаты уровня/стороны;
- цена, сторона и FIFO-позиция не меняются;
- книга не решает, разрешено ли увеличение и нужно ли терять приоритет;
- `previous_remaining` позволяет matcher применить собственную политику;
- `NotFound` и `InvalidQuantity` не изменяют состояние.

Сложность: expected O(1).

### 2.6 `erase`

```cpp
enum class EraseStatus : std::uint8_t {
  Ok,
  NotFound
};

struct EraseResult {
  EraseStatus status;
  OrderView removed;
  [[nodiscard]] bool ok() const noexcept;
};
```

Семантика:

- поиск выполняется по `OrderId` внутри книги;
- заявка удаляется из FIFO, ID-индекса и пула;
- удаление последней заявки деактивирует ценовой уровень;
- результат содержит снимок удалённой заявки для событий/резервов;
- `NotFound` не изменяет состояние.

Сложность: expected O(1); tail latency зависит от probe cluster ID-индекса и способа поиска следующего лучшего сегмента.

### 2.7 `validate_invariants`

Диагностическая операция, не hot path. Она может выделять scratch memory и должна проверять:

- взаимно-однозначное соответствие ID-индекса и активных слотов;
- каждый активный ордер находится ровно в одном FIFO;
- side/price ордера совпадают с расположением в книге;
- `prev/next`, `head/tail` и отсутствие циклов;
- `order_count` и `total_quantity` каждого уровня;
- active bit уровня эквивалентен непустому FIFO;
- агрегаты каждой стороны;
- корректность cached best segment;
- уникальность активных `OrderId`;
- `remaining > 0` у каждого активного ордера;
- количество активных ордеров не превышает capacity;
- корректность freelist пула.

## 3. Нефункциональный контракт

- Single writer; внешняя сериализация обязательна.
- Все runtime-операции `noexcept`.
- После конструктора runtime-операции не аллоцируют память.
- Нет callback, виртуальных функций, locks и atomics на hot path.
- Отказ операции атомарен на уровне логического состояния книги.
- Одна и та же последовательность операций создаёт одинаковое состояние и одинаковые результаты.
- Внешний API не меняется при замене списков, массивов, pool, hash index или price topology.

## 4. Спецификация тестов

### 4.1 Black-box contract tests

Тестировать только через внешний API.

**Конструкция**

- корректные минимальный и обычный диапазоны;
- capacity `0`, `1`, `N`;
- `min_price > max_price`;
- непредставимая capacity;
- новая книга пуста: `best(Bid/Ask) == nullopt`, invariants valid.

**Insert**

- одна Bid и одна Ask;
- граничные цены диапазона;
- цена ниже/выше диапазона;
- quantity `0`;
- duplicate ID;
- заполнение до capacity и ещё одна вставка;
- ID можно повторно использовать после erase, если уникальность определена только для активных заявок;
- при каждом отказе предыдущее состояние неизменно.

**Price/FIFO priority**

- Bid выбирается по убыванию цены;
- Ask выбирается по возрастанию цены;
- несколько заявок одной цены возвращаются FIFO;
- операции одной стороны не меняют другую;
- уровни на границе соседних 64-price segments.

**Set remaining**

- уменьшение, увеличение и присваивание того же значения;
- FIFO-позиция сохраняется во всех трёх случаях;
- корректно меняются агрегаты;
- missing ID;
- quantity `0`;
- возвращается прежнее значение;
- ошибка не изменяет книгу.

**Erase**

- единственная заявка книги;
- единственная заявка уровня;
- head, middle и tail FIFO;
- лучший и не лучший ценовой уровень;
- удаление последней заявки лучшего сегмента;
- missing ID и повторный erase;
- возвращается правильный снимок удалённой заявки;
- освобождённая capacity используется повторно.

После значимых операций вызывается `validate_invariants()`.

### 4.2 Reference model

Создать только в tests простой `ReferenceOrderBook` на стандартных контейнерах, например:

```text
unordered_map<OrderId, Order>
map<PriceTick, deque<OrderId>> bids
map<PriceTick, deque<OrderId>> asks
```

Это oracle корректности, а не кандидат production implementation.

### 4.3 Differential/property tests

Генерировать воспроизводимые последовательности `insert`, `best`, `set_remaining`, `erase` и сравнивать production book с reference model:

- одинаковые статусы;
- одинаковые snapshots/results;
- одинаковые best Bid/Ask;
- invariants после каждой операции;
- seed печатается при ошибке;
- failing trace сохраняется или выводится в минимально воспроизводимом виде.

Наборы:

- 100–1 000 коротких seeds в обычном CI;
- длинный фиксированный trace в nightly/manual stress;
- узкий диапазон цен с высокой плотностью;
- широкий разреженный диапазон;
- churn около полной capacity;
- частые misses и duplicate IDs;
- цены около segment boundaries.

### 4.4 Failure atomicity

Отдельно доказать, что состояние не меняется при:

- duplicate insert;
- out-of-range price;
- invalid quantity;
- capacity exhausted;
- missing set/erase;
- невозможном внутреннем index insert, если такой путь вообще остаётся.

### 4.5 Allocation tests

После завершения конструктора установить allocation guard/counter и выполнить большой набор runtime-операций. Ожидается ноль аллокаций и освобождений до разрушения книги. `validate_invariants()` из этого теста исключается.

### 4.6 Инструментальные проверки

- Debug assertions/death tests для нарушенных внутренних preconditions;
- ASan;
- UBSan;
- MSVC `/W4`, GCC/Clang `-Wall -Wextra -Wpedantic`;
- при наличии Linux — Valgrind только как дополнительный медленный прогон;
- deterministic replay одного command trace дважды с побайтным сравнением нормализованного результата.

## 5. Спецификация benchmarks

### 5.1 Правила измерения

- Release `/O2 /DNDEBUG` или эквивалент;
- setup и заполнение не входят в hot-path interval;
- constructor/first-touch измеряется отдельным сценарием;
- минимум 5 независимых прогонов;
- batch timing для throughput, sampled timing для latency distribution;
- p50, p90, p99, p99.9, p99.99 и max;
- CPU affinity и сведения о CPU/compiler/build печатаются;
- optimizer не может удалить результат операций;
- Debug benchmarks не используются для выводов о производительности;
- сравнивается median нескольких прогонов, а не один случайный запуск.

### 5.2 Microbenchmarks внешнего API

| Сценарий | Что проверяет |
|---|---|
| `best_bid`, `best_ask` | стоимость лучшей заявки |
| `insert_existing_level` | FIFO append без нового уровня |
| `insert_new_level_same_segment` | активация bit уровня |
| `insert_new_segment` | обновление cached best segment |
| `insert_duplicate` | отрицательный ID lookup |
| `insert_capacity_full` | дешёвый отказ при полном пуле |
| `set_remaining_hit` | lookup и агрегаты |
| `set_remaining_miss` | отрицательный lookup |
| `erase_head/middle/tail` | intrusive unlink разных позиций |
| `erase_only_order_at_level` | деактивация уровня |
| `erase_best_segment_with_near_next` | короткий поиск нового best |
| `erase_best_segment_with_far_next` | worst-case scan между сегментами |
| `erase_miss` | отрицательный lookup |

### 5.3 Составные сценарии

- `best + set_remaining`: механика partial fill;
- `best + erase`: механика full fill;
- `insert + erase`: steady-state churn;
- смешанный workload, например 45% insert, 35% erase, 15% set, 5% best;
- burst insert до capacity;
- churn 10M–100M операций без деградации probe length и latency tail.

### 5.4 Распределения данных

- много заявок на одной цене;
- один ордер на каждом из многих уровней;
- плотные цены в одном segment;
- разреженные цены по всему диапазону;
- реалистичный кластер вокруг mid-price;
- occupancy книги 10%, 50%, 90%, 100%;
- последовательные, случайные и специально коллидирующие `OrderId`.

У текущего индекса bucket count примерно вдвое больше order capacity, поэтому его максимальный штатный load factor около 50%. Не следует заявлять benchmark OrderBook при 85% load ID-индекса, если конфигурация индекса этого не допускает.

### 5.5 Performance gates

- ноль runtime allocations;
- `best` не зависит от числа ордеров;
- median throughput не регрессирует более чем на согласованный порог без объяснения;
- отдельно контролируются p99.9/p99.99 `erase` и смены best segment;
- любая оптимизация сопровождается before/after, одинаковым workload и анализом assembly/perf counters только при необходимости.

## 6. Пошаговый roadmap

Текущая реализация приблизительно находится между шагами 5 и 7, но шаги 0–4 всё равно нужны как доказательство контракта и корректности.

### Шаг 0. Инвентаризация и baseline

**Цель:** понять фактический код до рефакторинга.

**Работы:** все call sites старого API; tests; compiler matrix; размеры структур; runtime allocations; текущие benchmarks. Отдельно отметить `select_best_opposite/decrement_selected`, generation state, двухфазный warm-up и публичную диагностику.

**Готово, когда:** есть короткий отчёт и воспроизводимый baseline; код не изменён.

**Prompt Codex:**

```text
Проведи read-only аудит текущего OrderBook. Изучи implementation, tests, benchmarks и все call sites. Сопоставь публичные методы с целевым API insert/best/set_remaining/erase/validate_invariants. Зафиксируй Debug/Release tests, Release benchmark baseline, размеры Order/PriceLevel/PriceSegment/index bucket и наличие runtime allocations. Отдельно перечисли зависимости от select/decrement, warm_up/page_size, generation и test macros. Ничего не изменяй, не коммить. Дай краткий отчёт и точный список файлов для следующего шага.
```

### Шаг 1. Наивный oracle и контрактные тесты

**Цель:** получить независимый критерий правильности до изменения production-кода.

**Работы:** `ReferenceOrderBook` только в tests; black-box cases; фиксированные сценарии price/FIFO; результатные типы целевого API можно сначала оформить в test model.

**Готово, когда:** reference model полностью проходит собственные контрактные тесты; production ещё не переписан.

**Prompt Codex:**

```text
Добавь в test-support простой ReferenceOrderBook на стандартных контейнерах. Он реализует семантику insert, best(side), set_remaining(id, quantity), erase(id) и validate без попыток оптимизации. Напиши black-box контрактные тесты для capacity, duplicate ID, диапазона цен, Bid/Ask priority, FIFO, set_remaining, erase и failure atomicity. Production OrderBook пока не рефактори. Запусти tests и покажи изменённые файлы. Не коммить.
```

### Шаг 2. Стабилизация внешнего API

**Цель:** заменить policy-tainted интерфейс минимальным контрактом.

**Работы:** `put -> insert`; `select_best_opposite -> best(side)` без кэширования selection; `change -> set_remaining`; `cancel -> erase`; удалить `decrement_selected`, `selected_order_`, generation state, `find` и лишние getters. `erase` возвращает removed snapshot, `set_remaining` — previous quantity.

**Готово, когда:** Matcher/test call sites используют только пять операций; `OrderIndex` не выходит наружу; differential tests проходят.

**Prompt Codex:**

```text
Рефактори только внешний контракт OrderBook: insert, best(side), set_remaining(id,new_remaining), erase(id), validate_invariants. best возвращает snapshot по значению и не создаёт selected state. Удали select/decrement protocol, selected_order и generation. set_remaining сохраняет FIFO-позицию и возвращает прежнее quantity; erase возвращает снимок удалённого ордера. Удали find и лишние публичные getters, если production call sites их не требуют. Не меняй внутренние структуры и не оптимизируй. Обнови tests/call sites, сравни с ReferenceOrderBook, запусти Debug/Release. Не коммить.
```

### Шаг 3. Единофазный lifecycle и internal boundaries

**Цель:** после конструктора существует только полностью готовая книга.

**Работы:** убрать `page_size`, `WarmUpTouchStats`, `OrderBook::warm_up`, повторные `SideBook/OrderIdIndex::warm_up`; constructors сами последовательно инициализируют storage. Переместить `SideBook`, `PriceLevel`, `PriceSegment`, `OrderIdIndex` в `detail`, если это не вызывает неоправданного churn. Test access не должен менять class definition между translation units.

**Готово, когда:** нет двухфазного состояния и ODR-зависимых test macros; constructor first-touch документирован.

**Prompt Codex:**

```text
Убери двухфазный warm-up из OrderBook и его внутренних компонентов. Конструкторы должны выделять и последовательно инициализировать всю память; отдельные page_size, touch_pages и WarmUpTouchStats удалить. Книга создаётся на окончательном owner thread. Внутренние SideBook/PriceLevel/PriceSegment/OrderIdIndex помести в detail/internal API без PIMPL и дополнительных аллокаций. Test access вынеси в test-support и не меняй определения production-классов условными macros. Не меняй алгоритмы hot path. Запусти tests и baseline benchmarks. Не коммить.
```

### Шаг 4. Транзакционность и инварианты

**Цель:** любой отказ оставляет согласованное состояние.

**Работы:** проверить порядок pool/index/side mutations и rollback; расширить `validate_invariants`; differential random tests; allocation guard; sanitizers.

**Готово, когда:** ошибки и случайные trace не ломают книгу, после конструктора нет аллокаций.

**Prompt Codex:**

```text
Усиль корректность OrderBook без оптимизации. Проверь транзакционный порядок insert/set_remaining/erase и rollback каждого возможного отказа. Расширь validate_invariants до проверки index<->pool, FIFO links, level masks, counts, quantities, cached best и уникальности ID. Добавь seeded differential/property tests против ReferenceOrderBook, failure-atomicity tests и allocation guard после конструктора. Запусти Debug, Release, ASan/UBSan при наличии. Не коммить.
```

### Шаг 5. Воспроизводимый benchmark harness

**Цель:** запретить оптимизацию «по ощущениям».

**Работы:** micro/composite/worst-case scenarios из раздела 5; batch throughput и sampled latency; build context; несколько прогонов.

**Готово, когда:** один скрипт/команда воспроизводит baseline и выдаёт сравнимую таблицу.

**Prompt Codex:**

```text
Построй Release benchmark harness только через внешний API OrderBook. Добавь best, insert existing/new level, set hit/miss, erase head/middle/tail/last-level, best-segment near/far scan, partial/full cycle и steady-state churn. Используй разные occupancy и price distributions. Setup исключи из hot interval; constructor измеряй отдельно. Печатай compiler/flags/CPU/config, throughput и p50/p90/p99/p99.9/p99.99/max. Сделай несколько прогонов и baseline-таблицу. Код OrderBook не оптимизируй. Не коммить.
```

### Шаг 6. Проверка fixed storage и intrusive FIFO

**Цель:** подтвердить, что уже реализованные pool/FIFO дают ожидаемые свойства.

**Работы:** отдельные component tests/benches; O(1) unlink head/middle/tail; reused slot fully initialized; агрегаты меняются один раз.

**Готово, когда:** нет дублирующих lookup/обходов из-за неправильной интеграции; API пула не расширяется.

**Prompt Codex:**

```text
Проведи узкий аудит интеграции готового OrderPool и intrusive FIFO с OrderBook. Проверь append/unlink для head/middle/tail, повторное использование slot, полную инициализацию Order, обновление level/side aggregates ровно один раз и отсутствие OrderIndex во внешнем API. Добавь недостающие component tests и microbenchmarks. Меняй код только при доказанном дефекте или лишней работе; покажи before/after. Не коммить.
```

### Шаг 7. ID-индекс и churn tail

**Цель:** сделать lookup/mutation устойчивыми при длительном churn.

**Работы:** проверить текущие `Empty/Deleted`, `tombstones_` и backward-shift/reinsert semantics; исключить двойные lookup при erase/set; измерить clusters и probe distributions; сравнить варианты, не внедряя новый hash table без выигрыша.

**Готово, когда:** correctness доказана differential tests, probe tail стабилен, выбранный алгоритм описан.

**Prompt Codex:**

```text
Исследуй OrderIdIndex как отдельный этап. Проверь корректность Empty/Deleted/tombstones и текущего erase cluster repair; найди dead или противоречивое состояние. Измерь find/insert/erase hit/miss, probe distribution и churn на последовательных, случайных и коллидирующих ID. Проверь, делает ли OrderBook двойной lookup при set/erase, и предложи узкую API-операцию индекса для одного поиска. Реализуй только подтверждённое benchmark улучшение, сохрани fixed capacity/no allocation и differential tests. Дай before/after. Не коммить.
```

### Шаг 8. Поиск лучшей цены и tail latency

**Цель:** убрать линейный scan сегментов из плохого хвоста, если он наблюдается.

**Работы:** benchmark удаления последнего ордера best segment при разных gaps; при необходимости добавить верхний occupancy bitmap/иерархию слов; сохранить O(1) поиск внутри 64-price segment.

**Готово, когда:** worst-case/p99.99 улучшен измеримо, memory overhead посчитан, обычный случай не регрессировал.

**Prompt Codex:**

```text
Измерь tail latency смены best segment: соседний активный segment, большой gap и активный уровень на противоположной границе диапазона. Если линейный recompute_best заметен в p99.9/p99.99, спроектируй минимальный верхний occupancy bitmap или иерархию bitmap для поиска следующего segment. Посчитай memory overhead и сложность обновления. Реализуй только после baseline; сравни обычный и worst-case пути, проверь invariants и differential tests. Не коммить.
```

### Шаг 9. Layout, cache и инструкции

**Цель:** получить последний измеримый выигрыш без разрушения читаемости.

**Работы:** размеры/alignment, cache misses, branches, locality, SoA/control bytes/prefetch только как измеряемые гипотезы; одна гипотеза за раз.

**Готово, когда:** каждое изменение имеет benchmark и объяснение; сомнительные изменения отклонены.

**Prompt Codex:**

```text
Проведи измерительный cache/layout этап OrderBook. Зафиксируй sizeof/alignment Order, PriceLevel, PriceSegment и ID bucket; собери perf/VTune counters при наличии: cycles, instructions, branches, branch misses, cache/TLB misses. Сформулируй не более трёх конкретных гипотез и проверяй по одной. Не добавляй padding, prefetch, SoA или branch hints без воспроизводимого выигрыша. Для принятого изменения покажи код, memory trade-off и before/after median + tail. Не коммить.
```

### Шаг 10. Soak, portability и showcase

**Цель:** компонент, который не стыдно показывать профессионалам.

**Работы:** длительный churn; compiler/OS matrix; warnings; документация контракта, инвариантов, сложности и benchmark methodology; таблица честных результатов и известных ограничений.

**Готово, когда:** clean tests/sanitizers, воспроизводимые benchmarks, нет неподтверждённых performance claims.

**Prompt Codex:**

```text
Подготовь OrderBook к showcase без нового функционала. Запусти длительный seeded churn/stress, Debug/Release, sanitizers и доступные MSVC/GCC/Clang сборки. Устрани warnings и portability defects. Обнови README/docs: граница OrderBook vs Matcher, пять методов API, invariants, complexity, fixed-capacity/no-allocation contract, first-touch requirements, benchmark methodology, hardware/compiler context, результаты и честные ограничения. Добавь одну воспроизводимую команду tests и одну benchmarks. Не преувеличивай гарантии и не коммить.
```

## 7. Когда компонент можно считать готовым

Минимальный production-ready уровень достигнут после шагов 0–5:

- устойчивый внешний API;
- oracle и differential tests;
- доказанные инварианты и failure atomicity;
- отсутствие runtime allocations;
- воспроизводимый baseline.

Уровень «можно показывать low-latency профессионалам» достигнут после шагов 6–10:

- измерены обычные и худшие пути;
- ID-index не деградирует при churn;
- контролируется tail смены best price;
- layout решения подтверждены counters/benchmarks;
- есть stress, compiler matrix и честная документация.

Главное правило roadmap: каждый следующий уровень сохраняет внешний интерфейс. Меняются только внутренние структуры, тестовая глубина и доказательства производительности.
