# Авторство и лицензия

Этот проект основан на **Low-Cost-EEG** (https://github.com/XavierAsghar/Low-Cost-EEG), автор Xavier Asghar, лицензия GNU GPL v3.

Из исходного проекта взяты:
- Gerber-файлы платы и трафарета (`hardware/pcb/`), с исправленным смещением файлов сверловки;
- перечень элементов (`hardware/bom.csv`), с исправленными номиналами и заменами;
- схема, разводка и инструкция автора (`hardware/upstream/`);
- библиотека ADS1299 (`firmware/selftest/lib/ADS1299/` и `firmware/eeg/lib/ADS1299/`). Исходный код Conor Russomanno, доработка Xavier Asghar. В этой копии `RREG` и `WREG` не печатают в монитор порта.
- рабочая прошивка ESP32 (`firmware/eeg/`), на основе `src/main.cpp` Xavier Asghar. В этой копии уведомление Bluetooth короче (112 байт вместо 224), а `BIAS_SENSP` включает каналы 1 и 2.

Добавлено в этом проекте: инструкции в `docs/`, корпус и панель в `enclosure/`, прошивка самотеста, списки покупок для России.

Весь репозиторий распространяется на условиях GNU General Public License v3, полный текст в файле `LICENSE`. 3D-инструкции содержат встроенную библиотеку three.js (MIT License, © 2010–2023 three.js authors).
