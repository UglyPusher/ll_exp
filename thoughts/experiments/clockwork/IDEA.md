# Clockwork — deterministic multi-clock machine over an ordered tract

## Статус

Experimental concept.

Не является частью архитектуры exchange и не накладывает требований на production tract.

Цель эксперимента — проверить, можно ли поверх существующей механики ordered immutable tract построить небольшую детерминированную программируемую машину с независимыми тактовыми доменами, feedback и воспроизводимой временной динамикой.

## 1. Основная идея

Машина строится поверх единого упорядоченного append-only `RecordTape`.

```text
HEAD → [0] → [1] → [2] → [3] → ... → TAIL
          ↑       ↑       ↑
         M1      M2      M3
```

К `RecordTape` подключён набор stateful-модулей. Каждый модуль:

- имеет собственный frontier;
- последовательно читает записи `RecordTape`;
- имеет собственное внутреннее состояние;
- выполняет небольшую программу/FSM;
- имеет собственный детерминированный тактовый генератор;
- может записывать результат обработки в принадлежащий ему pocket записи;
- может породить новую запись через feedback;
- может реагировать на управляющие записи, адресованные этому модулю.

Опубликованная запись `RecordTape` immutable. Модуль не может изменить исходное событие или уже опубликованную историю.

## 2. Независимые clocks

У каждого модуля имеется собственный clock domain.

```text
M1: period = 2
M2: period = 3
M3: period = 5
M4: period = 7
```

Модули поэтому продвигаются по одному и тому же `RecordTape` с различной скоростью. Наличие общего `RecordTape` не означает наличия общей пошаговой синхронизации модулей. Глобального `tick`, на котором все модули одновременно выполняют `step()`, нет.

## 3. Детерминированное логическое время

Несмотря на независимые clocks, исполнение машины полностью детерминировано. Scheduler должен однозначно определять следующее вычислительное событие.

```text
next_event = min(next_tick(M1), next_tick(M2), ..., next_tick(MN))
```

При совпадении logical time используется стабильный deterministic tie-break, например:

```text
(logical_time, module_id, local_event_seq)
```

Конкретная модель времени и арбитраж являются частью semantics машины и должны быть явно определены.

Одинаковые initial state, initial tract, module programs и clock configuration обязаны давать идентичный execution trace.

## 4. Feedback

Модуль может породить новую запись в тракт.

```text
HEAD → M1 → M2 → M3 → M4 → TAIL
        ↑              │
        └── feedback ──┘
```

Feedback не изменяет прошлую запись и не перемещает frontier назад. Он создаёт новую запись, которая публикуется в append-only history и впоследствии снова проходит через модули.

Таким образом, логическая обратная связь существует поверх физически однонаправленной immutable history.

Несколько модулей могут независимо генерировать feedback. Порядок одновременных feedback emissions должен определяться scheduler semantics, а не scheduling ОС или mutex race.

## 5. Управление модулями через сам тракт

Конфигурация машины может изменяться записями того же `RecordTape`.

Например:

```text
SET_CLOCK M3 7
```

Это обычная immutable запись. Она существует в глобальной истории с момента публикации, однако влияет на `M3` только тогда, когда `M3` своим frontier достигает этой записи.

```text
SET_CLOCK M3 7 published
        ↓
other modules continue
        ↓
M3 reaches command
        ↓
M3 applies new clock configuration
```

Таким образом, изменение скорости самого consumer распространяется через поток данных, который этот consumer должен успеть прочитать. Это намеренное свойство машины.

## 6. Clock phase

Состояние clock не ограничивается nominal period. Как минимум логическое состояние модуля включает:

```text
module_state
frontier
clock_period
clock_phase / next_tick
```

Изменение периода не должно неявно уничтожать информацию о текущей фазе. Точная семантика `SET_CLOCK` относительно уже запланированного следующего tick должна быть определена отдельно. Различные варианты такой семантики могут использоваться как разные execution policies.

## 7. Детерминированные паразитные влияния

Вычислительная активность одного модуля может влиять на временные характеристики других модулей.

Это влияние не является random jitter. Оно полностью детерминировано и является частью модели машины.

```text
M1 executes expensive operation
        ↓
shared resource load increases
        ↓
effective timing of M3 changes
```

Концептуально:

```text
effective_clock(M, state) =
    nominal_clock(M)
    + deterministic_interference(M, machine_state)
```

Источниками interference могут быть:

- активность других модулей;
- состояние общего вычислительного ресурса;
- определённые классы инструкций;
- количество одновременно активных модулей;
- накопленное состояние виртуального ресурса;
- другие явно заданные детерминированные факторы.

Никакого физического wall-clock времени для определения результата использоваться не должно.

## 8. Косвенное управление clocks

Специального механизма `M1 changes clock of M2` не требуется.

Если программа `M1` должна изменить частоту `M2`, она может породить feedback:

```text
M1
 ↓
emit SET_CLOCK M2 11
 ↓
append to tract
 ↓
...
 ↓
M2 eventually reaches command
 ↓
clock changes
```

Таким образом, межмодульное управление также проходит через общую ordered history. Это сохраняет причинность наблюдаемой и replayable.

## 9. Причинные цепочки

Из простых правил могут возникать длинные детерминированные временные зависимости.

