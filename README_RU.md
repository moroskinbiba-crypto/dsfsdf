# TOTK Explorer v3.2 — CALIBRATED

Целевая игра: The Legend of Zelda: Tears of the Kingdom

- Version: 1.4.3
- Title ID: `0100F2C0115B6000`
- Build ID: `277178B7DBA1B6D4`

## CI/build note

CI использует libnx, который уже входит в `devkitpro/devkita64`. Проект **не собирает libnx из исходников**.
Это важно: попытка собрать старый pin libnx 4.9.0 внутри современного devkitA64 toolchain может падать в `RMutex`/newlib ещё до сборки самого overlay.

`libtesla` берётся из актуального upstream master, чтобы его HID/service API соответствовал тому же libnx ABI.

`libdmntcht.a` и `dmntcht.h` подтягиваются из Shiny-Stash-Live-Map как готовая зависимость.

## Calibration

После получения рабочего overlay создайте на SD:

`sd:/switch/totk_explorer/calibration.txt`

Пример:

`1234 85 -527`

или:

`1234 85 -527 1.0`

После успешного поиска Explorer должен сохранить профиль и использовать его для live X/Y/Z.

## Важно

Автоматический discovery — эвристический механизм. Я не считаю его гарантированно надёжным до проверки на реальной консоли.
