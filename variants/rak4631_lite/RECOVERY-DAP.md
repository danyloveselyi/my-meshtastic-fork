# 🔧 Восстановление RAK4631 Lite через DAP Debugger

## 📋 Подготовка перед восстановлением

### 1. Соберите прошивку (если еще не собрана):
```bash
cd /Users/danyloveselyi/Documents/work/my-meshtastic-fork
pio run -e rak4631_lite
```

**Путь к собранной прошивке:**
```
.pio/build/rak4631_lite/firmware.hex
```

---

## 🔌 Подключение DAP Debugger

**SWD интерфейс RAK4631 Lite:**
- **SWDIO** → DAP DATA (SWDIO)
- **SWDCLK** → DAP CLK (SWCLK)
- **GND** → DAP GND
- **VCC** → DAP 3.3V (если нужен внешний источник питания)

---

## ✅ Способ 1: Через pyocd (рекомендуется)

### Установка pyocd (если еще не установлен):
```bash
pip install pyocd
```

### Команды для восстановления:

**Шаг 1: Проверьте, что DAP подключен:**
```bash
pyocd list
```
Вы должны увидеть ваш DAP debugger в списке.

**Шаг 2: Прошейте bootloader (если есть файл):**
```bash
cd /Users/danyloveselyi/Documents/work/my-meshtastic-fork

# Если у вас есть bootloader HEX файл:
pyocd flash -t nrf52840 -e sector bin/generic/Meshtastic_7.3.0_bootloader-0.9.2_s140_7.3.0.hex
```

**Шаг 3: Прошейте исправленную прошивку:**
```bash
# Убедитесь, что прошивка собрана:
pio run -e rak4631_lite

# Прошейте прошивку:
pyocd flash -t nrf52840 -e sector .pio/build/rak4631_lite/firmware.hex

# Перезагрузите устройство:
pyocd commander -t nrf52840 -c reset
```

---

## ✅ Способ 2: Через openocd (если установлен)

### Установка openocd:
```bash
# macOS
brew install openocd

# или через PlatformIO:
# уже должен быть в ~/.platformio/packages/tool-openocd/
```

### Команды для восстановления:

**Шаг 1: Запустите openocd в отдельном терминале:**
```bash
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg
```

**Шаг 2: В другом терминале прошейте bootloader (если нужно):**
```bash
cd /Users/danyloveselyi/Documents/work/my-meshtastic-fork

# Если у вас есть bootloader HEX файл:
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg \
  -c "program bin/generic/Meshtastic_7.3.0_bootloader-0.9.2_s140_7.3.0.hex verify reset exit"
```

**Шаг 3: Прошейте прошивку:**
```bash
# Убедитесь, что прошивка собрана:
pio run -e rak4631_lite

# Прошейте прошивку:
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg \
  -c "program .pio/build/rak4631_lite/firmware.hex verify reset exit"
```

---

## ✅ Способ 3: Через PlatformIO с DAP

### Временно измените `variants/rak4631_lite/platformio.ini`:

Добавьте или раскомментируйте:
```ini
upload_protocol = jlink
```

Или для CMSIS-DAP:
```ini
upload_protocol = cmsis-dap
```

### Команды:

```bash
cd /Users/danyloveselyi/Documents/work/my-meshtastic-fork

# Соберите и прошейте:
pio run -e rak4631_lite -t upload
```

---

## ⚡ Быстрая команда (если bootloader НЕ нужен):

**Если bootloader на месте, просто прошейте прошивку:**

```bash
cd /Users/danyloveselyi/Documents/work/my-meshtastic-fork

# 1. Соберите прошивку:
pio run -e rak4631_lite

# 2. Прошейте через pyocd:
pyocd flash -t nrf52840 -e sector .pio/build/rak4631_lite/firmware.hex

# 3. Перезагрузите:
pyocd commander -t nrf52840 -c reset
```

---

## 📝 Важные заметки

1. **Флаги pyocd:**
   - `-t nrf52840` - тип микроконтроллера
   - `-e sector` - стирать только секторы (быстрее, чем `-e chip`)
   - `-e chip` - полное стирание (используйте если есть проблемы)

2. **Если pyocd не видит DAP:**
   ```bash
   # Проверьте USB подключение:
   lsusb  # Linux
   system_profiler SPUSBDataType  # macOS
   
   # Попробуйте указать DAP явно:
   pyocd list --target
   pyocd flash -t nrf52840 -u <DAP_ID> firmware.hex
   ```

3. **Если прошивка не работает после восстановления:**
   - Попробуйте полное стирание: `-e chip` вместо `-e sector`
   - Проверьте, что прошивка собрана правильно: `pio run -e rak4631_lite -v`
   - Убедитесь, что bootloader совместим

4. **После успешной прошивки:**
   - Устройство должно появиться как USB устройство
   - Серийный порт должен быть виден: `pio device list`
   - Должна работать прошивка через USB: `pio run -e rak4631_lite -t upload`

---

## 🔍 Проверка после восстановления

```bash
# Проверьте, что устройство видно:
pio device list

# Должно появиться что-то вроде:
# /dev/cu.usbmodem...
# Description: WisCore RAK4631 Board
```

---

## 📚 Полезные ссылки

- pyocd: https://github.com/pyocd/pyOCD
- openocd: http://openocd.org/
- Nordic DAP: https://www.nordicsemi.com/Software-and-Tools/Development-Tools/nRF52840-DK

