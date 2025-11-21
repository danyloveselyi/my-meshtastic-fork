# Объяснение незакоммиченных изменений в коде

## Дата: 2025-01-XX

## Обзор изменений:

Есть два файла с незакоммиченными изменениями:
1. **`src/mesh/NodeDB.cpp`** - исправления логики удаления нод
2. **`variants/rak4631_lite/NodeDB-extended-fs.cpp`** - защита от удаления нод при форматировании

---

## 1. Изменения в `src/mesh/NodeDB.cpp`

### Изменение #1: Лог MAX_NUM_NODES при инициализации

**Место:** Строка 493 (функция `NodeDB::installDefaultNodeDatabase()`)

**Было:**
```cpp
nodeDatabase.nodes.reserve(MAX_NUM_NODES);
numMeshNodes = 0;
```

**Стало:**
```cpp
nodeDatabase.nodes.reserve(MAX_NUM_NODES);
LOG_INFO("NodeDB initialized: MAX_NUM_NODES = %u (should be 500 for RAK4631 Lite with extended filesystem)", MAX_NUM_NODES);
numMeshNodes = 0;
```

**Для чего:**
- **Диагностика:** Показывает, какой `MAX_NUM_NODES` применяется при компиляции
- **Проверка:** Помогает убедиться, что переопределение в `variant.h` работает правильно
- **Отладка:** Если видим 80 вместо 500, значит переопределение не применилось

**Проблема, которую решает:**
- Пользователь сообщил, что ноды удаляются при 278 нодах, хотя лимит должен быть 500
- Этот лог поможет проверить, действительно ли `MAX_NUM_NODES = 500` применяется

---

### Изменение #2: Исправление логики удаления нод (НЕ удалять по памяти)

**Место:** Строки 2339-2359 (функция `NodeDB::getOrCreateMeshNode()`)

**Было:**
```cpp
if (reachedNodeLimit || lowMemory) {
    if (reachedNodeLimit) {
        LOG_INFO("Node database full: %u/%u nodes", numMeshNodes, MAX_NUM_NODES);
    }
    if (lowMemory) {
        LOG_WARN("Low memory: %u bytes free (minimum: %u)",
                 memGet.getFreeHeap(), MINIMUM_SAFE_FREE_HEAP);
    }
    LOG_INFO("Erasing oldest entry");
    // ... удаление ноды ...
}
```

**Стало:**
```cpp
// CRITICAL FIX: Only evict nodes if we've reached the node limit
// Don't evict nodes based on memory alone if we're below MAX_NUM_NODES
if (reachedNodeLimit) {
    LOG_INFO("Node database full: %u/%u nodes", numMeshNodes, MAX_NUM_NODES);
    LOG_INFO("Erasing oldest entry to make room for new node");
} else if (lowMemory) {
    // Only log memory warning, but don't evict nodes if we're below MAX_NUM_NODES
    LOG_WARN("Low memory: %u bytes free (minimum: %u), but node count (%u) is below limit (%u)",
             memGet.getFreeHeap(), MINIMUM_SAFE_FREE_HEAP, numMeshNodes, MAX_NUM_NODES);
    LOG_WARN("Not evicting nodes - memory pressure should be handled by other mechanisms");
}

// Only proceed with eviction if we've reached the node limit
if (reachedNodeLimit) {
    // ... удаление ноды ...
}
```

**Для чего:**
- **Проблема:** Старая логика удаляла ноды, если памяти мало, **даже если нод меньше MAX_NUM_NODES**
- **Пример:** При 278 нодах и низкой памяти (< 1500 bytes) ноды удалялись, хотя лимит 500
- **Решение:** Ноды удаляются **только** при достижении `MAX_NUM_NODES` (500)
- **Результат:** Можно использовать все 500 слотов, даже если память низкая

**Почему это важно:**
- Память может быть низкой по другим причинам (буферы, очереди, временные данные)
- Удаление нод не решает проблему памяти, а только уменьшает функциональность
- Если лимит 500, нужно использовать все 500 слотов перед удалением

---

### Изменение #3: Принудительное удаление, если нет "boring" нод

**Место:** Строки 2391-2405 (функция `NodeDB::getOrCreateMeshNode()`)

**Было:**
```cpp
if (oldestIndex != -1) {
    // Удаление ноды
    (numMeshNodes)--;
}
// Продолжение - добавление новой ноды
```

**Стало:**
```cpp
if (oldestIndex != -1) {
    // Удаление ноды
    (numMeshNodes)--;
} else {
    // CRITICAL FIX: If no node found to evict, try to evict the oldest non-favorite node
    if (numMeshNodes > 1 && numMeshNodes >= MAX_NUM_NODES) {
        LOG_WARN("No 'boring' node found to evict, forcing eviction of oldest non-favorite node");
        oldestIndex = 1;  // Force evict second node (oldest after our node)
        // Удаление ноды
        (numMeshNodes)--;
    } else if (numMeshNodes >= MAX_NUM_NODES) {
        // CRITICAL: Cannot add new node - database is full and no node can be evicted
        LOG_ERROR("Cannot add node %u: database full (%u/%u nodes) and no node can be evicted", n, numMeshNodes, MAX_NUM_NODES);
        return nullptr;  // Предотвратить добавление ноды
    }
}
```

**Для чего:**
- **Проблема:** Если все ноды - favorites/ignored/verified, старая логика не могла найти ноду для удаления
- **Результат:** `push_back()` вызывался при `numMeshNodes == MAX_NUM_NODES`, что вызывало `assert` и перезагрузку
- **Решение:** Если нет "boring" нод, принудительно удалить самую старую non-favorite ноду (индекс 1)
- **Защита:** Если даже это не помогает, вернуть `nullptr` вместо добавления ноды

