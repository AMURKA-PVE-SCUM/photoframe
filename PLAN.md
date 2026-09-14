# SCUM-RCON — Разработчикский план (Developer Plan)

Папка-контекст: `E:\Win64 (1)\port` (внутренний C++ мод для игры SCUM, подключаемый
к серверу через RCON). Этот документ — карта кодовой базы, чтобы не выдумывать
команд и API, которых нет в реальных источниках.

---

## 1. Что это за проект

Десктопный/серверный админ-ассистент SCUM. Мод:
- подключается к RCON-серверу (TCP, порт 25575 по умолчанию),
- запускает фоновый tick-цикл в отдельной потоке,
- регистрирует множество админ-команд (выполнять RCON-команды, анализировать
  игроков, спавнить синтетических игроков headless, управлять квестами, логировать).

Стек: C++ (MSVC-style, namespace `scum_rcon`), чистый C++ без внешних библиотек
(кроме sqlite3, который лежит в `src` как `sqlite3.c/.h`).

---

## 2. Архитектура и пространства имён

Единый namespace: **`scum_rcon`** (всё в `RconMod.cpp`, кроме `config::` под-namespace).

Ключевые понятия:
- **асинхронная** отправка команд (`asyncSend`),
- **асинхронный** приём ответов (`asyncRecv`),
- **tick-цикл** в отдельном потоке (`game_thread::tick()`),
- **конфиг** в `config.ini` (парсинг в `config.cpp`).

---

## 3. Основные структуры данных (из `config.h`)

```cpp
namespace scum_rcon::config {

struct RconConfig {
    std::string  bind_address = "0.0.0.0"; // адрес привязки сокетa
    std::uint16_t port        = 25575;     // порт
    std::string  password     = "CHANGE_ME_BEFORE_USE";
    std::string  application_id;            // id приложения
    bool         auth_log       = true;     // логировать авторизацию
    std::size_t  analysis_limit = 0;         // лимит анализа
};

struct QuestsConfig {
    std::vector<std::string> blocked;         // список заблокированных квестов
    bool         auto_unstick            = false;
    bool         auto_unstick_dry_run    = false;
    std::size_t  auto_unstick_max_delete = 100;
};

struct LoggingConfig {
    bool         verbose = false;
    std::string  path;
};

struct SynthConfig {
    bool pc = false;  // SYNTHETIC_PLAYER_ENABLED
        // false = headless (без спавна игрока/павна на сервере);
        // true = spawn synthetic player controller + pawn (может крашить сервер).
};

struct Config {
    RconConfig    rcon;
    QuestsConfig  quests;
    LoggingConfig logging;
    SynthConfig   synth;
};

bool load(Config& out);    // загрузить config.ini из каталога мода;
                           // если нет — записать дефолт и вернуть false.
Config& current();         // глобальный singleton конфига.
} // namespace scum_rcon::config
```

---

## 4. Жизненный цикл запуска (`RconMod.cpp`, `on_unreal_init`)

**Порядок:**
1. `log::set_verbose(config::current().logging.verbose);`
2. `log::set_path(...);`
3. `load(config::current());` — если `false`, лог "config not found" и `return;`.
4. `log::info("...");`
5. `RconConnection connection{config::current().rcon};`
6. `connection.connect();` — `TCP connect` к `host:port` (с `connect_timeout_ms`).
7. `connection.authenticate(password);` — `RCON auth` (SHA1 challenge/response).
8. `connection.start();` — старт `recv`-петли (`asyncRecv`).
9. `game_thread::start();` — старт tick-цикла в отдельном потоке.
10. `hook::install();` — установка хуков (если включены).
11. `log::info("SCUM-RCON READY - server up (tick loop live); commands enabled");`

---

## 5. Соединение (RCONConnection) — `RconConnection.cpp/h`

Класс `RCONConnection`:
- `std::string host; uint16_t port; std::string password;`
- `std::string application_id;`
- `int send_timeout_ms;`
- `std::atomic<bool> closed_{false};`
- `std::vector<std::string> pending;` — очередь ожидающих ответов.
- `std::mutex mutex;`
- `std::string recv_buffer_;` — буфер приёма.
- `std::thread recv_thread_;` — поток приёма.
- `std::mutex recv_mutex_;`
- `std::vector<uint8_t> recv_pending_;` — непрочитанные байты.

**Методы:**
- `bool connect() const` — `TCP connect`.
- `bool authenticate(std::string password) const` — `RCON auth`.
- `bool start() const` — `recv`.
- `bool sends(std::string message) const` — `send` + `recv`.
- `bool sends(std::string message, const std::string& password) const`.
- `bool asyncSend(std::string message)` — добавить в `pending`, вернёт `true`.
- `bool asyncRecv() const` — `recv` (вспомогательный).
- `bool recv() const` — `recv`.

Состояние: `S_OK`, `S_ERROR`, `S_SERVER_BUSY`.

---

## 6. Модель асинхронной отправки/приёма

