# QExcel — контракты MVP

**Версия:** 1.1-draft · **Дата:** 2026-10-29 · **Относится к:** SPEC.md 0.4, объём MVP

**Изменения 1.1** (по итогам фазы 0, `PHASE0_REPORT.md` §5): `''` в Oracle-результатах — это NULL (§3.3); Oracle `DATE` приходит без времени (§3.3); время — до 9 знаков дроби с отбрасыванием до микросекунд; `timestamptz` выводится по значениям со смещением; `fetch_size` по умолчанию 10 000 (§5.1); векторы T16–T25; закрыты Q2–Q4 и A1–A4, добавлены Q5–Q6 (§15–16); добавлены векторы проверок запроса G1–G19 (§8.1.1).

## 0. Как читать этот документ

- Это **нормативный** документ для реализации MVP. `SPEC.md` объясняет, зачем и почему; здесь — точные форматы, правила и ожидаемые результаты.
- При расхождении с `SPEC.md` для MVP действует этот документ. Найденное расхождение — повод исправить один из документов, а не выбирать по ситуации.
- Слова **ДОЛЖЕН**, **НЕ ДОЛЖЕН**, **МОЖЕТ** — обязательность, как в RFC 2119.
- Пометка **[Ф0]** — допущение, которое проверяется в фазе 0. Пока оно не подтверждено, реализация следует этому документу, а при опровержении документ меняется до того, как меняется код.
- Тестовые векторы в таблицах — **приёмка**. Каждый вектор ДОЛЖЕН стать автоматическим тестом. Реализация, не проходящая вектор, не готова, даже если «по смыслу правильная».
- Всё, чего здесь нет и нет в объёме MVP (`SPEC.md`, блок «Объём MVP»), реализовывать НЕ НУЖНО. Если без этого задачу не сделать — остановиться и спросить.

---

## 1. Общие соглашения

| Тема | Правило |
|---|---|
| Кодировка | UTF-8 везде: файлы, HTTP, JSON, SQL-тексты |
| Время в метаданных | UTC, тип DuckDB `TIMESTAMP`; в JSON — строка `YYYY-MM-DDTHH:MM:SSZ` |
| Идентификаторы запросов, импортов, подключений | `^[a-z][a-z0-9_]{0,62}$` |
| UUID | v4, строка в нижнем регистре с дефисами |
| Хеш | SHA-256, строка из 64 шестнадцатеричных символов в нижнем регистре |
| JSON в DuckDB | Колонки с JSON — тип `VARCHAR` (расширение `json` не линкуется) |
| Порядок ключей | Где нужен канонический JSON — ключи отсортированы по байтам UTF-8, без пробелов |
| Пути на диске | `QEXCEL_HOME`: macOS — `~/Library/Application Support/QExcel`, Windows — `%LOCALAPPDATA%\QExcel`. Проекты — `~/QExcel/projects/<проект>/queries/` |

### 1.1 Хеш определения
`definition_hash = sha256(bytes)`, где `bytes` — текст файла запроса после двух преобразований: удалить BOM UTF-8 в начале; заменить `\r\n` и одиночные `\r` на `\n`. Больше никаких нормализаций (пробелы, регистр, комментарии сохраняются).

### 1.2 Канонические параметры
`canonical_params` — канонический JSON-объект `{имя: значение}` по всем параметрам запроса **после** применения значений по умолчанию. Значения — строки в канонической форме типа (§3.1, колонка «Каноническая форма»); `list<T>` — JSON-массив канонических строк в исходном порядке.

### 1.3 Ключ экземпляра dataset'а
```
instance_key = sha256(query_id + "\n" + definition_hash + "\n" + connection_name + "\n" + canonical_params)
```
Для запроса с `@engine: local` в `connection_name` пишется `local`.

---

## 2. Формат файла запроса

### 2.1 Структура
- **Заголовок** — все строки от начала файла, пока строка (после удаления ведущих пробелов) начинается с `--` или пуста.
- Строка заголовка вида `-- @ключ: значение` — директива. Остальные строки заголовка — комментарии, игнорируются.
- **Тело** — всё после заголовка. Тело ДОЛЖНО быть непустым после удаления пробелов и комментариев.
- Ключи директив — в нижнем регистре. Неизвестный ключ — ошибка `E_QUERY_PARSE` (опечатка не должна молча игнорироваться).
- Повтор неповторяемой директивы — `E_QUERY_PARSE`.

### 2.2 Директивы MVP

| Директива | Повтор | Обязательна | Грамматика значения |
|---|---|---|---|
| `@id` | нет | да | идентификатор (§1) |
| `@engine` | нет | да | `oracle` \| `local` |
| `@version` | нет | да | целое `1..2147483647` |
| `@title` | нет | нет | текст до конца строки, ≤ 200 символов |
| `@description` | да | нет | текст; строки объединяются через `\n` |
| `@connection` | нет | нет | имя подключения; только для `oracle` |
| `@param` | да | нет | `имя: тип [= по_умолчанию] [\| описание]` |
| `@column` | да | нет | `ИМЯ: тип_колонки [\| заголовок]` |
| `@depends` | да | нет | только для `local`: `query_id(арг := значение, …)` или `xl_import.имя` |
| `@refresh` | нет | нет | `full`. Значение `incremental(...)` распознаётся и даёт `E_UNSUPPORTED` |
| `@max_rows` | нет | нет | целое `1..1048575` |

Грамматика (EBNF, пробелы между лексемами допускаются):
```
param      = name ":" ptype [ "=" default ] [ "|" text ] ;
ptype      = "integer" | "number" | "decimal(" int "," int ")" | "string" | "id"
           | "date" | "timestamp" | "boolean" | "enum(" word { "," word } ")"
           | "list<" scalar_ptype ">" ;
scalar_ptype = любой ptype, кроме list и enum ;
default    = '"' { любой символ, '\"' — кавычка } '"' | текст до "|" или конца строки (обрезается) ;
column     = colname ":" ctype [ "|" text ] ;
colname    = [A-Za-z][A-Za-z0-9_$#]* ;          (сравнивается без учёта регистра)
ctype      = "id" | "integer" | "decimal(" int "," int ")" | "number" | "string" | "text"
           | "date" | "timestamp" | "timestamptz" | "boolean" ;
depends    = query_id "(" [ dep_arg { "," dep_arg } ] ")" | "xl_import." name ;
dep_arg    = name ":=" ( "$" name | literal ) ;
literal    = "'" текст_с_удвоением_кавычки "'" | число | "DATE '" YYYY-MM-DD "'" | "TRUE" | "FALSE" ;
word       = [A-Za-z0-9_]+ ;
```
- Имена параметров — идентификаторы (§1), уникальные без учёта регистра.
- `decimal(p,s)`: `1 ≤ p ≤ 38`, `0 ≤ s ≤ p`.
- Значение по умолчанию проверяется по типу при разборе; неверное — `E_QUERY_PARSE`.
- `@param` и `@column` объявлены в том порядке, в котором записаны; порядок `@param` — это порядок позиционных аргументов формул (§10).

### 2.3 Векторы разбора

| # | Фрагмент заголовка | Результат |
|---|---|---|
| P1 | `-- @id: ap_invoices` `-- @engine: oracle` `-- @version: 1` + тело `SELECT 1 FROM dual` | OK |
| P2 | без `@version` | `E_QUERY_PARSE` «нет обязательной директивы @version» |
| P3 | `-- @engnie: oracle` | `E_QUERY_PARSE` «неизвестная директива @engnie» |
| P4 | `-- @param bu: integer = 204 \| Бизнес-единица` | имя `bu`, тип `integer`, по умолчанию `204`, описание «Бизнес-единица» |
| P5 | `-- @param d: date = 2026-13-01` | `E_QUERY_PARSE` (неверная дата) |
| P6 | `-- @param s: string = "a \| b"` | по умолчанию `a \| b` |
| P7 | `-- @param st: enum(OPEN,PAID,ALL) = ALL` | OK |
| P8 | `-- @param st: enum(OPEN,PAID) = ALL` | `E_QUERY_PARSE` (по умолчанию не из списка) |
| P9 | `-- @param ids: list<enum(A,B)>` | `E_QUERY_PARSE` (enum внутри list не поддерживается) |
| P10 | `-- @column VENDOR_ID: id \| Поставщик` | колонка `VENDOR_ID`, тип `id`, заголовок «Поставщик» |
| P11 | `-- @refresh: incremental(key = id, watermark = d)` | `E_UNSUPPORTED` |
| P12 | `-- @depends: ap_invoices(bu := $bu)` в запросе `@engine: oracle` | `E_QUERY_PARSE` (@depends только для local) |
| P13 | `-- @connection: fusion_prod` в запросе `@engine: local` | `E_QUERY_PARSE` |
| P14 | две строки `-- @id: …` | `E_QUERY_PARSE` (повтор) |
| P15 | файл только с заголовком | `E_QUERY_PARSE` (пустое тело) |
| P16 | `-- @param BU: integer` и `-- @param bu: string` | `E_QUERY_PARSE` (имена совпадают без учёта регистра) |

---

## 3. Типы

### 3.1 Типы параметров

| Тип | Что принимается на входе (JSON от панели) | Каноническая форма | Отказ |
|---|---|---|---|
| `integer` | число без дробной части, \|x\| ≤ 2^53; строка `^-?[0-9]{1,38}$` | `-?[1-9][0-9]*` или `0` | дробь, экспонента, пусто |
| `number` | число; строка десятичной записи без экспоненты | кратчайшая запись, обратимая в double (`std::to_chars`) | NaN, ±Inf, экспонента в строке |
| `decimal(p,s)` | число; строка `^-?[0-9]*(\.[0-9]*)?$` с хотя бы одной цифрой | без ведущих нулей (кроме `0`), без лишних нулей дробной части, `-0` → `0` | больше `p` значащих цифр или больше `s` знаков после точки |
| `string` | строка; число или boolean → строка в канонической форме их типа | строка как есть | NUL и символы C0, кроме TAB/LF/CR; длина > 4000 байт UTF-8 |
| `id` | строка `^[0-9]{1,38}$`; число без дробной части ≤ 2^53 | цифры без ведущих нулей (кроме `0`) | иное |
| `date` | строка `YYYY-MM-DD` | `YYYY-MM-DD` | несуществующая дата |
| `timestamp` | строка `YYYY-MM-DD[ T]HH:MM:SS[.f{1,6}]` | `YYYY-MM-DD HH:MM:SS[.ffffff]` (дробь — только если ненулевая, без хвостовых нулей) | иное |
| `boolean` | `true`/`false`; строки `Y/N`, `TRUE/FALSE`, `1/0` без учёта регистра | `true` / `false` | иное |
| `enum(…)` | строка, совпадающая с вариантом без учёта регистра | вариант в написании из объявления | не из списка |
| `list<T>` | JSON-массив элементов `T` | массив канонических форм | любой элемент не проходит `T`; больше 100 000 элементов |