```text
record A
 ↓
M1 performs operation
 ↓
interference changes timing of M3
 ↓
M3 reaches record B later
 ↓
M3 emits SET_CLOCK M2
 ↓
command appended to tract
 ↓
M2 eventually reaches command
 ↓
M2 changes frequency
 ↓
relative phase M2/M4 changes
 ↓
M2 emits feedback before M4
 ↓
different record ordering at HEAD
 ↓
later machine state changes
```

Вся цепочка при этом полностью воспроизводима. Сложность возникает не из nondeterminism, а из композиции нескольких детерминированных измерений состояния.

## 10. Состояние машины

Концептуально полное состояние может быть представлено как:

```text
MachineState {
    tract;

    modules[] {
        frontier;
        state;
        program;

        clock_period;
        clock_phase;
        next_tick;
        local_event_seq;
    };

    scheduler_state;
    interference_state;
    pending_feedback;
}
```

Конкретное представление не фиксируется этой концепцией. Существенно только то, что состояние должно быть достаточным для точного продолжения исполнения.

## 11. Replay

Машина должна поддерживать exact deterministic replay.

Для одинаковых initial machine state, initial tract и module programs должны совпадать:

- module execution order;
- frontier progression;
- clock changes;
- interference effects;
- feedback generation;
- feedback ordering;
- tract append order;
- module state transitions;
- Tail output.

В идеале должен совпадать полный trace:

```text
logical_time
module
frontier
instruction/action
state_before
state_after
emitted_records
clock_before
clock_after
```

Это позволяет использовать машину не только как игрушку, но и как stress-test детерминированной tract-механики.

## 12. Snapshot

Если существующая snapshot-механика действительно является общей, Clockwork представляет для неё неприятный тест.

Snapshot должен быть способен сохранить состояние машины так, чтобы непрерывный run и run → snapshot → stop → load snapshot → continue давали идентичный последующий trace.

Следовательно, одного сохранения FSM state недостаточно. Необходимо восстановить как минимум всё логически значимое состояние clocks, scheduler, frontiers и interference model.

Clockwork не определяет snapshot architecture. Он лишь может использоваться для проверки её достаточности.

## 13. Программируемые модули

Модуль должен быть намеренно маленьким. Предполагаемая модель ближе к small FSM / tiny instruction machine, чем к произвольному C++ callback.

Минимальный instruction set может включать операции типа:

```text
LOAD
STORE
ADD
SUB
CMP
JMP
JZ
JNZ
READ
EMIT
SET_STATE
```

Clock management желательно оставить событиям тракта, а не привилегированной внешней функции программы. Instruction set на данном этапе не фиксируется.

## 14. Задачи

Машина должна позволять задавать:

```text
initial tract
+
module programs
+
initial clocks
+
interference rules
+
expected Tail output
```

и проверять результат.

Простейшие задачи могут использовать общий или почти общий clock.

Более сложные: independent clocks.

Задачи со звёздочкой: independent clocks + multiple feedback producers + `SET_CLOCK` records.

Особо извращённые: independent clocks + feedback + dynamic clock changes + deterministic computational interference.

Цель задачи может состоять не только в получении результата, но и в соблюдении ограничений: max records, max module state, max instructions, max logical time, forbidden direct communication.

## 15. Намеренная когнитивная сложность

Clockwork не должен скрывать временную модель от пользователя.

Игрок/исследователь должен иметь возможность рассуждать одновременно о нескольких координатах:

```text
tract position
×
module frontier
×
FSM state
×
local clock phase
×
current clock period
×
feedback causality
×
interference state
```

Это не побочный эффект. Это одна из центральных особенностей эксперимента.

При этом каждое отдельное правило должно оставаться простым и формально определённым. Сложность должна возникать из композиции правил, а не из исключений и неявного поведения.

## 16. Что эксперимент НЕ должен делать на первом этапе

Не нужны:

- GUI;
- полноценный DSL;
- редактор уровней;
- графическая визуализация clocks;
- оптимизирующий compiler;
- физически параллельное исполнение;
- wall-clock simulation;
- random jitter;
- networking;
- production-grade persistence;
- попытка превратить эксперимент в отдельный продукт.

Первый этап — маленький executable поверх существующей tract implementation.

## 17. Первый proof-of-concept

Минимальная конфигурация:

```text
M1 period=2
M2 period=3
M3 period=5

HEAD → M1 → M2 → M3 → TAIL
        │          │
        └ feedback ┤
                   └ feedback
```

Необходимо реализовать:

1. независимое продвижение трёх модулей;
2. deterministic scheduler;
3. deterministic arbitration simultaneous events;
4. feedback от двух модулей;
5. `SET_CLOCK`;
6. полный execution trace;
7. повторный запуск с проверкой идентичности trace.

После этого добавить один простой deterministic interference rule.

Например:

```text
while M1 is in state BUSY:
    effective period of M3 += 1
```

Если после этого машина остаётся понятной, воспроизводимой и интересной для программирования — эксперимент имеет смысл продолжать.

## 18. Главный технический вопрос эксперимента

Эксперимент должен в первую очередь ответить не на вопрос:

> Можно ли написать такую игрушку?

Очевидно, можно.

Главный вопрос:

> Можно ли построить детерминированную многотактовую feedback-машину поверх существующей ordered tract abstraction, не изменяя фундаментальную механику самого `RecordTape`?

Если да, это будет дополнительным свидетельством того, что tract является общей вычислительной абстракцией, а не специализированной конструкцией matching-engine pipeline.

Если нет, места, где Clockwork ломает существующую абстракцию, потенциально значительно интереснее самой игрушки.
