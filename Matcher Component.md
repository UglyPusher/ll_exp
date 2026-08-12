# Matcher Component

## Назначение

`Matcher` — это stateful-компонент matching engine, который последовательно читает входные команды через абстрактный интерфейс источника команд, применяет их к своему внутреннему состоянию и публикует результирующие события через абстрактный интерфейс получателя событий.

Документ описывает целевую архитектурную границу компонента. Текущий минимальный sample намеренно реализует только базовую модель `running / stopped / fatal`, один `OrderBook`, limit-order matching и ordered shutdown command; полноценные `MatcherFSM`, auction/halt/resume и несколько книг являются target architecture, а не требованием к первому образцу.

`Matcher` является владельцем **matching state**, но не владельцем **execution infrastructure**.

Он не создаёт поток, не закрепляет его за CPU, не настраивает scheduler, IRQ affinity, isolation, NUMA policy и другие параметры среды исполнения.

Среда исполнения подготавливается внешним кодом, после чего этот код вызывает `Matcher::run()` в уже подготовленном потоке.

---

## Концептуальная модель

```text
External execution environment
(main / process runtime / matcher executor / test harness)
                 |
                 | prepares current thread:
                 | - CPU affinity
                 | - scheduling policy / priority
                 | - CPU isolation
                 | - IRQ placement
                 | - NUMA / memory policy
                 | - other runtime tuning
                 |
                 v
          +----------------+
          | Matcher::run() |
          +-------+--------+
                  |
          +-------v---------+
          | CommandReader   |
          +-------+---------+
                  |
             next command
                  |
                  v
          +---------------+
          | Matcher FSM   |
          | Order Books   |
          | Matcher State |
          +-------+-------+
                  |
             emitted events
                  |
                  v
          +---------------+
          | EventWriter   |
          +---------------+
```

---

## Что такое Matcher

С архитектурной точки зрения `Matcher` — это **активный stateful-компонент с собственным processing loop**, но без собственного execution context.

То есть у него есть:

- внутреннее mutable state;
- FSM;
- order books;
- matching logic;
- последовательный цикл обработки команд;
- абстрактный источник команд;
- абстрактный sink для событий.

Но у него нет:

- `std::thread`;
- thread creation;
- CPU affinity management;
- scheduler configuration;
- IRQ management;
- знания о Command WAL;
- знания об Event WAL;
- знания о процессе, в котором он запущен.

Ключевой принцип:

> `Matcher` владеет алгоритмом и состоянием.  
> Внешняя среда владеет ресурсами исполнения.

---

## Execution model

`Matcher::run()` является синхронной блокирующей функцией.

Она выполняется **в потоке вызывающего кода**.

Пример:

```cpp
int main()
{
    configure_realtime_environment();
    pin_current_thread_to_cpu(7);

    CommandReader command_reader{...};
    EventWriter event_writer{...};

    Matcher matcher{
        command_reader,
        event_writer,
        matcher_config
    };

    matcher.run();

    return 0;
}
```

В данном случае `main()` фактически является executor'ом матчера.

Никакой специальный класс `MatcherExecutor` архитектурно не обязателен.

То же самое можно оформить отдельным runtime-компонентом:

```cpp
void MatcherExecutor::run()
{
    prepare_current_thread();

    matcher_.run();
}
```

Это инфраструктурная деталь, а не часть контракта `Matcher`.

---

## Поток принадлежит executor'у, но выполняет Matcher

Важно различать ownership и execution.

Матчер **не владеет thread object**, однако весь его mutable state предполагается принадлежащим одному execution thread.

После входа в:

```cpp
matcher.run();
```

этот поток становится единственным владельцем runtime-state матчера на всё время работы.

Таким образом:

```text
Executor owns thread resource
        |
        v
Thread executes Matcher::run()
        |
        v
Matcher exclusively owns mutable matching state
```

Это позволяет получить single-writer semantics без mutex'ов на hot path.

---

## Command source

`Matcher` не знает, что команды физически находятся в `Command WAL`.

Он получает интерфейс примерно следующего уровня:

```cpp
enum class CommandReadStatus : std::uint8_t {
    Ok,
    Empty,
    Fatal
};

struct CommandReadResult {
    CommandReadStatus status;
    Command command;
};

class ICommandReader {
public:
    virtual ~ICommandReader() = default;

    virtual CommandReadResult read_next() = 0;
};
```

или low-latency вариант без virtual dispatch:

```cpp
template<class CommandReader, class EventWriter>
class Matcher {
public:
    Matcher(CommandReader& commands,
            EventWriter& events);

    void run();
};
```

Конкретная реализация `CommandReader` может читать:

- Command WAL;
- shared-memory ring;
- replay-файл;
- заранее подготовленный массив команд;
- synthetic test source.