- **Даты из Excel приходят строками.** Панель ДОЛЖНА превращать серийные даты Excel в `YYYY-MM-DD` / `YYYY-MM-DD HH:MM:SS` сама, с учётом системы дат книги (1900 или 1904). Демон серийных дат не принимает.
- **Пустое значение** (`null`, `""`, отсутствие ключа) → значение по умолчанию; если его нет — `E_PARAM_MISSING`. Для `list<T>` пустой массив `[]` — это значение, а не пустота.

### 3.2 Типы колонок результата

| Тип | Хранение в DuckDB | JSON (демон → панель) | Запись в Excel |
|---|---|---|---|
| `id` | `VARCHAR` | строка | строка; формат колонки `@` |
| `integer` | `BIGINT` | число | число |
| `decimal(p,s)` | `DECIMAL(p,s)` | **строка** в канонической форме `decimal` | `Number(строка)` |
| `number` | `DOUBLE` | число; NaN/±Inf → `null` + предупреждение запуска | число |
| `string` | `VARCHAR` | строка | строка; формат колонки `@` |
| `text` | `VARCHAR` | строка | строка ≤ 32 767 символов (длиннее — усечение, §11.4); формат `@` |
| `date` | `DATE` | `YYYY-MM-DD` | серийная дата |
| `timestamp` | `TIMESTAMP` | `YYYY-MM-DD HH:MM:SS[.ffffff]` | серийная дата-время |
| `timestamptz` | `TIMESTAMP`, приведённый к UTC | `YYYY-MM-DDTHH:MM:SS[.ffffff]Z` | серийная дата-время UTC; к заголовку добавляется ` (UTC)` |
| `boolean` | `BOOLEAN` | `true`/`false` | `TRUE`/`FALSE` |
| любой, значение NULL | `NULL` | `null` | пустая ячейка |

### 3.3 Определение типов колонок Oracle-результата
Результат Oracle-запроса приходит **строками** (`all_varchar := true`, §5.1): типы Oracle недоступны, потому что `dbms_xmlgen` отдаёт текст. Тип каждой колонки определяется демоном **по полному результату**, а не по первой странице.

**Пустая строка — это NULL.** Ofquack в режиме `all_varchar` отдаёт NULL пустой строкой (`PHASE0_REPORT.md` §4.2). В Oracle `''` и NULL неразличимы, поэтому для Oracle-результатов демон ДОЛЖЕН заменить `''` на NULL **до** всех шагов ниже — и в выводе типов, и в сохранённых значениях. Для Local-результатов замена не делается: DuckDB различает `''` и NULL.

Для каждой колонки:
1. Если колонка объявлена `@column` — тип из объявления. Если хоть одно ненулевое значение не приводится к нему — запуск падает с `E_TYPE_DECLARED_MISMATCH` (в сообщении: колонка, номер строки, значение).
2. Иначе, если в схеме запроса (`meta.query_schemas`, §6.2) для этого `definition_hash` уже есть тип — он берётся как стартовый и **может только расширяться** (п. 4).
3. Иначе тип выводится по значениям (NULL пропускаются):

   | Условие на все ненулевые значения | Тип |
   |---|---|
   | значений нет (все NULL или 0 строк) | `string` |
   | все — целые без ведущих нулей `^-?(0\|[1-9][0-9]*)$`, и (имя колонки оканчивается на `_ID` без учёта регистра **или** хоть одно значение длиннее 15 цифр) | `id` |
   | все — целые без ведущих нулей, длина ≤ 15 цифр | `integer` |
   | все — числа `^-?(0\|[1-9][0-9]*)?(\.[0-9]+)?$` с хотя бы одной цифрой (в т. ч. `.84`, `-.5`), и `max(цифр целой части) + max(масштаб) ≤ 38` | `decimal(38, max(масштаб))` |
   | то же, но больше 38 цифр | `number` + предупреждение |
   | все — `YYYY-MM-DD` | `date` |
   | все — `TS` и у всех время `00:00:00` с нулевой дробью | `date` |
   | все — `TS` | `timestamp` |
   | все — `TSTZ` | `timestamptz`, значения приводятся к UTC |
   | иначе (в т. ч. смесь `TS` и `TSTZ`) | `string` |

   Здесь `TS` — `^YYYY-MM-DD[ T]HH:MM:SS(\.[0-9]{1,9})?$`, `TSTZ` — то же с суффиксом `±HH:MM` (пробел перед знаком необязателен). Дробь до 9 знаков допускается, потому что Oracle `TIMESTAMP` отдаёт до 9 знаков. Хранится с точностью до микросекунд: знаки с 7-го по 9-й **отбрасываются**, а не округляются.

   Тип `text` автоматически не выводится (только объявлением), `boolean` — тоже.
4. **Расширение типа.** Если стартовый тип (п. 2) не вмещает новые значения, он расширяется по цепочкам: `integer → decimal → number → string`, `id → string`, `date → timestamp → string`, `timestamptz → string`; переход между цепочками — сразу в `string`. Расширение пишется в `meta.query_schemas` и в предупреждения запуска. Сужение не делается никогда.

**Форматы, которые отдаёт ofquack** (подтверждены в фазе 0, `PHASE0_REPORT.md` §4.2):
- числа меньше единицы — без ведущего нуля: `.84`, `-.5`;
- `DATE` — `YYYY-MM-DD`;
- `TIMESTAMP` — `YYYY-MM-DD HH:MM:SS.ffffff…`, 6–9 знаков дроби;
- `TIMESTAMP WITH TIME ZONE` — `YYYY-MM-DD HH:MM:SS.ffffff +HH:MM`;
- бесконечность `BINARY_DOUBLE` — `Inf`; колонка с ней выводится как `string`.

**Oracle `DATE` приходит без времени.** SQL-отчёт отдаёт колонку `DATE` в формате `YYYY-MM-DD`, и время суток теряется: `SYSDATE` → `2026-10-29`. Колонки `TIMESTAMP` время сохраняют; в Fusion это, например, `CREATION_DATE` и `LAST_UPDATE_DATE`. Пока это не исправлено в модели данных отчёта (решение Р2, `PHASE0_REPORT.md` §6):
- автор, которому нужно время из колонки `DATE`, пишет `CAST(col AS TIMESTAMP)`;
- редактор и Preview показывают это правило в подсказке к колонкам типа `date`;
- правило «все `TS` с временем `00:00:00` → `date`» срабатывает, только если отчёт отдаёт `DATE` вместе со временем. Сегодня такие колонки приходят как `YYYY-MM-DD` и попадают в правило `date` строкой выше.

### 3.4 Типы колонок Local-результата

| Тип DuckDB результата | Тип договора |
|---|---|
| `VARCHAR` | `string` |
| `BOOLEAN` | `boolean` |
| `TINYINT`…`BIGINT`, `UTINYINT`…`UBIGINT`, `HUGEINT` | `id`, если имя оканчивается на `_ID` или хоть одно \|значение\| ≥ 10^15; иначе `integer` |
| `DECIMAL(p,s)` | `decimal(p,s)` |
| `FLOAT`, `DOUBLE` | `number` |
| `DATE` | `date` |
| `TIMESTAMP`, `TIMESTAMP_MS`, `TIMESTAMP_NS`, `TIMESTAMP_S` | `timestamp` |
| `TIMESTAMP WITH TIME ZONE` | `timestamptz` |
| прочие (`LIST`, `STRUCT`, `MAP`, `BLOB`, `INTERVAL`, `UUID`, `TIME`…) | `string` через `CAST(… AS VARCHAR)` + предупреждение |

`@column` в Local-запросе переопределяет тип так же, как в п. 1 §3.3.

### 3.5 Векторы типов

| # | Колонка | Значения (Oracle, строки) | Стартовый тип | Результат |
|---|---|---|---|---|
| T1 | `AMOUNT` | `1`, `2`, `300` | — | `integer` |
| T2 | `VENDOR_ID` | `1`, `2`, `300` | — | `id` |
| T3 | `REF` | `1`, `1234567890123456` | — | `id` (16 цифр) |
| T4 | `CODE` | `0012`, `13` | — | `string` |
| T5 | `AMT` | `.84`, `-1.5`, `3` | — | `decimal(38,2)`, значения `0.84`, `-1.5`, `3` |
| T6 | `X` | `1.5`, `abc` | — | `string` |
| T7 | `D` | `2026-01-01`, `2026-02-01` | — | `date` |
| T8 | `D` | `2026-01-01 00:00:00`, `2026-02-01 00:00:00` | — | `date` |
| T9 | `D` | `2026-01-01 10:00:00` | — | `timestamp` |
| T10 | `N` | NULL, NULL | — | `string` |
| T11 | `E` | `1e5` | — | `string` |
| T12 | `AMOUNT` | `1.5` | `integer` | `decimal(38,1)` + предупреждение о расширении |
| T13 | `D` | `n/a` | `date` | `string` + предупреждение |
| T14 | `AMOUNT` | `7` | `decimal(38,2)` | `decimal(38,2)` (сужения нет) |
| T15 | `VENDOR_ID` (`@column VENDOR_ID: integer`) | `12`, `x` | — | `E_TYPE_DECLARED_MISMATCH`, строка 2, значение `x` |
| T16 | `AMOUNT` | `''`, `5`, `''` | — | `integer`; значения NULL, `5`, NULL |
| T17 | `N` | `''`, `''` | — | `string`; значения NULL, NULL |
| T18 | `TS` | `2026-01-01 10:11:12.123456789` | — | `timestamp`; значение `2026-01-01 10:11:12.123456` (отброшено, не округлено) |
| T19 | `CREATION_DATE` | `2024-09-03 13:30:19.001000`, `2024-10-01 11:11:46.171000` | — | `timestamp` |
| T20 | `TZ` | `2026-10-29 14:04:12.418151 +00:00` | — | `timestamptz`; значение `2026-10-29 14:04:12.418151` UTC |
| T21 | `TZ` | `2026-10-29 17:04:12 +03:00` | — | `timestamptz`; значение `2026-10-29 14:04:12` UTC |
| T22 | `TZ` | `2026-10-29 14:04:12 +00:00`, `2026-10-29 14:04:12` | — | `string` (смесь со смещением и без) |
| T23 | `D` | `2026-10-29` (так приходит `SYSDATE`) | — | `date` — время потеряно на стороне отчёта, см. «Oracle `DATE`» выше |
| T24 | `X` | `1.5`, `Inf` | — | `string` |
| T25 | `TS` | `2026-01-01 00:00:00.000000000`, `2026-01-02 00:00:00` | — | `date` |

