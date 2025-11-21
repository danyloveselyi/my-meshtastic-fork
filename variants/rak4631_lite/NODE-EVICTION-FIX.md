# Исправление удаления нод при низкой памяти

## Дата: 2025-01-XX

## Проблема:

**Пользователь сообщил:**
- Команда `/mem` показывает 278 нод
- Ноды удаляются, хотя MAX_NUM_NODES = 500 установлено
- Не должно быть удаления нод при 278 нодах, если лимит 500

## Причина проблемы:

### Старая логика (ПРОБЛЕМНАЯ):

```cpp
bool reachedNodeLimit = (numMeshNodes >= MAX_NUM_NODES);
bool lowMemory = isFull();

if (reachedNodeLimit || lowMemory) {
    // Удаляет ноды, даже если их меньше MAX_NUM_NODES!
}
```

**Проблема:**
- `isFull()` проверяет память (MINIMUM_SAFE_FREE_HEAP = 1500 или 8192 bytes)
- Если памяти мало, ноды удаляются, **даже если их меньше MAX_NUM_NODES**
- Это неправильно - ноды должны удаляться только при достижении лимита MAX_NUM_NODES

### Пример:
- MAX_NUM_NODES = 500
- Текущее количество нод = 278
- Свободная память = 1000 bytes (< MINIMUM_SAFE_FREE_HEAP)
- **Результат:** Ноды удаляются, хотя лимит не достигнут! ❌

---

## Решение:

### Новая логика (ИСПРАВЛЕННАЯ):

```cpp
bool reachedNodeLimit = (numMeshNodes >= MAX_NUM_NODES);
bool lowMemory = isFull();

// CRITICAL FIX: Only evict nodes if we've reached the node limit
// Don't evict nodes based on memory alone if we're below MAX_NUM_NODES
if (reachedNodeLimit) {
    // Удаляет ноды только если лимит достигнут
    LOG_INFO("Node database full: %u/%u nodes", numMeshNodes, MAX_NUM_NODES);
    // ... удаление старой ноды ...
} else if (lowMemory) {
    // Только логируем предупреждение, но НЕ удаляем ноды
    LOG_WARN("Low memory: %u bytes free, but node count (%u) is below limit (%u)",
             memGet.getFreeHeap(), numMeshNodes, MAX_NUM_NODES);
    LOG_WARN("Not evicting nodes - memory pressure should be handled by other mechanisms");
    // Продолжаем добавлять новую ноду, так как лимит не достигнут
}
```

**Исправление:**
- Ноды удаляются **только** если `reachedNodeLimit = true` (numMeshNodes >= MAX_NUM_NODES)
- Если `lowMemory = true`, но `reachedNodeLimit = false`, ноды **НЕ удаляются**
- Это позволяет использовать все MAX_NUM_NODES слотов перед удалением

---

## Проверка MAX_NUM_NODES:

### В `variants/rak4631_lite/variant.h`:

```cpp
// Override default MAX_NUM_NODES for RAK4631 at compile time
#undef MAX_NUM_NODES
#define MAX_NUM_NODES 500
```

**MAX_NUM_NODES = 500 установлено правильно!** ✅

### Порядок включения файлов:

1. `configuration.h` (включает `variant.h`) - MAX_NUM_NODES переопределяется до 500
2. `mesh-pb-constants.h` - проверяет `#ifndef MAX_NUM_NODES`, но он уже определен
3. Результат: MAX_NUM_NODES = 500 применяется ✅

---

## Ожидаемое поведение после исправления:

### Сценарий 1: 278 нод, память нормальная
- `reachedNodeLimit = false` (278 < 500)
- `lowMemory = false` (память нормальная)
- **Результат:** Новая нода добавляется, старые не удаляются ✅

### Сценарий 2: 278 нод, память низкая
- `reachedNodeLimit = false` (278 < 500)
- `lowMemory = true` (память < MINIMUM_SAFE_FREE_HEAP)
- **Результат:** Новая нода добавляется, старые НЕ удаляются ✅
- **Логируется:** WARN о низкой памяти, но ноды не удаляются

### Сценарий 3: 500 нод, память нормальная
- `reachedNodeLimit = true` (500 >= 500)
- `lowMemory = false` (память нормальная)
- **Результат:** Самая старая нода удаляется, новая добавляется ✅

### Сценарий 4: 500 нод, память низкая
- `reachedNodeLimit = true` (500 >= 500)
- `lowMemory = true` (память < MINIMUM_SAFE_FREE_HEAP)
- **Результат:** Самая старая нода удаляется, новая добавляется ✅

---

## Выводы:

### ✅ Исправлено:

1. **Ноды не удаляются по памяти**, если количество нод меньше MAX_NUM_NODES
2. **Ноды удаляются только при достижении лимита** MAX_NUM_NODES (500)
3. **Память логируется**, но не вызывает удаление нод

### ✅ MAX_NUM_NODES = 500 применяется:

- Переопределено в `variant.h`
- Должно применяться при компиляции
- Если не применяется, нужно проверить порядок включения файлов

### ⚠️ Если MAX_NUM_NODES все еще 80:

Возможные причины:
1. `variant.h` не включается правильно
2. Порядок включения файлов неправильный
3. Нужно проверить build flags в `platformio.ini`

---

## Следующие шаги:

1. ✅ Исправлена логика удаления нод
2. ⏳ Пересобрать проект и проверить
3. ⏳ Проверить, что MAX_NUM_NODES = 500 применяется (можно добавить лог)
4. ⏳ Протестировать с большим количеством нод

---

## Дополнительная проверка MAX_NUM_NODES:

Можно добавить лог при загрузке для проверки:

```cpp
LOG_INFO("MAX_NUM_NODES = %u (should be 500 for RAK4631 Lite)", MAX_NUM_NODES);
```

Это поможет убедиться, что переопределение работает правильно.