Для `Matcher` эти варианты эквивалентны.

Это принципиальная граница:

```text
Matcher knows:
    "give me the next command"

Matcher does NOT know:
    "read record N from Command WAL"
```

Graceful shutdown не является отдельным статусом reader. Остановка Matcher должна приходить как ordered `CommandType::Shutdown` внутри общего command stream. Это сохраняет ordering относительно всех предыдущих business/control commands.

`Empty` означает polling: команда сейчас недоступна, `Matcher::run()` остаётся активным, не делает state transition и продолжает цикл чтения. Конкретная idle-стратегия — pure spin, pause/backoff, метрики или reader-owned wait policy — задаётся отдельно и не является частью текущего минимального sample.

---

## Event output

Аналогично, `Matcher` не должен знать, куда физически записываются события.

Он работает с абстракцией:

```cpp
class IEventWriter {
public:
    virtual ~IEventWriter() = default;

    virtual PublishResult publish(const Event&) noexcept = 0;
};
```

Конкретный writer может писать:

```text
Event WAL
Shared-memory queue
Replay collector
Test vector
Network publisher
```

Матчер формирует **семантику событий**, но не владеет механизмом их durability или transport.

`EventWriter::publish()` имеет только два наблюдаемых результата: `Ok` и `Fatal`. Временная нехватка capacity не является ошибкой: writer применяет backpressure и ждёт освобождения места. Поэтому в контракте нет `WouldBlock`, `Retry` или похожих состояний.

`PublishStatus::Fatal` означает, что writer больше не может предоставить свой publication contract. Для конкретного failing event возможны обе ситуации: событие гарантированно не принято или результат публикации неизвестен. После `Fatal` matcher немедленно прекращает дальнейшую обработку команд и `Matcher::run()` возвращает fatal-статус. Откат уже изменённого состояния matcher не требуется: текущий экземпляр считается непригодным для продолжения, а восстановление выполняется внешним runtime через replay от последнего валидного snapshot. Fatal diagnostics являются out-of-band обязанностью runtime/executor, а не частью matcher event stream.

---

## Processing loop

Концептуально `Matcher::run()` выглядит так:

```cpp
RunResult Matcher::run()
{
    initialize();

    while (running()) {
        CommandReadResult read = commands_.read_next();

        switch (read.status) {
            case CommandReadStatus::Ok:
                process(read.command);
                break;
            case CommandReadStatus::Empty:
                break;
            case CommandReadStatus::Fatal:
                return {RunStatus::Fatal};
        }
    }

    finalize();
    return {RunStatus::Stopped};
}
```

При этом:

```cpp
void Matcher::process(const Command& command)
{
    switch (fsm_.state()) {
        case State::Continuous:
            process_continuous(command);
            break;

        case State::Auction:
            process_auction(command);
            break;

        case State::Halted:
            process_halted(command);
            break;
    }
}
```

Главное свойство — команды применяются **строго последовательно**.

Для одного экземпляра Matcher отсутствует конкурентная обработка двух команд.

Для команд, создающих новый ордер, `OrderId` назначается upstream до попадания
в Matcher. В рамках одной matcher epoch входящие `OrderId` должны быть строго
монотонно возрастающими:

```cpp
command.order_id > last_order_id_
```

Пара `(EpochId, OrderId)` является глобальным идентификатором ордера. Текущий
sample не вводит epoch infrastructure; `last_order_id_` относится только к
текущей epoch.

Нарушение монотонности не является пользовательской или business ошибкой и не
публикует `OrderRejected`. Это fatal-нарушение ordered command stream:

```text
order_id <= last_order_id_ -> Fatal
```

Так как при `NonMonotonicOrderId` сам `EventWriter` не является источником
ошибки, Matcher публикует terminal `MatcherFatal` event с причиной,
offending `OrderId` и последним принятым `last_order_id_`, а затем завершает
обработку. Для `EventWriterFatal` такой marker не публикуется через тот же
writer, потому что publication channel уже ненадёжен.

После успешной проверки ID считается потреблённым независимо от дальнейшего
результата обработки: accepted, rejected, fully filled, partially filled или
rested. Business rejection не откатывает `last_order_id_`.

---

## FSM и lifecycle потока — разные вещи

FSM матчера не управляет жизненным циклом execution thread.

Например:

```text
Matcher FSM:

Initializing
    |
    v
Stopped
    |
    v
Auction
    |
    v
Continuous
    |
    v
Halted
```

состояние:

```text
Halted
```

не означает:

```text
thread stopped
```

Поток продолжает выполнять `Matcher::run()` и читать команды.

Это необходимо, потому что из состояния `Halted` может прийти следующая управляющая команда:

```text
Resume
StartAuction
Shutdown
...
```

Поэтому нужно различать:

```text
Market state
```

и:

```text
Execution lifecycle
```

---

## Shutdown

Завершение `Matcher::run()` должно происходить по явному протоколу.

Предпочтительно, чтобы shutdown также приходил через последовательность команд:

```text
CommandReader
      |
      v
Shutdown command
      |
      v
Matcher FSM / lifecycle logic
      |
      v
Matcher::run() returns
```

Тогда ordering относительно остальных команд остаётся детерминированным.

Внешний executor после возврата из `run()` может освобождать execution infrastructure.

```cpp
prepare_thread();

matcher.run();

cleanup_thread();
```

Matcher не должен самостоятельно уничтожать или завершать поток.

Он просто возвращает управление вызывающему коду.

---

## Возможный интерфейс класса

Концептуально:

```cpp
template<
    typename CommandReader,
    typename EventWriter
>
class Matcher {
public:
    Matcher(
        CommandReader& command_reader,
        EventWriter& event_writer,
        const MatcherConfig& config
    );

    Matcher(const Matcher&) = delete;
    Matcher& operator=(const Matcher&) = delete;

    void run();

private:
    void process(const Command& command);

    CommandReader& commands_;
    EventWriter& events_;

    MatcherState state_;
    MatcherFSM fsm_;

    OrderBooks books_;
};
```

При необходимости можно отдельно оставить primitive для тестирования:

```cpp
void process(const Command&);
```

Однако production execution path остаётся:

```cpp
matcher.run();
```

---

## Почему loop находится внутри Matcher

Есть альтернативный дизайн:

```cpp
while (...) {
    auto command = reader.read_next();
    matcher.process(command);
}
```

где processing loop принадлежит executor'у.

Для данной архитектуры это менее желательно.

Такой вариант заставляет внешний executor знать protocol обработки Matcher:

```text
как получать команды;
когда вызывать matcher;
что делать при idle;
как трактовать termination;
какой ordering соблюдать;
как выглядит processing cycle.
```

Тем самым executor начинает владеть частью семантики Matcher.

При варианте:

```cpp
matcher.run();
```

граница значительно чище.

Executor знает только:

> «Я предоставляю этому компоненту подготовленный execution context и запускаю его».

Matcher знает:

> «В этом execution context я последовательно выполняю свой processing protocol».

---

## Почему Matcher не создаёт поток самостоятельно

Обратный вариант:

```cpp
class Matcher {
    std::thread thread_;

public:
    void start();
    void stop();
};
```

также нежелателен.

Он связывает matching component с конкретным способом исполнения и заставляет Matcher знать об инфраструктуре, которая находится за пределами его ответственности.

Для low-latency системы подготовка thread может включать:

```text
CPU pinning
CPU isolation
scheduler policy
scheduler priority
IRQ affinity
RCU configuration
NUMA placement
memory prefaulting
huge pages
frequency / power management
```

Эти решения относятся к deployment/runtime layer.

Matcher не должен содержать эту политику.

---

## Рекомендуемая граница ответственности

### Matcher

Отвечает за:

```text
command consumption protocol
matching FSM
order books
matching algorithms
matching invariants
deterministic state transitions
event generation
ordering
```

### CommandReader

Отвечает за:

```text
предоставление последовательности команд Matcher'у
```

Может быть адаптером над Command WAL, но Matcher этого не знает.

### EventWriter

Отвечает за:

```text
приём последовательности событий Matcher'а
```

Может быть адаптером над Event WAL, но Matcher этого не знает.

### Executor / main / runtime

Отвечает за:

```text
создание execution thread
CPU affinity
RT scheduling
CPU isolation
IRQ setup
NUMA placement
runtime preparation
создание зависимостей Matcher
вызов Matcher::run()
cleanup после завершения
```

---

## Основной архитектурный инвариант

Для одного экземпляра `Matcher`:

```text
ONE matcher
    +
ONE mutable matching state
    +
ONE execution thread at a time
    +
ONE ordered command stream
    =
deterministic state machine
```

При этом конкретный execution thread не является частью `Matcher`.

---

## Короткое определение для Codex

> `Matcher` is a single-threaded stateful processing component implementing the matching-engine FSM and owning all mutable matching state. It receives abstract command-reader and event-writer interfaces as dependencies. `Matcher::run()` synchronously executes the command-consumption and matching loop in the caller's already-prepared thread. Matcher must not create, configure, pin, stop, or otherwise manage threads and must not know about Command WAL, Event WAL, CPU affinity, scheduler policy, IRQ placement, or other runtime infrastructure. The external execution environment — which may be as simple as `main()` — prepares the execution context and invokes `Matcher::run()`.