| # | Тип | Значение в DuckDB | JSON |
|---|---|---|---|
| J1 | `decimal(18,2)` | `1234567890123456.78` | `"1234567890123456.78"` |
| J2 | `decimal(18,2)` | `-0.50` | `"-0.5"` |
| J3 | `id` | `300000012345678901` | `"300000012345678901"` |
| J4 | `integer` | `42` | `42` |
| J5 | `date` | `2026-03-31` | `"2026-03-31"` |
| J6 | `timestamp` | `2026-03-31 23:59:59.120000` | `"2026-03-31 23:59:59.12"` |
| J7 | `number` | `NaN` | `null` + предупреждение |
| J8 | любой | `NULL` | `null` |

---

## 4. Подстановка параметров в Oracle SQL

### 4.1 Токенизатор
Текст тела разбирается слева направо. Вне перечисленных ниже конструкций ищется `:имя`. Внутри — текст копируется без изменений.

| Конструкция | Начало | Конец |
|---|---|---|
| Строковый литерал | `'` (также с префиксом `N`/`n`) | `'`, не являющаяся частью `''` |
| q-литерал | `q'` / `Q'` / `nq'` / `Nq'` / `nQ'` / `NQ'` + символ-разделитель `X` | `X'`, где для `[ ( { <` закрывающий — `] ) } >` |
| Идентификатор в кавычках | `"` | `"`, не являющаяся частью `""` |
| Однострочный комментарий | `--` | конец строки |
| Многострочный комментарий | `/*` | `*/` (вложенность не поддерживается) |

- **Параметр:** `:` + `[A-Za-z][A-Za-z0-9_]*` — берётся максимально длинное имя. Сопоставление с `@param` — без учёта регистра.
- `:` + цифра — `E_PARAM_POSITIONAL`.
- `:` перед символом, не начинающим имя (например `::`, `: `), копируется как есть.
- Имена, начинающиеся с `__`, зарезервированы; в MVP встретить их в тексте — `E_PARAM_UNKNOWN`.
- Незакрытый литерал, идентификатор или комментарий — `E_SQL_TOKENIZE`.
- Найденный `:имя` без `@param` — `E_PARAM_UNKNOWN`. Объявленный, но не встретившийся параметр — предупреждение.
- После комментариев и пробелов тело ДОЛЖНО начинаться с `SELECT` или `WITH` (без учёта регистра), иначе `E_SQL_NOT_SELECT`.

### 4.2 Литералы

| Тип | Подстановка |
|---|---|
| `integer`, `id` | каноническая форма; отрицательное — в скобках: `(-5)` |
| `number`, `decimal` | каноническая форма; отрицательное — в скобках: `(-12.5)` |
| `string`, `enum` | `'…'` с удвоением `'` |
| `date` | `DATE 'YYYY-MM-DD'` |
| `timestamp` | `TIMESTAMP 'YYYY-MM-DD HH:MM:SS[.ffffff]'` |
| `boolean` | `'Y'` / `'N'` |
| `list<T>` | только сразу после ключевого слова `IN` (между ними — только пробелы и комментарии), иначе `E_PARAM_LIST_CONTEXT`. Подстановка — подзапрос (ниже) |

`list<T>` → подзапрос по коллекциям Oracle, порциями не больше 999 элементов:
```
(SELECT column_value FROM TABLE(sys.<тип>(e1, …, e999))
 UNION ALL SELECT column_value FROM TABLE(sys.<тип>(e1000, …)))
```
| T | `<тип>` | элементы |
|---|---|---|
| `integer`, `id`, `number`, `decimal` | `odcinumberlist` | литералы чисел (отрицательные — в скобках) |
| `string` | `odcivarchar2list` | строковые литералы |
| `date`, `timestamp` | `odcidatelist` | `DATE '…'` / `TIMESTAMP '…'` |
| `boolean` | `odcivarchar2list` | `'Y'`/`'N'` |

Пустой список → `(SELECT NULL FROM dual WHERE 1 = 0)`.
Доступность `sys.odci*list` через SQL-отчёт BIP — **[Ф0]**.

### 4.3 Векторы подстановки
`bu: integer`, `n: string`, `d: date`, `ts: timestamp`, `a: decimal(18,2)`, `ids: list<integer>`, `f: boolean`, `bu_2: integer`.

| # | Тело (фрагмент) | Значения | Результат |
|---|---|---|---|
| R1 | `WHERE org_id = :bu` | bu=204 | `WHERE org_id = 204` |
| R2 | `WHERE name = :n` | n=`O'Brien` | `WHERE name = 'O''Brien'` |
| R3 | `WHERE x = ':bu' AND y = :bu` | bu=204 | `WHERE x = ':bu' AND y = 204` |
| R4 | `-- :bu⏎SELECT :bu FROM dual` | bu=204 | `-- :bu⏎SELECT 204 FROM dual` |
| R5 | `/* :bu */ SELECT 1 FROM dual WHERE a = :bu` | bu=204 | `/* :bu */ SELECT 1 FROM dual WHERE a = 204` |
| R6 | `SELECT q'[it's :bu]' FROM dual WHERE a = :bu` | bu=204 | `SELECT q'[it's :bu]' FROM dual WHERE a = 204` |
| R7 | `SELECT nq'{:bu}' FROM dual` | — | без изменений |
| R8 | `SELECT "COL:bu" FROM t WHERE a = :BU` | bu=204 | `SELECT "COL:bu" FROM t WHERE a = 204` |
| R9 | `WHERE d >= :d` | d=`2026-01-01` | `WHERE d >= DATE '2026-01-01'` |
| R10 | `WHERE t <= :ts` | ts=`2026-03-31T23:59:59` | `WHERE t <= TIMESTAMP '2026-03-31 23:59:59'` |
| R11 | `WHERE amt > :a` | a=-12.5 | `WHERE amt > (-12.5)` |
| R12 | `WHERE x - :bu > 0` | bu=-5 | `WHERE x - (-5) > 0` |
| R13 | `WHERE id IN :ids` | ids=[1,2,3] | `WHERE id IN (SELECT column_value FROM TABLE(sys.odcinumberlist(1, 2, 3)))` |
| R14 | `WHERE id IN :ids` | 1 500 элементов | два `TABLE(sys.odcinumberlist(…))` по 999 и 501, через `UNION ALL` |
| R15 | `WHERE id IN :ids` | ids=[] | `WHERE id IN (SELECT NULL FROM dual WHERE 1 = 0)` |
| R16 | `WHERE id = :ids` | ids=[1] | `E_PARAM_LIST_CONTEXT` |
| R17 | `WHERE a = :x` | — | `E_PARAM_UNKNOWN` (`x`) |
| R18 | `WHERE a = :1` | — | `E_PARAM_POSITIONAL` |
| R19 | `WHERE a = :bu` | bu не передан, нет значения по умолчанию | `E_PARAM_MISSING` |
| R20 | `WHERE a = :n` | n содержит NUL | `E_PARAM_TYPE` |
| R21 | `WHERE a = :n` | n длиной 4 001 байт | `E_PARAM_TOO_LONG` |
| R22 | `WHERE a = :bu_2` | bu=1, bu_2=2 | `WHERE a = 2` |
| R23 | `SELECT :bu\|\|'x' FROM dual` | bu=204 | `SELECT 204\|\|'x' FROM dual` |
| R24 | `WHERE a = :bu` | bu=3.5 | `E_PARAM_TYPE` |
| R25 | `WHERE a = :bu` | bu=3.0 (число) | `WHERE a = 3` |
| R26 | `WHERE flag = :f` | f=true | `WHERE flag = 'Y'` |
| R27 | `SELECT 'abc FROM dual` | — | `E_SQL_TOKENIZE` |
| R28 | `UPDATE t SET a = 1` | — | `E_SQL_NOT_SELECT` |
| R29 | `/* c */ WITH t AS (SELECT :bu b FROM dual) SELECT * FROM t` | bu=204 | `/* c */ WITH t AS (SELECT 204 b FROM dual) SELECT * FROM t` |
| R30 | `WHERE a = :x::y` | — | `E_PARAM_UNKNOWN` (`x`); `::y` копируется как есть |

---

## 5. Получение данных

### 5.1 Вызов ofquack (Oracle-запрос)
Демон ДОЛЖЕН выполнять Oracle-запрос ровно так:
```sql
SELECT * FROM oracle_fusion_query(
    $rendered_sql,
    secret        := $connection_name,
    all_varchar   := true,
    on_cast_error := 'error',
    fetch_size    := $fetch_size        -- по умолчанию 10000; для Preview — 200
);
```
- `$rendered_sql` — тело после подстановки (§4), передаётся параметром подготовленного запроса DuckDB, не склейкой.
- Результат читается **потоково** (`PendingQuery` / `Fetch`) по `DataChunk`, строки дописываются через `Appender` в таблицу-заготовку `ds."<stage>"` со всеми колонками `VARCHAR`. Это даёт прогресс (строки), контроль `@max_rows` и отмену.
- При переносе из заготовки в таблицу экземпляра `''` заменяется на NULL (§3.3), затем значения приводятся к типам договора.
- `fetch_size` 10 000 — по замеру фазы 0: примерно на 7 % быстрее 5 000 при ~5 200 строк/с (`PHASE0_REPORT.md` §4.1). Каждый запрос к BI Publisher стоит 1–3,5 с накладных расходов, поэтому меньший размер страницы заметно замедляет загрузку.
- Имена колонок — как вернул отчёт (обычно в верхнем регистре). Сопоставление с `@column` — без учёта регистра.
- Ofquack сам добавляет `ORDER BY` для постраничного чтения. Автору для больших запросов рекомендуется свой `ORDER BY` по индексированному ключу (из документации ofquack); демон это не проверяет.