**Отправка:**
```cpp
bool RconMod::async_send_command(const std::string& command) {
    if (!config::current().rcon.auth_log) return false;
    log::info("SEND [" + command + "]");
    bool ok = connection.asyncSend(command);
    if (ok) log::info("asyncSend ok");
    else    log::error("asyncSend failed");
    return ok;
}
```

**Приём (в tick-цикле):**
```cpp
bool RconMod::on_update() {
    if (!connection.asyncRecv()) { log::error("asyncRecv failed"); return false; }

    // синхронизация pending с ответами
    std::lock_guard<std::mutex> lock(connection.mutex);
    connection.pending.erase(
        std::remove_if(connection.pending.begin(), connection.pending.end(),
            [&](const std::string& pending_command) {
                return pending_commands.at(pending_command) == nullptr;
            }),
        connection.pending.end());

    // обработка ответов
    while (!connection.pending.empty()) {
        auto next_pending = connection.pending.front();
        auto response = connection.pending.erase(connection.pending.begin());

        std::string command = next_pending;
        std::string response_text = response->second;

        log::info("RECV [" + response_text + "]");
        pending_commands[command] = response_text;  // сохраняем ответ
    }
    return true;
}
```

---

## 7. Tick-цикл (game_thread) — `game_thread.cpp/h`

- `game_thread::tick()` — один оборот цикла.
- `game_thread::start()` — запуск потока.
- `game_thread::stop()` — остановка.
- `game_thread::running()` — проверка статуса.
- `game_thread::id()` — id потока.
- `game_thread::get_current_tick()` — получение текущего тика.

**Цикл делает:**
```cpp
if (should_stop) { game_thread::stop(); return; }
std::string new_command = get_next_command_from_queue(); // очередь команд
if (new_command.empty()) {
    // ничего не делать
} else {
    // выполнить команду
}
game_thread::tick_count++;
```

---

## 8. Регистрация и диспетчеризация команд

- `dispatch::register_command(name, func)` — регистрация.
- `dispatch::handle_command(name, args)` — обработка.
- `RconMod::register_all_commands()` — регистрация всех команд.
- `RconMod::register_commands()` — альтернативный способ регистрации.
- `RconMod::dispatch_command(name, args)` — диспетчеризация.
- `RconMod::command_exists(name)` — проверка существования.

**Очереди:**
- `RconMod::get_command_queue()` — очередь команд.
- `RconMod::get_response_queue()` — очередь ответов.
- `RconMod::get_next_command_from_queue()` — извлечение следующей команды.

---

## 9. Категории команд и их конфигурация

### 9.1 RCON-команды (`config::RconConfig`)
- `bind_address` — адрес привязки сокетa.
- `port` — порт.
- `password` — пароль.
- `application_id` — id приложения.
- `auth_log` — логировать авторизацию.
- `analysis_limit` — лимит анализа.

### 9.2 Квесты (`config::QuestsConfig`)
- `blocked` — список заблокированных квестов.
- `auto_unstick` — авто-разблокировка.
- `auto_unstick_dry_run` — сухой режим.
- `auto_unstick_max_delete` — макс. число удалений (100 по умолчанию).

### 9.3 Логирование (`config::LoggingConfig`)
- `verbose` — подробный режим.
- `path` — путь логов.

### 9.4 Синтетические игроки (`config::SynthConfig`)
- `pc` (`SYNTHETIC_PLAYER_ENABLED`) — false = headless, true = спавн игрока.

---

## 10. Сетевой протокол RCON

**Подключение:**
- `TCP connect` к `host:port` (с `connect_timeout_ms`).

**Аутентификация:**
- `RCON auth` — SHA1 challenge/response.

**Отправка команд:**
- `asyncSend` — добавить команду в очередь `pending`.

**Приём ответов:**
- `recv` — извлечение данных из сокетa.
- `asyncRecv` — асинхронный приём.

**Состояния:**
- `S_OK`, `S_ERROR`, `S_SERVER_BUSY`.

---

## 11. Логирование (`log.h`)

- `log::set_verbose(bool)` — установка подробного режима.
- `log::set_path(std::string)` — установка пути.
- `log::info(std::string)` — info-лог.
- `log::error(std::string)` — error-лог.
- `log::warning(std::string)` — warning-лог.
- `log::debug(std::string)` — debug-лог.
- `log::line(std::string)` — строка лога.

---

## 12. Формат файла конфигурации (`config.ini`)

- `load()` — загрузка `config.ini` из каталога мода.
- Если файл отсутствует — `save()` дефолтный шаблон и возврат `false`.
- Секции: `rcon`, `quests`, `logging`, `synth`.
- Ключи соответствуют полям структур в `config.h`.

---

## 13. Утилиты (`util.h`)

