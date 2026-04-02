Справочник системных сущностей VOID-Core (v2.0: Iron Shield)
Этот документ описывает физическое устройство данных и механизмы управления ресурсами в архитектуре v2.0.
1. Универсальный тип cell и его модификаторы
    В v2.0 поведение ячейки (cell) жестко привязано к аппаратному контексту.
    Модификатор	Синтаксис	Слой	Описание (Hardware Context)	Аналог
    Vocal	cell:vocal	[LOGIC]	Открытые данные. Динамическая типизация. ARC-очистка.	Python / JS
    Ghost	cell:ghost	[SYS]	Hardware AES. Расшифровка только в кэше L1/L2. Невидима для дамперов.	VOID-Security
    Firmware	cell:fwr	[SYS]	BIOS-Locked. Привязана к подписи прошивки. Стирается при смене HWID.	TPM / TEE
    Raw	cell:raw	[SYS]	Прямой адрес (Pointer). Работа без проверок.	C++ / B
    Secure	cell:sec	[LOGIC]	Типизированная ячейка с контролем владения (Borrow Checker).	Rust
2. Операторы управления данными (Data Ops)

    &- (Bridge) — Прямой системный сигнал из высокоуровневой логики в защищенное ядро.
    >> (Pipe) — Передача асинхронного потока данных между изолятами.
    move — Передача владения. Исходный адрес немедленно подвергается Zeroize.
    ghost_cast<T> — Принудительное наложение аппаратного шифрования на данные.

3. Системные узлы (Native Nodes v2.0)
fwr — Firmware & BIOS (Новое в v2.0)

    fwr.verify_sig() — Проверка целостности BIOS через SPI-шину.
    fwr.lock_spi() — Блокировка флэш-памяти прошивки на время сессии.
    fwr.anti_dma_on() — Активация IOMMU для блокировки PCIe-сканеров.

sys — Anti-Forensics & Identity

    sys.hwid() — Уникальный отпечаток устройства (CPU + Flash + Board).
    sys.trigger_fake_panic(code) — Симуляция 0xC0000005 при детекте IDA/Ghidra.
    sys.unlink_peb() — Скрытие процесса из системных таблиц Windows.

mem — Secure RAM

    mem.set_policy(AUTO_ZEROIZE) — Очистка памяти сразу после выхода из области видимости.
    mem.physical_shred(addr, size) — Физическая перезапись RAM через регистры CPU (xzr).

net — Stealth Networking

    net.raw_inject(packet, mode) — Пробив VPN/Proxy на уровне L2 драйвера.
    net.listen_stealth(port) — Скрытый сокет, невидимый для системных утилит.

4. Конструкции управления (Flow Control)

    spawn isolate Name { ... }: Создает автономную подпрограмму с собственным PID и изолированной кучей памяти.
    scoped_shield(policy) { ... }: Контекстный блок, отключающий любые методы внешнего воздействия (включая программную эмуляцию ввода).
    [x for x in data if condition]: Векторная обработка (SIMD), компилируемая в нативные инструкции процессора.

5. Спецификация файлов v2.0
    Расширение	Слой	Привилегии	Назначение
    .ctos	[SYS]	RING 0 / BIOS	Драйверы, Ядро, BIOS-Shield, Ghost-RAM.
    .vc	[LOGIC]	USER / ISOLATE	Приложения, Асинхронные боты, SQL-логика.
    .web	[VIEW]	UI / SANDBOX	Графический интерфейс управления.