### 5.2 Пустой результат
Ofquack на запрос без строк бросает `IOException` с текстом, начинающимся с `Oracle Fusion returned no rows`. Демон ДОЛЖЕН:
1. Если все колонки объявлены `@column` **или** в `meta.query_schemas` есть схема для этого `definition_hash` — создать dataset с 0 строк и этой схемой. Запуск — `succeeded`, `rows = 0`.
2. Иначе — запуск `failed` с `E_EMPTY_UNKNOWN_SCHEMA` и подсказкой «Запрос вернул 0 строк, а схема неизвестна. Объявите колонки через @column или выполните запрос с параметрами, при которых есть строки».

Распознавание по тексту — временное, до доработки ofquack (§15).

### 5.3 Local-запрос
- Перед выполнением для каждой `@depends` находится экземпляр dataset'а (§6.3); если его нет или статус не `ready` — `E_DEPENDENCY_MISSING` (в `details` — список недостающих, для Smart Refresh).
- На соединении создаются временные представления: `CREATE TEMP VIEW <query_id> AS SELECT * FROM ds."<таблица экземпляра>"`, для импортов — `CREATE TEMP VIEW <name> AS SELECT * FROM xl_import."<name>"`. После выполнения они удаляются. Разрешение коротких имён во временные представления — **[Ф0]**.
- Тело проходит валидацию (§7), затем выполняется как подготовленный запрос с именованными параметрами `$имя`. Значения — по типам §3.1 (`list<T>` → `LIST`).
- Результат читается потоково и пишется через `Appender` в таблицу экземпляра с типами DuckDB результата; тип договора — по §3.4.

### 5.4 Пределы
- Лимит строк: `@max_rows`, иначе 1 000 000. Для Preview — 200.
- Демон читает до `limit + 1` строки. Если строка `limit + 1` получена — запуск помечается `truncated = true`, сохраняются первые `limit` строк, оставшееся чтение прерывается (`Connection::Interrupt()` на соединении запуска).
- Тайм-аут запуска: 30 минут (`ofexcel_run_timeout`), по истечении — прерывание и `E_TIMEOUT`.

---

## 6. Хранилище

### 6.1 Имена таблиц
- Экземпляр dataset'а: `ds."<query_id>__<первые 12 символов instance_key>__v<N>"`, `N` — номер версии экземпляра (1, 2, …).
- Заготовка Oracle-запуска: `ds."stage__<run_id без дефисов>"`; удаляется по завершении запуска в любом исходе.
- Импорт: `xl_import."<name>"`.

### 6.2 DDL метаданных
Миграции применяются при старте по `meta.schema_version`. Для MVP — версия 1:

```sql
CREATE SCHEMA IF NOT EXISTS meta;
CREATE SCHEMA IF NOT EXISTS ds;
CREATE SCHEMA IF NOT EXISTS xl_import;

CREATE TABLE meta.schema_version (version INTEGER NOT NULL);

CREATE TABLE meta.state (
    key   VARCHAR PRIMARY KEY,        -- 'data_version', 'catalog_version'
    value VARCHAR NOT NULL
);

CREATE TABLE meta.connections (
    name        VARCHAR PRIMARY KEY,
    environment VARCHAR NOT NULL CHECK (environment IN ('DEV', 'TEST', 'PROD')),
    endpoint    VARCHAR NOT NULL,     -- https://host, без пути
    report_path VARCHAR NOT NULL,
    created_at  TIMESTAMP NOT NULL
);

CREATE TABLE meta.query_definitions (
    definition_hash VARCHAR NOT NULL,
    query_id        VARCHAR NOT NULL,
    version         INTEGER NOT NULL,
    engine          VARCHAR NOT NULL CHECK (engine IN ('oracle', 'local')),
    source_kind     VARCHAR NOT NULL CHECK (source_kind IN ('project', 'workbook', 'example')),
    source_ref      VARCHAR NOT NULL, -- имя проекта, workbook_id или 'builtin'
    header_json     VARCHAR NOT NULL, -- разобранный заголовок (§2), канонический JSON
    text            VARCHAR NOT NULL, -- исходный текст файла
    registered_at   TIMESTAMP NOT NULL,
    PRIMARY KEY (source_kind, source_ref, query_id, definition_hash)
);

CREATE TABLE meta.query_schemas (
    definition_hash VARCHAR NOT NULL,
    ordinal         INTEGER NOT NULL, -- с 0
    column_name     VARCHAR NOT NULL,
    contract_type   VARCHAR NOT NULL, -- 'id', 'integer', 'decimal(38,2)', …
    header          VARCHAR,          -- заголовок из @column или NULL
    PRIMARY KEY (definition_hash, ordinal)
);

CREATE TABLE meta.dataset_instances (
    instance_key    VARCHAR PRIMARY KEY,
    query_id        VARCHAR NOT NULL,
    definition_hash VARCHAR NOT NULL,
    connection_name VARCHAR NOT NULL, -- 'local' для Local-запросов
    params_json     VARCHAR NOT NULL, -- canonical_params
    current_table   VARCHAR,          -- NULL, пока нет успешной версии
    current_version INTEGER NOT NULL DEFAULT 0,
    prev_table      VARCHAR,
    data_version    BIGINT  NOT NULL DEFAULT 0,
    fetched_at      TIMESTAMP,
    row_count       BIGINT,
    truncated       BOOLEAN,
    status          VARCHAR NOT NULL CHECK (status IN ('empty', 'ready')),
    last_error_code VARCHAR,
    last_error      VARCHAR,
    last_access_at  TIMESTAMP NOT NULL
);

CREATE TABLE meta.runs (
    run_id          VARCHAR PRIMARY KEY,
    instance_key    VARCHAR NOT NULL,
    query_id        VARCHAR NOT NULL,
    definition_hash VARCHAR NOT NULL,
    connection_name VARCHAR NOT NULL,
    environment     VARCHAR,          -- NULL для local
    params_json     VARCHAR NOT NULL,
    workbook_id     VARCHAR,
    parent_run_id   VARCHAR,          -- для зависимостей
    user_name       VARCHAR,          -- пользователь Fusion; NULL для local
    status          VARCHAR NOT NULL CHECK (status IN ('queued', 'running', 'succeeded', 'failed', 'cancelled')),
    started_at      TIMESTAMP NOT NULL,
    finished_at     TIMESTAMP,
    rows_fetched    BIGINT NOT NULL DEFAULT 0,
    truncated       BOOLEAN NOT NULL DEFAULT false,
    result_table    VARCHAR,          -- версия, созданная этим запуском
    warnings_json   VARCHAR NOT NULL DEFAULT '[]',
    error_code      VARCHAR,
    error_message   VARCHAR
);

CREATE TABLE meta.trusted_definitions (
    definition_hash VARCHAR PRIMARY KEY,
    query_id        VARCHAR NOT NULL,
    origin          VARCHAR NOT NULL CHECK (origin IN ('local_editor', 'user_consent', 'example')),
    workbook_id     VARCHAR,
    trusted_at      TIMESTAMP NOT NULL
);

CREATE TABLE meta.missing (
    workbook_id     VARCHAR NOT NULL,
    instance_key    VARCHAR NOT NULL,
    query_id        VARCHAR NOT NULL,
    definition_hash VARCHAR NOT NULL,
    connection_name VARCHAR NOT NULL,
    params_json     VARCHAR NOT NULL,
    first_seen      TIMESTAMP NOT NULL,
    last_seen       TIMESTAMP NOT NULL,
    requests        BIGINT NOT NULL,
    PRIMARY KEY (workbook_id, instance_key)
);

CREATE TABLE meta.imports (
    name        VARCHAR PRIMARY KEY,
    workbook_id VARCHAR NOT NULL,
    source      VARCHAR NOT NULL,     -- 'Sheet1!tblBank' или 'Sheet1!A1:F200'
    schema_json VARCHAR NOT NULL,     -- [{"name":…,"type":…}]
    row_count   BIGINT NOT NULL,
    imported_at TIMESTAMP NOT NULL,
    data_version BIGINT NOT NULL
);

INSERT INTO meta.schema_version VALUES (1);
INSERT INTO meta.state VALUES ('data_version', '0'), ('catalog_version', '');
```

### 6.3 Выбор экземпляра
Для запроса `Q` с определением `H`, подключением `C` и параметрами `P`: `instance_key` по §1.3. Если строки в `meta.dataset_instances` нет — создаётся со `status = 'empty'`.

### 6.4 Подмена версии (успешный запуск)
В одной транзакции:
```sql
-- было: current_table = T_N, prev_table = T_{N-1}
DROP TABLE IF EXISTS ds."<T_{N-1}>";
UPDATE meta.dataset_instances
   SET prev_table = current_table, current_table = '<T_{N+1}>',
       current_version = current_version + 1, data_version = data_version + 1,
       fetched_at = now(), row_count = …, truncated = …, status = 'ready',
       last_error_code = NULL, last_error = NULL
 WHERE instance_key = …;
UPDATE meta.state SET value = CAST(CAST(value AS BIGINT) + 1 AS VARCHAR) WHERE key = 'data_version';
```
- Неудачный или отменённый запуск: удалить свою таблицу `T_{N+1}` (если создана), записать `last_error_code`/`last_error` в экземпляр, `current_table` не трогать.
- Одновременно для одного `instance_key` выполняется не больше одного запуска. Повторный `POST /v1/runs` для того же ключа возвращает уже идущий `run_id` (если не передан `force: true`; с `force` — `E_CONFLICT`, пока идущий не закончится).
- Экземпляры, у которых `last_access_at` старше 30 дней (`ofexcel_dataset_ttl_days`), удаляются вместе с таблицами при старте демона.

