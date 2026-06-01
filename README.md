# Real-Time Hardware Offloader

A hardware-in-the-loop benchmarking pipeline designed to offload time-sensitive, deterministic tasks from a Linux host system to a dedicated, bare-metal **STM32F4 (ARM Cortex-M4)** microcontroller. 

This project implements a custom Linux character device driver, a cross-architecture synchronized communication protocol, a userspace benchmarking daemon, and low-level firmware utilizing hardware timer interrupts to achieve microsecond-precision task execution.

---

##  System Architecture

The pipeline bypasses standard high-level TTY abstractions to create a low-overhead path from userspace to physical hardware:

1. **Userspace Daemon (`rt_daemon.c`)**: Dispatches timing payloads via custom `IOCTL` system calls and calculates raw round-trip microsecond jitter using `CLOCK_MONOTONIC`.
2. **Linux Kernel Module (`rt_driver.c`)**: Registers a character device (`/dev/realtime_exec`), intercepting `IOCTL` payloads and managing robust kernel-level reads/writes directly to the underlying USB CDC-ACM VFS file structure.
3. **Cross-Architecture Protocol (`rt_protocol.h`)**: Implements strictly packed structural layout constraints (`__attribute__((packed))`) to unify data alignment boundaries between 64-bit host space and 32-bit embedded space.
4. **Bare-Metal Firmware (`main.c`)**: Operates on an STM32F446RE MCU. It parses inbound packets over non-blocking UART Interrupts, triggers hardware timer `TIM2` for deterministic execution microsecond counting, toggles targeted GPIOs, and streams low-overhead execution receipts back to the host.

---

##  Repository Structure

```text
realtime-hardware-offloader/
├── .gitignore
├── README.md
├── host-linux/
│   ├── Makefile                  # Builds kernel module & userspace daemon
│   ├── rt_driver.c               # Custom Linux character device driver
│   ├── rt_daemon.c               # Performance profiling userspace daemon
│   └── rt_protocol.h             # Shared packed structural synchronization layer
└── embedded-stm32/
    ├── Nucleo_Blinky_Real.ioc    # STM32CubeMX hardware configuration file
    └── Core/
        ├── Inc/                  # Embedded header manifests
        └── Src/
            └── main.c            # Bare-metal ISR callbacks & main loop control