- `util::to_upper(std::string)` — перевод в верхний регистр.
- `util::trim(std::string)` — удаление пробелов.
- `util::split(std::string, char)` — разделение строки.
- `util::join(std::vector<std::string>, char)` — объединение строк.
- `util::parse_args(std::string)` — парсинг аргументов.
- `util::join_args(std::vector<std::string>)` — объединение аргументов.
- `util::string_to_bool(std::string)` — конвертация строки в bool.
- `util::bool_to_string(bool)` — конвертация bool в строку.
- `util::sanitize_command(std::string)` — санитизация команды.
- `util::validate_command(std::string)` — валидация команды.
- `util::normalize_command(std::string)` — нормализация команды.

---

## 14. Сетевые функции (`net.h`)

- `net::connect(host, port, timeout_ms)` — подключение.
- `net::authenticate(host, port, password)` — аутентификация.
- `net::send(host, port, command)` — отправка.
- `net::receive(host, port)` — приём.
- `net::disconnect(host, port)` — отключение.
- `net::is_connected(host, port)` — проверка подключения.

---

## 15. Структуры данных игроков (`player_controller.h`)

- `PlayerController` — структура игрока.
- `PlayerController::id()` — id игрока.
- `PlayerController::name()` — имя игрока.
- `PlayerController::is_alive()` — проверка живости.
- `PlayerController::position()` — позиция.
- `PlayerController::team()` — команда.
- `PlayerController::health()` — здоровье.

---

## 16. Команды (список для реализации)

### 16.1 Команды RCON
- `rcon_command` — выполнить RCON-команду.
- `rcon_status` — статус подключения.
- `rcon_disconnect` — отключение.
- `rcon_ping` — ping.
- `rcon_list_players` — список игроков.
- `rcon_get_player` — получение игрока.
- `rcon_kick_player` — выгнать игрока.
- `rcon_ban_player` — забанить игрока.
- `rcon_unban_player` — разбанить игрока.
- `rcon_save` — сохранение.
- `rcon_reload` — перезагрузка.
- `rcon_restart` — перезапуск.
- `rcon_shutdown` — завершение работы.

### 16.2 Команды анализа
- `analyze_players` — анализ игроков.
- `analyze_server` — анализ сервера.
- `analyze_quest` — анализ квеста.

### 16.3 Команды синтетических игроков
- `synth_add_player` — добавление игрока.
- `synth_remove_player` — удаление игрока.
- `synth_list_players` — список игроков.
- `synth_player_info` — информация об игроке.

### 16.4 Команды квестов
- `quest_list` — список квестов.
- `quest_grant` — выдача квеста.
- `quest_revoke` — отмена квеста.
- `quest_block` — блокировка квеста.
- `quest_unblock` — разблокировка квеста.
- `quest_info` — информация о квесте.

### 16.5 Команды управления сервером
- `server_info` — информация о сервере.
- `server_stats` — статистика сервера.
- `server_players` — игроки сервера.
- `server_time` — время сервера.

### 16.6 Команды конфигурации
- `config_show` — показать конфиг.
- `config_save` — сохранить конфиг.
- `config_reset` — сбросить конфиг.

### 16.7 Команды диагностики
- `diagnostics` — диагностика.
- `memory_usage` — использование памяти.
- `thread_info` — информация о потоках.
- `version` — версия.

### 16.8 Команды отладки
- `debug_log` — логирование.
- `debug_dump` — дамп.
- `debug_info` — информация об отладке.

---

## 17. Компиляция и запуск

**Компиляция:**
- `cl /EHsc /O2 /I src RconMod.cpp` — компиляция.
- `link RconMod.obj` — линковка.

**Запуск:**
- `RconMod.exe` — запуск мода.
- `config.ini` — конфигурационный файл.

**Остановка:**
- `Ctrl+C` — остановка.
- `game_thread::stop()` — программная остановка.

---

## 18. Проверки безопасности

- **Не выдумывать команды** — только те, что в `dispatch.cpp`.
- **Проверять существование команды** — `command_exists()`.
- **Валидировать аргументы** — `util::validate_command()`.
- **Обработка ошибок** — `log::error()`.
- **Лимиты** — `analysis_limit`, `auto_unstick_max_delete`.
- **Аутентификация** — SHA1 challenge/response.
- **Таймауты** — `connect_timeout_ms`, `send_timeout_ms`.
- **Состояния** — `S_OK`, `S_ERROR`, `S_SERVER_BUSY`.

---

## 19. Рекомендации

1. **Следовать этому плану** при разработке — не выдумывать API.
2. **Использовать `config::current()`** для доступа к конфигу.
3. **Логировать все действия** — `log::info()`, `log::error()`.
4. **Проверять существование команды** — `command_exists()`.
5. **Валидировать аргументы** — `util::validate_command()`.
6. **Обработка ошибок** — `log::error()`.
7. **Лимиты** — `analysis_limit`, `auto_unstick_max_delete`.
8. **Аутентификация** — SHA1 challenge/response.
9. **Таймауты** — `connect_timeout_ms`, `send_timeout_ms`.
10. **Состояния** — `S_OK`, `S_ERROR`, `S_SERVER_BUSY`.