---

## 7. Валидация Local SQL

Разбор — `duckdb::Parser`, обход AST. Нарушение любого правила — `E_VALIDATION` с номером правила и фрагментом.

| # | Правило |
|---|---|
| V1 | Ровно одна инструкция |
| V2 | Тип инструкции — `SELECT` (включая `WITH`, `UNION`, `EXCEPT`, `INTERSECT`, `VALUES` внутри `SELECT`) |
| V3 | Каждая ссылка на таблицу (`BaseTableRef`) без схемы — имя CTE этого запроса, имя `@depends`-запроса или имя `@depends`-импорта (без схемы) |
| V4 | Ссылка со схемой разрешена только `xl_import.<name>`, и только если `xl_import.<name>` объявлен в `@depends`. Указание каталога (`db.schema.table`) запрещено |
| V5 | Имя таблицы, похожее на путь или URL (содержит `.`, `/`, `\`, `:` или начинается с `'`), запрещено — защита от replacement scan (`FROM 'file.csv'`) |
| V6 | Табличные функции разрешены только: `unnest`, `range`, `generate_series` |
| V7 | Запрещены скалярные функции: `getenv`, `current_setting`, `ofexcel_*`, `fusion_scanner_*`, `oracle_fusion_*`, `read_text`, `read_blob` |
| V8 | Каждый `$имя` объявлен в `@param`; позиционные `?` и `$1` запрещены |
| V9 | Подзапросы и CTE проверяются по тем же правилам рекурсивно |

Векторы (запрос объявляет `@depends: ap_invoices(...)` и `@depends: xl_import.bank`, `@param bu: integer`):

| # | SQL | Результат |
|---|---|---|
| L1 | `SELECT * FROM ap_invoices` | OK |
| L2 | `SELECT * FROM ap_invoices i JOIN xl_import.bank b ON b.ref = i.invoice_num` | OK |
| L3 | `WITH t AS (SELECT * FROM ap_invoices) SELECT count(*) FROM t` | OK |
| L4 | `SELECT * FROM ap_invoices; SELECT 1` | V1 |
| L5 | `CREATE TABLE x AS SELECT 1` | V2 |
| L6 | `SELECT * FROM gl_balances` | V3 (не объявлено) |
| L7 | `SELECT * FROM xl_import.other` | V4 |
| L8 | `SELECT * FROM 'data.csv'` | V5 |
| L9 | `SELECT * FROM read_csv('x.csv')` | V6 |
| L10 | `SELECT * FROM oracle_fusion_query('SELECT 1 FROM dual')` | V6 |
| L11 | `SELECT getenv('HOME')` | V7 |
| L12 | `SELECT * FROM ap_invoices WHERE org_id = $bu` | OK |
| L13 | `SELECT * FROM ap_invoices WHERE org_id = $x` | V8 |
| L14 | `SELECT * FROM ap_invoices WHERE org_id = ?` | V8 |
| L15 | `SELECT * FROM range(10)` | OK |
| L16 | `SELECT * FROM (SELECT * FROM read_parquet('x'))` | V6 (через V9) |
| L17 | `SELECT * FROM memory.main.t` | V4 |

Валидатор — дополнительная защита, а не песочница (`SPEC.md` §8.4). Независимо от него демон работает с `enable_external_access = false`.

---

## 8. HTTP API

### 8.1 Общее
- База: hosted — `http://127.0.0.1:<port>`, local — `https://localhost:<port>`. Порт по умолчанию `47321`.
- Тела запросов и ответов — `application/json; charset=utf-8`.
- **Проверки на каждом запросе** (до разбора тела):
  1. `Host` ∈ {`127.0.0.1:<port>`, `localhost:<port>`}, иначе 403 `E_HOST`.
  2. Для `/v1/*`: hosted — `Origin` **обязателен** и в точности равен `ofexcel_hosted_origin` (после нормализации: нижний регистр, `https://host[:port]`, без пути и завершающей `/`); local — `Origin` **может отсутствовать** (браузер не отправляет его на GET того же источника), но если есть — равен `https://localhost:<port>` или `https://127.0.0.1:<port>`. Иначе 403 `E_ORIGIN` **без** CORS-заголовков.
  3. local: заголовок `X-OF-Session` равен сессионному токену (сравнение за постоянное время), иначе 401 `E_SESSION`. В local именно токен, а не `Origin`, защищает запросы без `Origin`.
  4. Preflight `OPTIONS` проверяет `Host` и `Origin` (тот обязателен в обоих профилях), но не `X-OF-Session`: браузер не отправляет пользовательские заголовки в preflight.
- **CORS (hosted):** на `OPTIONS` для разрешённого `Origin` — `204` с заголовками:
  ```
  Access-Control-Allow-Origin: <ofexcel_hosted_origin>
  Access-Control-Allow-Methods: GET, POST, PUT, DELETE
  Access-Control-Allow-Headers: Content-Type, X-OF-Workbook, X-OF-Session
  Access-Control-Allow-Private-Network: true
  Access-Control-Max-Age: 600
  Vary: Origin
  ```
  На остальные ответы для разрешённого `Origin` — `Access-Control-Allow-Origin` и `Vary: Origin`.
- **Каждый ответ** `/v1/*` содержит заголовки `X-OF-Data-Version` и `X-OF-Catalog-Version`.
- **Ошибка:** HTTP-статус из §14 и тело
  ```ts
  type ErrorBody = { error: { code: string; message: string; details?: unknown } };
  ```
  `message` — на русском, для показа пользователю.

### 8.1.1 Векторы проверок
Порт 47321. hosted: `ofexcel_hosted_origin = https://app.example.com`. local: токен `s3cr3t`. Реализованы в `test/cpp/guard_test.cpp` репозитория `ofexcel`.

| # | Профиль, запрос | Host | Origin | X-OF-Session | Результат |
|---|---|---|---|---|---|
| G1 | hosted | `127.0.0.1:47321` | `https://app.example.com` | — | ALLOW |
| G2 | hosted | `localhost:47321` | `https://app.example.com` | — | ALLOW |
| G3 | hosted | `LOCALHOST:47321` | `https://app.example.com` | — | ALLOW |
| G4 | hosted | `127.0.0.1:47322` | `https://app.example.com` | — | `E_HOST` |
| G5 | hosted | `evil.example.com` | `https://app.example.com` | — | `E_HOST` |
| G6 | hosted | (пусто) | `https://app.example.com` | — | `E_HOST` |
| G7 | hosted | `127.0.0.1:47321` | `https://evil.example.com` | — | `E_ORIGIN` |
| G8 | hosted | `127.0.0.1:47321` | (нет) | — | `E_ORIGIN` |
| G9 | hosted | `127.0.0.1:47321` | `https://app.example.com/` | — | `E_ORIGIN` |
| G10 | hosted | `127.0.0.1:47321` | `null` | — | `E_ORIGIN` |
| G11 | hosted | `127.0.0.1:47321` | `http://app.example.com` | — | `E_ORIGIN` |
| G12 | local | `localhost:47321` | (нет) | `s3cr3t` | ALLOW |
| G13 | local | `localhost:47321` | `https://localhost:47321` | `s3cr3t` | ALLOW |
| G14 | local | `localhost:47321` | `https://app.example.com` | `s3cr3t` | `E_ORIGIN` |
| G15 | local | `localhost:47321` | (нет) | (нет) | `E_SESSION` |
| G16 | local | `localhost:47321` | (нет) | `guess` | `E_SESSION` |
| G17 | hosted, `OPTIONS` | `127.0.0.1:47321` | `https://app.example.com` | — | ALLOW |
| G18 | hosted, `OPTIONS` | `127.0.0.1:47321` | `https://evil.example.com` | — | `E_ORIGIN` |
| G19 | hosted, `OPTIONS` | `127.0.0.1:47321` | (нет) | — | `E_ORIGIN` |

Нормализация `ofexcel_hosted_origin`: `https://APP.Example.com:8443` → `https://app.example.com:8443`; недопустимы (`""`, запуск сервера отказывает с `E_BAD_REQUEST`): `http://…`, завершающая `/`, путь, `user@`, порт > 65535, пустой хост, хост с точкой в начале.

### 8.2 Общие типы
```ts
type Environment = "DEV" | "TEST" | "PROD";
type ParamValue = string | number | boolean | null | Array<string | number | boolean>;
type ParamDef = { name: string; type: string; default?: string; description?: string };
type ColumnInfo = { name: string; header: string; type: string };   // type — тип договора (§3.2)
type Warning = { code: string; message: string };
type QueryRef = { id: string; hash: string };
type QuerySummary = {
  id: string; hash: string; version: number; engine: "oracle" | "local";
  title?: string; description?: string; params: ParamDef[]; columns: ColumnInfo[];
  source: { kind: "project" | "workbook" | "example"; ref: string };
  trusted: boolean;
};
type Run = {
  run_id: string; instance_key: string; query: QueryRef; connection: string;
  environment?: Environment; params: Record<string, ParamValue>;
  status: "queued" | "running" | "succeeded" | "failed" | "cancelled";
  started_at: string; finished_at?: string; rows_fetched: number; truncated: boolean;
  parent_run_id?: string; depends_on: string[];   // run_id зависимостей этого запуска
  warnings: Warning[]; error?: { code: string; message: string };
};
type RowsPage = {
  columns: ColumnInfo[]; rows: unknown[][];       // значения по §3.2
  offset: number; limit: number; total: number; truncated: boolean;
};
```

### 8.3 Маршруты

**`GET /v1/health`** → 200
```ts
{ product: "qexcel"; version: string; api: { version: "1.0"; min_client: "1.0" };
  profile: "hosted" | "local"; data_version: number; catalog_version: string; time: string }
```

**`GET /v1/status`** → 200
```ts
{ connections: Array<{ name: string; environment: Environment; host: string;
                       signed_in: boolean; user?: string }>;
  active_runs: number; warnings: Warning[] }
```

**`GET /v1/connections`** → `{ connections: Array<{ name; environment; endpoint; report_path }> }`

**`POST /v1/connections`** `{ name; environment; endpoint; report_path }` → 201 с тем же объектом. Создаёт строку `meta.connections` и секрет `CREATE SECRET <name> (TYPE oracle_fusion, PROVIDER browser, ENDPOINT …, REPORT_PATH …)`. Имя занято — 409 `E_CONFLICT`. При старте демона секреты пересоздаются из `meta.connections`; подключения из `init.sql` имеют приоритет при совпадении имён.

**`POST /v1/connections/{name}/test`** → `{ ok: boolean; user?: string; error?: {code; message} }` — выполняет `SELECT 1 FROM dual` через §5.1.

**`POST /v1/auth/login`** `{ connection: string }` → 200 `{ user: string }`. Вызывает `fusion_scanner_sso_login()` для секрета подключения; ответ — после завершения входа или по тайм-ауту 300 с (`E_FUSION_AUTH`). Клиент ДОЛЖЕН ждать ответа не меньше 310 с.

**`POST /v1/auth/logout`** `{ connection }` → 204.

**`GET /v1/dictionary/tables?connection=&q=&limit=50`** → `{ tables: Array<{ name: string; description?: string }> }` — через `oracle_fusion_tables()`, фильтр по подстроке имени без учёта регистра.

**`GET /v1/dictionary/columns?connection=&table=`** → `{ columns: Array<{ name; type?; description? }> }` — через `oracle_fusion_columns(table)`.

**`GET /v1/queries?workbook=<id>`** → `{ queries: QuerySummary[] }` — проекты, примеры и (если передан `workbook`) запросы этой книги.

**`GET /v1/queries/{id}?source_kind=&source_ref=`** → `QuerySummary & { text: string }`.

**`PUT /v1/queries/{id}`** `{ project: string; text: string }` → 200 `QuerySummary`. Пишет файл `~/QExcel/projects/<project>/queries/<id>.sql`, регистрирует определение, добавляет хеш в `meta.trusted_definitions` с `origin = 'local_editor'`. `@id` в тексте ≠ `{id}` — 422 `E_QUERY_PARSE`.

**`PUT /v1/workbooks/{workbook_id}/queries`** `{ queries: Array<{ text: string }> }` → `{ queries: QuerySummary[] }`. Заменяет набор запросов книги в `meta.query_definitions` (`source_kind = 'workbook'`). Поле `trusted` показывает, есть ли хеш в `meta.trusted_definitions`. Хеши не доверяются автоматически.

**`POST /v1/queries/validate`** `{ text: string }` → 200
```ts
{ ok: boolean; header?: QuerySummary; params_in_text: string[];
  errors: Array<{ code: string; message: string; line?: number }>; warnings: Warning[] }
```

**`POST /v1/preview`**
```ts
{ text?: string; query?: QueryRef; connection?: string;
  params: Record<string, ParamValue>; limit?: number /* ≤ 200 */ }
```
→ 200 `{ columns: ColumnInfo[]; rows: unknown[][]; elapsed_ms: number; truncated: boolean;
          rendered_sql?: string /* только oracle */; warnings: Warning[] }`.
- Ровно одно из `text` / `query`. Текст из `text` — это работа автора: не требует доверия, но проходит разбор (§2) и, для local, валидацию (§7).
- `query` из книги без доверия — 403 `E_NOT_TRUSTED`.
- Preview не создаёт экземпляр dataset'а и не пишет в `meta.runs`. Типы — по §3.3 на строках Preview (без записи в `meta.query_schemas`).

**`POST /v1/runs`**
```ts
{ query: QueryRef; connection?: string; params: Record<string, ParamValue>;
  workbook_id?: string; deps?: "missing" | "all" | "none" /* по умолчанию "missing" */; force?: boolean }
```
→ 202 `Run`.
- Недоверенное определение — 403 `E_NOT_TRUSTED`.
- Для local: зависимости запускаются раньше (`deps = all` — все; `missing` — только без `ready`-экземпляра; `none` — не запускаются, а отсутствие даёт `E_DEPENDENCY_MISSING`). Запуски зависимостей получают `parent_run_id`. Ошибка любой зависимости — родительский запуск `failed` с `E_DEPENDENCY_FAILED`.
- `connection` для oracle: из тела, иначе из `@connection`, иначе 422 `E_BAD_REQUEST`.

**`GET /v1/runs/{run_id}`** → `Run`.

**`POST /v1/runs/{run_id}/cancel`** → `Run`. Для завершённого запуска — без изменений. Отмена ДОЛЖНА переводить запуск в `cancelled` не позже чем через 5 с **[Ф0]**.

**`GET /v1/runs/{run_id}/rows?offset=0&limit=5000`** → `RowsPage`. Только для `succeeded`, иначе 409 `E_RUN_NOT_FINISHED`. `limit` ≤ 20 000. Читает `result_table` запуска; если она уже вытеснена новой версией — 410 `E_GONE`.

**`GET /v1/runs?query=<id>&workbook=<id>&limit=50`** → `{ runs: Run[] }`, новые первыми.

**`GET /v1/datasets`** → `{ datasets: Array<{ instance_key; query: QueryRef; connection; params; status; fetched_at?; row_count?; truncated?; last_error? }> }`.

**`GET /v1/datasets/{instance_key}/rows?offset=&limit=`** → `RowsPage` по `current_table`; `status = 'empty'` — 409 `E_DEPENDENCY_MISSING`.

**`DELETE /v1/datasets/{instance_key}`** → 204.

**`POST /v1/imports`** `{ name: string; workbook_id: string; source: string; columns: Array<{ name: string; type: string }> }` → 201 `{ import_id: string }`. Типы колонок — из §3.2, кроме `timestamptz`.
**`POST /v1/imports/{import_id}/rows`** `{ rows: unknown[][] }` → 204. Значения — в кодировке §3.2 (даты — строками).
**`POST /v1/imports/{import_id}/commit`** → 200 `{ name; rows: number }`. Атомарно заменяет `xl_import."<name>"`, увеличивает `data_version`. Незакоммиченный импорт удаляется через 10 минут.

**`POST /v1/trust`** `{ workbook_id: string; hashes: string[] }` → `{ trusted: string[] }`. Пишет `origin = 'user_consent'`. Хеш, которого нет среди запросов книги, — 422 `E_BAD_REQUEST`.

**`POST /v1/batch`** — §10.3.

**`GET /v1/catalog`** → `{ catalog_version: string; functions: Array<{ id; name; description; params: ParamDef[] }> }`. В MVP — 4 функции ядра (§10.1).

**`GET /v1/missing?workbook=<id>`** → `{ missing: Array<{ query: QueryRef; connection: string; params: Record<string, ParamValue>; first_seen; last_seen; requests }> }`. Записи старше 7 дней удаляются; запись удаляется после успешного запуска этого `instance_key`.

**`GET /addin/*`** (только local) — вшитые файлы; `functions.json` — по §10.1.

---

## 9. Книга Excel

### 9.1 Идентификатор книги
`Office.context.document.settings` ключ `qexcel.workbook_id` — UUID v4. Создаётся при первом сохранении запроса или привязки в книге (`settings.saveAsync`).

### 9.2 Хранение в книге
Один `CustomXmlPart` в пространстве имён `urn:qexcel:workbook:v1`:
```xml
<qexcel xmlns="urn:qexcel:workbook:v1" schema="1"><payload><![CDATA[ … JSON … ]]></payload></qexcel>
```
- JSON внутри CDATA: последовательность `]]>` ДОЛЖНА записываться как `]]>` (она может встретиться только внутри JSON-строк, поэтому это корректно).
- При сохранении старая часть удаляется и создаётся новая (`CustomXmlParts.getByNamespaceAsync` → `deleteAsync` → `addAsync`).
- **Секретов, токенов и паролей в книге нет.**

```ts
type WorkbookPayload = {
  schema: 1;
  workbook_id: string;
  queries: Array<{ id: string; hash: string; text: string;
                   saved_by?: string /* пользователь Fusion */; saved_at: string }>;
  bindings: Binding[];
  imports: Array<{ name: string; sheet: string; table?: string; range?: string;
                   reimport_before_run: boolean;
                   columns: Array<{ name: string; type: string }> }>;
};

type Binding = {
  binding_id: string;                      // UUID v4
  target: { kind: "table"; sheet: string; table: string };
  query: QueryRef;                         // hash ДОЛЖЕН быть среди queries[] книги или в проекте
  connection?: { name: string; host: string };   // для oracle
  params: Record<string, { ref: string } | { value: ParamValue }>;  // ref — "Лист!B2" или имя
  schema_policy: "strict" | "append";
  query_columns: number;                   // сколько первых колонок таблицы принадлежат запросу
  last_success?: { at: string; rows: number; truncated: boolean; run_id: string };
  partial?: { at: string; run_id: string }; // запись прервалась (§11.3)
};
```

### 9.3 Открытие книги и доверие
1. Прочитать `WorkbookPayload`. Если части нет — книга без запросов.
2. `PUT /v1/workbooks/{workbook_id}/queries` с текстами.
3. Если есть `trusted: false` — показать диалог подтверждения до любых других действий с этими запросами. Диалог ДОЛЖЕН показать для каждого запроса: `id`, `title`, `saved_by`, `saved_at`, подключение и окружение из привязок, **полный текст SQL**, список таблиц (лексемы после `FROM`/`JOIN` вне литералов и комментариев — для показа, не для безопасности) и предупреждение:
   > Запрос выполнится под вашей учётной записью Fusion. Если вы сохраните и отправите книгу, полученные данные уйдут вместе с ней.
4. «Доверять» → `POST /v1/trust`. «Не доверять» → запросы и привязки показываются с пометкой «не подтверждён», запуск недоступен, формулы по ним возвращают `#N/A` «Запрос не подтверждён».
5. Если `connection.host` привязки не совпадает с хостом подключения с тем же именем у пользователя — предупреждение с обоими хостами; запуск — только после явного выбора подключения пользователем.

---

## 10. Формулы

### 10.1 `functions.json` ядра
```json
{
  "functions": [
    { "id": "TABLE", "name": "TABLE",
      "description": "Результат запроса QExcel как массив; первая строка — заголовки",
      "parameters": [
        { "name": "query", "type": "string", "dimensionality": "scalar" },
        { "name": "args", "type": "any", "dimensionality": "matrix", "repeating": true, "optional": true } ],
      "result": { "type": "any", "dimensionality": "matrix" } },
    { "id": "CALL", "name": "CALL",
      "description": "Скалярное значение запроса QExcel (результат 1 × 1)",
      "parameters": [
        { "name": "query", "type": "string", "dimensionality": "scalar" },
        { "name": "args", "type": "any", "dimensionality": "matrix", "repeating": true, "optional": true } ],
      "result": { "type": "any", "dimensionality": "scalar" } },
    { "id": "ASOF", "name": "ASOF",
      "description": "Время последнего успешного получения данных запроса",
      "parameters": [
        { "name": "query", "type": "string", "dimensionality": "scalar", "optional": true },
        { "name": "args", "type": "any", "dimensionality": "matrix", "repeating": true, "optional": true } ],
      "result": { "type": "number", "dimensionality": "scalar" } },
    { "id": "STATUS", "name": "STATUS",
      "description": "Состояние QExcel: OK, NOT_SIGNED_IN, STALE, OFFLINE",
      "parameters": [],
      "result": { "type": "string", "dimensionality": "scalar" } }
  ]
}
```

### 10.2 Семантика
- **Разрешение запроса по имени** `query`: запросы книги (по `workbook_id`) → проекты → примеры; внутри источника — по `@id` без учёта регистра. Если в одном источнике несколько определений с одним `@id` — берётся с наибольшим `@version`. Не найдено — `#NAME?` (`invalidName`).
- **Аргументы** — позиционно в порядке `@param`. Аргумент-матрица для скалярного типа ДОЛЖЕН быть 1 × 1, иначе `#VALUE!`. Для `list<T>` — матрица разворачивается по строкам, пустые ячейки пропускаются. Пустая ячейка или пропущенный аргумент → значение по умолчанию или `E_PARAM_MISSING` (`#VALUE!`). Как Excel передаёт пустые ячейки (`null` или `""`) — **[Ф0]**; обе формы трактуются как пусто.
- **Подключение** для oracle-запроса в формуле: подключение по умолчанию книги (`settings` ключ `qexcel.default_connection`), иначе `@connection`, иначе `#N/A` «Не выбрано подключение».
- **`OF.TABLE`** читает `current_table` экземпляра (§6.3); **никогда** не запускает запрос. Нет данных — `#N/A` и запись в `meta.missing`. Строк больше 10 000 (`ofexcel_spill_max_rows`) — `#N/A` «Результат большой: выведите в таблицу». Результат: первая строка — заголовки (`header`), далее значения по §3.2 для Excel (даты — серийные числа, `decimal` — числа, `id` — строки).
- **`OF.CALL`** — то же, но результат ДОЛЖЕН быть ровно 1 строка × 1 колонка, иначе `#VALUE!` «Ожидался результат 1 × 1, получено R × C». `NULL` → `#N/A`.
- **`OF.ASOF(query, args…)`** — `fetched_at` экземпляра в локальном времени машины как серийное число Excel; нет данных — `#N/A`. **`OF.ASOF()`** без аргументов — минимум `last_success.at` по всем привязкам книги; вычисляется в shared runtime без обращения к демону.
- **`OF.STATUS()`** — `OFFLINE` (демон недоступен), `NOT_SIGNED_IN` (подключение по умолчанию без входа), `STALE` (у какой-либо привязки книги последняя попытка обновления неудачна), иначе `OK`.
- **Local-запрос в формуле** выполняется локально, если его экземпляра нет, но все зависимости `ready` (без сети). Если зависимости не готовы — `#N/A` и запись в `meta.missing` по каждой недостающей.

### 10.3 `/v1/batch`
```ts
// запрос
{ calls: Array<{ i: number; kind: "table" | "call" | "asof"; query: string; args: unknown[][][] }> }
// заголовок X-OF-Workbook: <workbook_id>

// ответ
{ data_version: number; catalog_version: string;
  results: Array<{ i: number; v?: unknown /* scalar или unknown[][] */;
                   e?: { code: "invalidValue" | "invalidName" | "notAvailable"; msg: string } }> }
```
- `args` — массив аргументов, каждый — матрица (как передаёт Excel).
- Порядок обработки вызова: разрешение запроса → проверка доверия → нормализация аргументов (§3.1) → `instance_key` → чтение или локальное выполнение.
- **Кэш результата** (сервер и клиент) по ключу `(kind, query_id, definition_hash, connection_name, canonical_params, data_version экземпляра и всех его зависимостей, user_name)`. Клиент сбрасывает кэш целиком при смене `data_version` или `catalog_version` в ответе или в `/v1/health` (опрос каждые 30 с и при возврате фокуса).
- **Деление при ошибках** применяется только к локальному выполнению, при `E_SQL_DATA` (§14). Бюджет — не больше 2 × число групп дополнительных выполнений и не больше 2 с на пакет.

---

## 11. Запись в Excel Table

### 11.1 Создание таблицы
1. Колонки: `ColumnInfo[]` запуска. Заголовок — `header`, иначе имя колонки. Пустой — `Column_<n>`. Совпадения без учёта регистра — суффиксы `_2`, `_3`… Длина заголовка — до 255 символов.
2. Перед записью значений — формат колонок:

   | Тип | `numberFormat` |
   |---|---|
   | `id`, `string`, `text` | `@` |
   | `date` | `yyyy-mm-dd` |
   | `timestamp`, `timestamptz` | `yyyy-mm-dd hh:mm:ss` |
   | `decimal(p,s)` | `#,##0` при `s = 0`, иначе `#,##0.` + `s` нулей |
   | `integer` | `0` |
   | `number`, `boolean` | `General` |

3. Таблица создаётся `worksheet.tables.add(range, true)` и получает имя `tbl<QueryId в PascalCase>` с суффиксом при занятости.

### 11.2 Обновление таблицы
1. Дождаться `succeeded`. До этого лист не трогать.
2. Сравнить схему: имена первых `query_columns` колонок таблицы (без учёта регистра) с заголовками нового результата.
   - `strict`: любое отличие в составе или порядке → ничего не писать, ошибка `E_SCHEMA_CHANGED` (на стороне панели) с разницей и кнопкой «Применить новую схему».
   - `append`: новые колонки вставляются после колонки `query_columns` (перед пользовательскими), `query_columns` увеличивается; пропавшая колонка → `E_SCHEMA_CHANGED`.
3. Форматы колонок `id`, `string`, `text` ДОЛЖНЫ выставляться в `@` при **каждом** обновлении (иначе Excel превратит `0012` в число). Остальные форматы выставляются только при создании таблицы и для новых колонок; пользовательское форматирование не перезаписывается.
4. Размер: `Table.resize` до `1 + rows` строк (заголовок + данные), если доступен ExcelApi 1.13; иначе удаление лишних строк (`table.rows.getItemAt(i).delete()` порциями с конца) и добавление недостающих (`table.rows.add(null, values)`). При 0 строк таблица сохраняет заголовок и одну пустую строку.
5. Значения пишутся порциями по `ofexcel_write_chunk_rows` (по умолчанию 5 000 строк, но не больше ~4 МБ JSON на порцию) **[Ф0]** только в первые `query_columns` колонок. Перед каждой порцией — `context.application.suspendApiCalculationUntilNextSync()`.
6. Пользовательские колонки справа от `query_columns`, вычисляемые колонки, стиль таблицы, ссылки формул на таблицу ДОЛЖНЫ сохраняться.
7. После записи — `last_success` в привязке, сохранение `WorkbookPayload`, удаление `partial`.

### 11.3 Ошибки записи
- Ошибка Office.js посередине записи → `partial = { at, run_id }`, баннер «Таблица обновлена частично» и кнопка «Повторить запись» (повтор читает тот же `run_id`; если он вытеснен — `E_GONE` и предложение запустить заново).
- Ошибка запуска → таблица не меняется, баннер с ошибкой и временем `last_success`.

### 11.4 Значения
| Тип | Значение в `range.values` |
|---|---|
| `id`, `string` | строка |
| `text` | строка; длиннее 32 767 символов — первые 32 767, в предупреждения запуска на стороне панели «усечено N значений в колонке X» |
| `integer`, `number` | число |
| `decimal` | `Number(строка)` |
| `date` | серийное число: дни от 1899-12-30 (система 1900) или от 1904-01-01 (система 1904) |
| `timestamp`, `timestamptz` | серийное число + доля суток |
| `boolean` | `true` / `false` |
| `null` | `""` |

**Защита от формул в данных.** Строка из результата, начинающаяся с `=`, `+`, `-` или `@`, НЕ ДОЛЖНА стать формулой. Колонки строковых типов имеют формат `@` (п. 11.2.3), и при нём значение сохраняется как текст **[Ф0]**. Если фаза 0 покажет, что формат `@` не защищает при записи через `range.values`, такие значения пишутся с ведущим апострофом.

---

## 12. Импорт таблицы листа
1. Источник — Excel Table (по имени) или диапазон с первой строкой-заголовком.
2. Имя импорта: из имени таблицы → нижний регистр, всё кроме `[a-z0-9_]` → `_`, ведущая цифра → префикс `t_`; длина ≤ 63.
3. Тип колонки определяет панель:
   - формат первой непустой ячейки колонки похож на дату (содержит `y` или `d` и не `@`) → `date`, если у всех значений нулевая доля суток, иначе `timestamp`;
   - все непустые значения — числа, и хоть одно имеет ведущие нули в отображаемом тексте или длину > 15 цифр → `id`;
   - все непустые значения — числа без дробной части, ≤ 15 цифр → `integer`; с дробной частью → `number`;
   - все — `TRUE`/`FALSE` → `boolean`;
   - иначе `string`.

   Пользователь может переопределить тип перед импортом.
4. Передача: `POST /v1/imports` → порции `rows` по 5 000 строк → `commit`. Даты — строками (§3.1).
5. `reimport_before_run: true` → панель повторяет импорт перед каждым запуском Local-запроса, который зависит от этого импорта.

---

## 13. SQL-функции расширения `ofexcel`

| Сигнатура | Возвращает | Назначение |
|---|---|---|
| `ofexcel_start(port := INTEGER)` | `TABLE(url VARCHAR)` | Запустить HTTP-сервер в фоне |
| `ofexcel_stop()` | `TABLE(stopped BOOLEAN)` | Остановить |
| `ofexcel_serve()` | `TABLE(stopped BOOLEAN)` | Запустить и блокировать до SIGTERM/SIGINT |
| `ofexcel_status()` | `TABLE(key VARCHAR, value VARCHAR)` | Состояние |
| `ofexcel_set_environment(name VARCHAR, environment VARCHAR)` | `TABLE(name VARCHAR, environment VARCHAR)` | Метка окружения подключения |
| `ofexcel_queries()` | `TABLE(query_id, definition_hash, version, engine, source_kind, source_ref, trusted BOOLEAN)` | Каталог запросов |
| `ofexcel_render_oracle(sql VARCHAR, params := STRUCT/MAP)` | `TABLE(rendered VARCHAR, warnings VARCHAR[])` | Подстановка по §4 без выполнения; `@param` выводятся из типов значений, если запроса нет — для отладки |
| `ofexcel_validate(sql VARCHAR, depends := VARCHAR[], params := VARCHAR[])` | `TABLE(ok BOOLEAN, rule VARCHAR, message VARCHAR)` | Валидация по §7 |
| `ofexcel_run(query_id VARCHAR, params := STRUCT, connection := VARCHAR)` | `TABLE(run_id, status, rows_fetched, truncated, error_code, error_message)` | Запуск и ожидание завершения |
| `ofexcel_runs()` | `TABLE` колонок `meta.runs` | История |
| `ofexcel_datasets()` | `TABLE` колонок `meta.dataset_instances` | Экземпляры |
| `ofexcel_reload()` | `TABLE(source VARCHAR, query_id VARCHAR, ok BOOLEAN, message VARCHAR)` | Перечитать проекты |
| `ofexcel_call(kind VARCHAR, query VARCHAR, args JSON-строка VARCHAR)` | `VARCHAR` (JSON результата вызова) | Тот же путь, что `/v1/batch` |
| `ofexcel_missing()` | `TABLE` колонок `meta.missing` | Недостающие данные |

Настройки (`SET …`): `ofexcel_port` (INTEGER, 47321), `ofexcel_profile` (`'hosted'`/`'local'`), `ofexcel_hosted_origin` (VARCHAR), `ofexcel_refresh_parallel` (INTEGER, 4), `ofexcel_run_timeout` (INTEGER секунд, 1800), `ofexcel_spill_max_rows` (INTEGER, 10000), `ofexcel_write_chunk_rows` (INTEGER, 5000), `ofexcel_dataset_ttl_days` (INTEGER, 30), `ofexcel_addin_dir` (VARCHAR, пусто).

---

## 14. Коды ошибок

| Код | HTTP | Excel | Когда |
|---|---|---|---|
| `E_BAD_REQUEST` | 400 / 422 | — | Неверное тело запроса |
| `E_HOST` | 403 | — | Неверный `Host` |
| `E_ORIGIN` | 403 | — | Неверный `Origin` |
| `E_SESSION` | 401 | — | Нет или неверный `X-OF-Session` |
| `E_NOT_FOUND` | 404 | `invalidName` | Нет запроса, запуска, экземпляра |
| `E_CONFLICT` | 409 | — | Имя занято; параллельный запуск с `force` |
| `E_GONE` | 410 | — | Версия результата вытеснена |
| `E_QUERY_PARSE` | 422 | `invalidValue` | Заголовок запроса (§2) |
| `E_UNSUPPORTED` | 422 | `invalidValue` | Не входит в MVP |
| `E_PARAM_UNKNOWN` | 422 | `invalidValue` | §4.1 |
| `E_PARAM_POSITIONAL` | 422 | `invalidValue` | §4.1 |
| `E_PARAM_MISSING` | 422 | `invalidValue` | §3.1 |
| `E_PARAM_TYPE` | 422 | `invalidValue` | §3.1 |
| `E_PARAM_TOO_LONG` | 422 | `invalidValue` | §3.1 |
| `E_PARAM_LIST_CONTEXT` | 422 | `invalidValue` | §4.2 |
| `E_SQL_TOKENIZE` | 422 | `invalidValue` | §4.1 |
| `E_SQL_NOT_SELECT` | 422 | `invalidValue` | §4.1 |
| `E_VALIDATION` | 422 | `invalidValue` | §7 |
| `E_NOT_TRUSTED` | 403 | `notAvailable` | Определение не подтверждено |
| `E_DEPENDENCY_MISSING` | 409 | `notAvailable` | Нет экземпляра зависимости |
| `E_DEPENDENCY_FAILED` | — (в `Run.error`) | — | Зависимость упала |
| `E_EMPTY_UNKNOWN_SCHEMA` | — (в `Run.error`) | `notAvailable` | §5.2 |
| `E_TYPE_DECLARED_MISMATCH` | — (в `Run.error`) | `notAvailable` | §3.3 |
| `E_FUSION_AUTH` | 409 | `notAvailable` | Нет входа, токен истёк, вход не завершён |
| `E_FUSION_REJECTED` | 502 | `notAvailable` | Oracle отклонил запрос (ошибка SQL, прав) |
| `E_FUSION_UNAVAILABLE` | 503 | `notAvailable` | Сеть, BIP недоступен, сработал circuit breaker ofquack |
| `E_CANCELLED` | — (в `Run.error`) | — | Отмена пользователем |
| `E_TIMEOUT` | 504 | `notAvailable` | Превышен тайм-аут |
| `E_OUT_OF_MEMORY` | 507 | `notAvailable` | Нехватка памяти DuckDB |
| `E_SQL_COMPILE` | 422 | `notAvailable` | Ошибка разбора/связывания Local SQL (Parser/Binder/Catalog) |
| `E_SQL_DATA` | 422 | `invalidValue` | Ошибка данных при выполнении Local SQL (Conversion, OutOfRange, InvalidInput) |
| `E_RUN_NOT_FINISHED` | 409 | — | Строки запрошены до завершения |
| `E_INTERNAL` | 500 | `notAvailable` | Прочее; в лог — полный текст |

### 14.1 Сопоставление исключений
| Источник | Исключение | Код |
|---|---|---|
| Запуск Oracle | `InterruptException` после отмены пользователем | `E_CANCELLED` |
| Запуск Oracle | `InterruptException` после тайм-аута | `E_TIMEOUT` |
| Запуск Oracle | `InvalidInputException` из ofquack (ошибка аутентификации) | `E_FUSION_AUTH` |
| Запуск Oracle | `IOException`, текст начинается с `Oracle Fusion rejected the query` | `E_FUSION_REJECTED` |
| Запуск Oracle | `IOException`, текст начинается с `Oracle Fusion returned no rows` | §5.2 |
| Запуск Oracle | прочие `IOException` | `E_FUSION_UNAVAILABLE` |
| Любой | `OutOfMemoryException` | `E_OUT_OF_MEMORY` |
| Local | `ParserException`, `BinderException`, `CatalogException` | `E_SQL_COMPILE` |
| Local | `ConversionException`, `OutOfRangeException`, `InvalidInputException` | `E_SQL_DATA` |
| Любой | прочее | `E_INTERNAL` |

Сопоставление по тексту сообщения — временное, до доработки ofquack (§15).

---

## 15. Требования к ofquack

Изменения в ofquack, которые нужны ядру. До их появления действуют временные решения из этого документа.

| # | Требование | Временное решение |
|---|---|---|
| Q1 | Стабильные коды ошибок: аутентификация, отказ Oracle, недоступность, пустой результат, отмена — отдельными типами исключений или префиксом `[CODE]` в сообщении | Сопоставление по началу текста (§14.1) |
| Q2 | ~~Подтвердить, что отмена прерывает идущий HTTP-запрос~~ | ✅ Подтверждено: 0,2–0,3 с посреди запроса, соединение остаётся рабочим (`PHASE0_REPORT.md` §4.5). Роль `worker` для отмены не нужна |
| Q3 | ~~Подтвердить формат дат и времени при `all_varchar := true`~~ | ✅ Подтверждено с поправками: до 9 знаков дроби, смещение у `TIMESTAMP WITH TIME ZONE`, `DATE` без времени (§3.3) |
| Q4 | ~~Подтвердить, что `all_varchar := true` возвращает значения так, как их выдал `dbms_xmlgen`~~ | ✅ Подтверждено: `.84`, `-.5`, `0012`, длинные целые без изменений |
| Q5 | В режиме `all_varchar` отдавать NULL как NULL, а не пустой строкой | Демон заменяет `''` на NULL для Oracle-результатов (§3.3) |
| Q6 | Колонки `DATE` отдаются со временем (`YYYY-MM-DD HH24:MI:SS`). Это настройка модели данных SQL-отчёта, а не кода ofquack (решение Р2) | `CAST(col AS TIMESTAMP)` в запросе и подсказка в редакторе (§3.3) |

---

## 16. Сводка допущений [Ф0]

| # | Допущение | Раздел | Статус (`PHASE0_REPORT.md`) |
|---|---|---|---|
| A1 | ofquack отдаёт даты как `YYYY-MM-DD`, время как `YYYY-MM-DD[ T]HH:MM:SS` | §3.3, Q3 | ✅ с поправками: дробь до 9 знаков, смещение, `DATE` без времени — правила обновлены (§3.3) |
| A2 | `sys.odcinumberlist`, `odcivarchar2list`, `odcidatelist` доступны через SQL-отчёт BIP | §4.2 | ✅ §4.3 |
| A3 | Временные представления разрешают короткие имена в Local SQL | §5.3 | ✅ §4.7, в том числе при одноимённой таблице |
| A4 | Отмена переводит запуск в `cancelled` за ≤ 5 с | §8.3, Q2 | ✅ 0,2–0,3 с, §4.5 |
| A5 | Порция записи 5 000 строк / ~4 МБ укладывается в лимиты Office.js на обеих платформах | §11.2 | ⏳ надстройка-зонд |
| A6 | Формат `@` не даёт строке `=…` стать формулой при записи через `range.values` | §11.4 | ⏳ надстройка-зонд |
| A7 | Пустые ячейки передаются в custom functions как `null` или `""` | §10.2 | ⏳ надстройка-зонд |
| A8 | `Table.resize` (ExcelApi 1.13) доступен в целевых версиях Excel Mac и Windows | §11.2 | ⏳ надстройка-зонд |
