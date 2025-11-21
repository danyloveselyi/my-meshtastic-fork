# Анализ удаления всех нод после перепрошивки

## Дата: 2025-01-XX

## Проблема:

**Пользователь сообщил:**
- После изменений и перепрошивки все ноды были удалены
- Это не входило в план изменений

## Причина проблемы:

### Логика версионирования extended filesystem:

В `NodeDB-extended-fs.cpp` есть проверка версии файловой системы:

```cpp
constexpr const char* VERSION_FILE = "/.extended_fs_version";
constexpr uint32_t EXPECTED_VERSION = 1;  // Version 1 = 80 pages extended filesystem
```

### Когда происходит форматирование (удаление всех данных):

1. **Mount failed** - файловая система не может быть смонтирована
2. **Version file not found** - файл версии не найден
3. **Version mismatch** - версия не совпадает с EXPECTED_VERSION

### Код, который удаляет данные:

```cpp
if (need_reformat) {
    // Unmount if mounted
    if (mount_result == LFS_ERR_OK) {
        LOG_INFO("Unmounting filesystem before reformat...");
        lfs_unmount(&extended_lfs);
    }
    
    LOG_WARN("REFORMATTING EXTENDED FILESYSTEM");
    LOG_WARN("Reason: %s", 
             mount_result != LFS_ERR_OK ? "Mount failed" :
             "Version mismatch or missing version file");
    
    // CRITICAL: Erase all pages before formatting to avoid "Bad block" errors
    bool erase_success = eraseExtendedFSPages();  // УДАЛЯЕТ ВСЕ ДАННЫЕ!
    
    // Format filesystem
    int format_result = lfs_format(&extended_lfs, &extended_lfs_cfg);  // УДАЛЯЕТ ВСЕ ДАННЫЕ!
}
```

---

## Почему это произошло:

### Возможные причины:

1. **Первая инициализация после перепрошивки:**
   - Если extended filesystem не была инициализирована ранее
   - Файл версии не существует
   - Файловая система форматируется заново
   - **Результат:** Все данные удалены, включая `nodes.proto`

2. **Изменение EXPECTED_VERSION:**
   - Если EXPECTED_VERSION был изменен с 1 на другое значение
   - Старая файловая система имеет версию 1
   - Новая прошивка ожидает другую версию
   - **Результат:** Файловая система форматируется заново

3. **Mount failed:**
   - Файловая система повреждена
   - Не может быть смонтирована
   - **Результат:** Файловая система форматируется заново

4. **Файл версии удален или поврежден:**
   - Файл `/.extended_fs_version` не найден
   - **Результат:** Файловая система форматируется заново

---

## Это было запланировано?

**НЕТ!** Это НЕ было запланировано. Это побочный эффект логики версионирования.

### Проблема в дизайне:

Логика версионирования была добавлена для:
- Защиты от использования старого формата файловой системы
- Обеспечения совместимости при изменении структуры

Но она **слишком агрессивна** - форматирует файловую систему при любом несоответствии, даже если это просто первая инициализация.

---

## Решение:

### ❌ Вариант 1: Backup/Restore в main filesystem (НЕВОЗМОЖЕН)

**Проблема:** Main filesystem имеет только 7 страниц (28 KB), а `nodes.proto` для 278 нод может быть **70-100 KB** (в protobuf формате).

**Вывод:** Backup в main filesystem **невозможен** - не хватает места! ❌

### ✅ Вариант 2: Проверить main filesystem перед форматированием (РЕКОМЕНДУЕТСЯ)

Если extended filesystem нужно отформатировать, но `nodes.proto` существует в **main filesystem**, не форматировать extended filesystem, а просто использовать main filesystem:

```cpp
if (need_reformat) {
    // CRITICAL: Check if nodes.proto exists in main filesystem
    // If it does, don't reformat extended FS - just use main FS
    if (FSCom.exists("/prefs/nodes.proto")) {
        LOG_WARN("nodes.proto found in main filesystem - skipping extended FS reformat");
        LOG_WARN("Will use main filesystem for nodes.proto (limited to ~80 nodes)");
        need_reformat = false;  // Don't reformat - use main FS
    } else {
        // No nodes.proto in main FS - safe to reformat extended FS
        // This is first initialization or nodes.proto was never in main FS
    }
}
```

### ✅ Вариант 3: Не форматировать при первой инициализации (РЕКОМЕНДУЕТСЯ)

Проверять, была ли extended filesystem инициализирована ранее. Если нет - не форматировать, просто создать новую:

```cpp
// Check if this is first initialization
bool is_first_init = (mount_result != LFS_ERR_OK && 
                      !checkIfExtendedFSWasEverInitialized());

if (need_reformat && !is_first_init) {
    // Only reformat if filesystem was initialized before
    // This prevents deleting data on first boot
}
```

### Вариант 2: Не форматировать при первой инициализации

Проверять, была ли файловая система инициализирована ранее, и не форматировать, если это первая инициализация:

```cpp
// Check if this is first initialization
bool is_first_init = (mount_result != LFS_ERR_OK && 
                      !checkIfExtendedFSWasEverInitialized());

if (need_reformat && !is_first_init) {
    // Only reformat if filesystem was initialized before
    // This prevents deleting data on first boot
}
```

### Вариант 3: Убрать проверку версии (НЕ РЕКОМЕНДУЕТСЯ)

Убрать проверку версии полностью - это может привести к проблемам совместимости в будущем.

---

## Рекомендация:

**Использовать Вариант 1** - сохранять `nodes.proto` перед форматированием и восстанавливать после.

Это позволит:
- Сохранить данные при обновлении прошивки
- Сохранить данные при изменении версии
- Сохранить данные при повреждении файловой системы (если возможно)

---

## Временное решение (для пользователя):

Если ноды были удалены, они могут быть восстановлены из:
1. **Других нод в сети** - ноды будут автоматически добавлены при получении пакетов
2. **Backup** - если был сделан backup перед перепрошивкой
3. **Main filesystem** - если `nodes.proto` был сохранен в main filesystem (но это не должно происходить)

---

## Следующие шаги:

1. ✅ Анализ проблемы завершен
2. ⏳ Реализовать backup/restore механизм для `nodes.proto`
3. ⏳ Протестировать на пустой файловой системе
4. ⏳ Протестировать на файловой системе с данными
5. ⏳ Добавить предупреждение в логи при форматировании

---

## Вывод:

**Удаление всех нод НЕ было запланировано.** Это побочный эффект агрессивной логики версионирования, которая форматирует файловую систему при любом несоответствии версии или при первой инициализации.

**Нужно добавить механизм backup/restore для `nodes.proto` перед форматированием.**

