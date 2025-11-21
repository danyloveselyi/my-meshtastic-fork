# Расширенная Filesystem для NodeDB

## Обзор

Этот вариант реализует **двухфайловую систему** для RAK4631 Lite:

1. **Основная filesystem (28 KB, 7 страниц)** - используется для конфигов:
   - `config.proto` - настройки устройства
   - `channels.proto` - каналы
   - `device.proto` - состояние устройства
   - `module.proto` - конфигурация модулей
   - `uiconfig.proto` - настройки UI

2. **Расширенная filesystem (320 KB, 80 страниц)** - используется **ТОЛЬКО** для NodeDB:
   - `nodes.proto` - база данных нод (может хранить 400+ нод)

## Преимущества

- ✅ Основная filesystem остается стандартной (28 KB) - **ничего не меняется в framework**
- ✅ NodeDB получает 320 KB (80 страниц) - достаточно для 400+ нод
- ✅ Безопасность: основная filesystem не переполняется
- ✅ Совместимость: стандартный Meshtastic framework используется для основной filesystem

## Конфигурация

### В `platformio.ini`:

```ini
platform_packages =
  ; Стандартный Meshtastic framework (7 страниц, 28 KB) - для основной filesystem
  platformio/framework-arduinoadafruitnrf52 @ https://github.com/meshtastic/Adafruit_nRF52_Arduino#e13f5820002a4fb2a5e6754b42ace185277e5adf

build_flags =
  ...
  -DUSE_EXTENDED_FS_FOR_NODEDB  ; Включить расширенную filesystem для NodeDB
```

### Ваш custom fork НЕ нужен!

Расширенная filesystem создается **вручную** через прямые вызовы LittleFS API, поэтому ваш custom fork с 80 страницами **не требуется**. Стандартный framework используется для основной filesystem.

## Архитектура

### Основная filesystem (28 KB)
- **Адрес**: `0xED000` (стандартный адрес Meshtastic)
- **Размер**: 7 страниц × 4 KB = 28 KB
- **Использование**: Все конфиги, кроме `nodes.proto`

### Расширенная filesystem (320 KB)
- **Адрес**: `0x80000` (после application, ограниченного linker script)
- **Размер**: 80 страниц × 4 KB = 320 KB
- **Использование**: Только `nodes.proto`
- **Защита**: 144 KB зазор до bootloader (0xF4000)

## Файлы

- `NodeDB-extended-fs.cpp` - реализация расширенной filesystem
- `NodeDB-extended-fs.h` - заголовочный файл
- `FSCommon-fallback.cpp` - инициализация расширенной filesystem

## Инициализация

Расширенная filesystem инициализируется автоматически в `fsInitExtended()` после монтирования основной filesystem.

## Текущий статус

✅ **Компиляция**: Успешно  
✅ **Инициализация**: Реализована  
✅ **Монтирование**: Реализовано  
⚠️ **Интеграция с NodeDB**: Требует модификации `NodeDB.cpp` для перехвата операций с `nodes.proto`

## Следующие шаги

Для полной интеграции нужно модифицировать `NodeDB::loadProto()` и `NodeDB::saveProto()` в `src/mesh/NodeDB.cpp`, чтобы они проверяли, является ли файл `nodes.proto`, и если да - использовали расширенную filesystem вместо основной.

## Безопасность

- ✅ Проверка перекрытия с bootloader (144 KB зазор)
- ✅ Защита от стирания bootloader страниц
- ✅ Автоматическое форматирование при первой инициализации
- ✅ Fallback на основную filesystem, если расширенная недоступна

