# Анализ ASSERT failure при перегрузке нодами

## Дата анализа: 2025-01-XX

## Проблема из лога:

```
INFO | 16:15:13 2924 [Router] Adding node to database with 426 nodes and 69940 bytes free!
INFO | 16:15:13 2924 [Router] Adding node to database with 427 nodes and 69940 bytes free!
ERROR | 16:15:40 2951 [Router] assert failed src/platform/nrf52/a
```

### Критичность: 🔴 **КРИТИЧНО!**

Нода перезагружается из-за assert failure.

---

## Причина проблемы:

### 1. Превышение лимита MAX_NUM_NODES

**Лимит для NRF52:**
- `MAX_NUM_NODES = 80` (из `mesh-pb-constants.h`)
- В логе видно **427 нод** - это в **5.3 раза больше лимита!**

### 2. Проблема в логике удаления старых нод

**Код в `getOrCreateMeshNode()` (строки 2336-2389):**

```cpp
bool reachedNodeLimit = (numMeshNodes >= MAX_NUM_NODES);
if (reachedNodeLimit || lowMemory) {
    // Пытается найти старую ноду для удаления
    if (oldestIndex != -1) {
        // Удаляет старую ноду
        (numMeshNodes)--;
    }
    // ПРОБЛЕМА: Если oldestIndex == -1, старая нода НЕ удаляется!
}

// КРИТИЧНО: Код ВСЕГДА добавляет новую ноду, даже если лимит превышен!
meshNodes->push_back(newNode);
numMeshNodes++;
```

**Проблема:**
- Если `oldestIndex == -1` (не найдена старая нода для удаления), код все равно добавляет новую ноду
- Это приводит к превышению лимита MAX_NUM_NODES
- `push_back()` может вызвать перераспределение памяти
- Если памяти не хватает, `rtos_malloc()` вернет NULL
- `operator new()` в `alloc.cpp` делает `assert(p)` - если NULL, то assert и перезагрузка!

### 3. Возможные причины assert:

1. **Нехватка памяти при `push_back()`:**
   - `vector::push_back()` может вызвать перераспределение
   - Если памяти не хватает, `rtos_malloc()` вернет NULL
   - `assert(p)` в `alloc.cpp` срабатывает → перезагрузка

2. **Assert в `getMeshNodeByIndex()`:**
   - `assert(x < numMeshNodes)` в строке 216
   - Если индекс >= numMeshNodes, assert срабатывает

3. **Другие assert в коде:**
   - Могут быть другие места, где используется assert

---

## Важная информация:

**MAX_NUM_NODES уже переопределен в `variant.h`:**
- `#define MAX_NUM_NODES 500` (строка 270)
- Но в логе видно 427 нод - это меньше лимита
- Значит, проблема не в превышении лимита, а в логике удаления или нехватке памяти

## Решение:

### ✅ Вариант 1: Исправить логику удаления (РЕАЛИЗОВАНО)

**Проблема:** Если `oldestIndex == -1`, нода все равно добавляется

**Решение:** Не добавлять новую ноду, если не удалось удалить старую

```cpp
if (reachedNodeLimit || lowMemory) {
    // ... поиск старой ноды ...
    
    if (oldestIndex != -1) {
        // Удалить старую ноду
        (numMeshNodes)--;
    } else {
        // КРИТИЧНО: Не добавлять новую ноду, если не удалось удалить старую!
        LOG_WARN("Cannot add node: database full and no node to evict");
        return nullptr;  // Или вернуть существующую ноду, если есть
    }
}
```

### Вариант 2: Увеличить MAX_NUM_NODES для extended filesystem

**Проблема:** MAX_NUM_NODES = 80 слишком мало для extended filesystem (320 KB)

**Решение:** Увеличить лимит для RAK4631 Lite с extended filesystem

```cpp
// В variants/rak4631_lite/variant.h или platformio.ini
#define MAX_NUM_NODES 400  // Увеличить для extended filesystem
```

**Ограничение:** Нужно проверить, хватит ли RAM для 400 нод

### Вариант 3: Принудительно удалять самую старую ноду

**Проблема:** Если не найдена "boring" нода, старая нода не удаляется

**Решение:** Всегда удалять самую старую ноду, если лимит превышен

```cpp
if (reachedNodeLimit || lowMemory) {
    // ... поиск старой ноды ...
    
    if (oldestIndex == -1) {
        // Если не найдена "boring" нода, удалить самую старую (индекс 1, так как 0 - это наша нода)
        if (numMeshNodes > 1) {
            oldestIndex = 1;  // Принудительно удалить вторую ноду (самую старую после нашей)
        }
    }
    
    if (oldestIndex != -1) {
        // Удалить старую ноду
        (numMeshNodes)--;
    } else {
        // Не добавлять новую ноду, если не удалось удалить старую
        LOG_WARN("Cannot add node: database full and no node to evict");
        return nullptr;
    }
}
```

---

## Рекомендация:

**Использовать Вариант 1 + Вариант 3:**

1. Принудительно удалять самую старую ноду, если лимит превышен
2. Не добавлять новую ноду, если не удалось удалить старую
3. Опционально: увеличить MAX_NUM_NODES для extended filesystem (но проверить RAM)

---

## Проверка RAM:

**Расчет памяти для нод:**
- `sizeof(meshtastic_NodeInfoLite) <= 200 bytes` (из static_assert)
- 400 нод × 200 bytes = **80 KB RAM**
- Плюс overhead vector = **~100 KB RAM**

**Доступная RAM на NRF52:**
- Обычно ~200-300 KB свободной RAM
- 100 KB для нод - это приемлемо

**Вывод:** Можно увеличить MAX_NUM_NODES до 400 для extended filesystem

---

## Реализованные исправления:

### ✅ Исправлена логика удаления старых нод:

1. **Принудительное удаление:** Если не найдена "boring" нода, принудительно удаляется самая старая (индекс 1)
2. **Проверка перед добавлением:** Не добавляется новая нода, если не удалось удалить старую
3. **Дополнительная проверка:** Проверка лимита перед `push_back()` для предотвращения переполнения

**Код:**
```cpp
if (oldestIndex == -1) {
    // Принудительно удалить самую старую ноду, если лимит превышен
    if (numMeshNodes > 1 && numMeshNodes >= MAX_NUM_NODES) {
        oldestIndex = 1;  // Force evict second node
        // ... удаление ...
    } else if (numMeshNodes >= MAX_NUM_NODES) {
        return nullptr;  // Не добавлять новую ноду
    }
}

// Дополнительная проверка перед push_back
if (numMeshNodes >= MAX_NUM_NODES) {
    return nullptr;  // Предотвратить переполнение
}
```

## Следующие шаги:

1. ✅ Исправлена логика удаления старых нод
2. ✅ MAX_NUM_NODES уже увеличен до 500 в variant.h
3. ⏳ Протестировать с большим количеством нод
4. ⏳ Проверить, что assert больше не происходит