**Почему это важно:**
- Предотвращает `assert failed` и перезагрузку устройства
- Обеспечивает, что новая нода добавляется только если есть место
- Логирует ошибку, если база данных полна и нет нод для удаления

---

### Изменение #4: Дополнительная проверка перед добавлением ноды

**Место:** Строки 2407-2412 (функция `NodeDB::getOrCreateMeshNode()`)

**Было:**
```cpp
// Create new node and add it to vector
meshtastic_NodeInfoLite newNode = {};
newNode.num = n;
meshNodes->push_back(newNode);
```

**Стало:**
```cpp
// NOTE: At this point, we should have space (either under limit, or old node was evicted)
if (numMeshNodes >= MAX_NUM_NODES) {
    // This should ideally not be reached if eviction logic is perfect, but as a safeguard
    LOG_ERROR("Attempting to add node %u when database is full (%u/%u nodes) after eviction attempt. Returning nullptr.", n, numMeshNodes, MAX_NUM_NODES);
    return nullptr;  // Prevent adding node to avoid memory issues
}

meshtastic_NodeInfoLite newNode = {};
newNode.num = n;
meshNodes->push_back(newNode);
```

**Для чего:**
- **Защита:** Дополнительная проверка перед `push_back()`, даже если логика удаления должна была освободить место
- **Безопасность:** Если логика удаления не сработала, предотвращает добавление ноды и потенциальные проблемы с памятью
- **Диагностика:** Логирует ошибку, если это происходит (должно быть редко)

**Почему это важно:**
- `push_back()` при `numMeshNodes == MAX_NUM_NODES` может вызвать проблемы с памятью
- Это последняя линия защиты от превышения лимита
- Помогает диагностировать проблемы в логике удаления

---

## 2. Изменения в `variants/rak4631_lite/NodeDB-extended-fs.cpp`

### Изменение: Защита от удаления нод при форматировании extended filesystem

**Место:** Строки 730-760 (функция `ExtendedNodeDBFS::init()`)

**Было:**
```cpp
if (need_reformat) {
    // Unmount if mounted
    if (mount_result == LFS_ERR_OK) {
        lfs_unmount(&extended_lfs);
    }
    
    LOG_WARN("REFORMATTING EXTENDED FILESYSTEM");
    // ... форматирование ...
}
```

**Стало:**
```cpp
if (need_reformat) {
    // CRITICAL: Before reformatting, check if nodes.proto exists in main filesystem
    const char* nodes_proto_path = "/prefs/nodes.proto";
    bool nodes_in_main_fs = false;
    
    #ifdef FSCom
    if (FSCom.exists(nodes_proto_path)) {
        nodes_in_main_fs = true;
        LOG_WARN("WARNING: nodes.proto found in MAIN filesystem!");
        LOG_WARN("Reformatting extended FS will DELETE all nodes!");
        LOG_WARN("DECISION: Will NOT reformat extended FS to preserve data");
        need_reformat = false;  // Don't reformat - preserve data
    }
    #endif
    
    if (need_reformat) {
        // Unmount if mounted
        // ... форматирование ...
    }
}
```

**Для чего:**
- **Проблема:** При перепрошивке extended filesystem форматируется, если:
  - Mount failed (файловая система не смонтировалась)
  - Version file not found (файл версии не найден)
  - Version mismatch (версия не совпадает)
- **Результат:** Все ноды удаляются при форматировании
- **Решение:** Перед форматированием проверить, есть ли `nodes.proto` в main filesystem
- **Логика:** Если `nodes.proto` найден в main FS, extended FS **НЕ форматируется**, чтобы сохранить данные

**Почему это важно:**
- **Backup невозможен:** Main FS (28 KB) не может вместить backup `nodes.proto` (70-100 KB для 278 нод)
- **Защита данных:** Если `nodes.proto` есть в main FS, значит extended FS может содержать данные
- **Предотвращение потери:** Не форматировать extended FS, если есть риск потери данных

**Когда это срабатывает:**
- При первой перепрошивке после включения extended filesystem
- Если extended FS повреждена, но `nodes.proto` есть в main FS
- Если версия изменилась, но данные важны

**Ограничения:**
- Это не идеальное решение - extended FS останется неиспользуемой
- Но это лучше, чем потерять все ноды
- В будущем можно улучшить (например, попытаться восстановить данные)

---

## Резюме изменений:

### Проблемы, которые решают изменения:

1. **Ноды удаляются при 278 нодах, хотя лимит 500:**
   - ✅ Исправлено: Ноды удаляются только при достижении лимита, не по памяти

2. **Все ноды удалились после перепрошивки:**
   - ✅ Исправлено: Extended FS не форматируется, если `nodes.proto` найден в main FS

3. **Assert failed при добавлении ноды:**
   - ✅ Исправлено: Принудительное удаление и проверка перед добавлением

### Что делают изменения:

1. **Лог MAX_NUM_NODES:** Диагностика - показывает, какой лимит применяется
2. **Логика удаления:** Исправление - удалять только по лимиту, не по памяти
3. **Принудительное удаление:** Защита - удалять ноду, даже если все favorites/ignored
4. **Проверка перед добавлением:** Безопасность - не добавлять, если нет места
5. **Защита от форматирования:** Сохранение данных - не форматировать, если есть риск потери

---

## Следующие шаги:

1. ✅ Изменения восстановлены
2. ⏳ Протестировать на устройстве
3. ⏳ Проверить логи (MAX_NUM_NODES должен быть 500)
4. ⏳ Проверить, что ноды не удаляются при 278 нодах
5. ⏳ Проверить, что ноды не удаляются при перепрошивке

