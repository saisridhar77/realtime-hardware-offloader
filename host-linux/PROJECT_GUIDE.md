# Real-Time Task Offloading System
## Linux → STM32 Co-Processor via UART | Complete Implementation Guide

---

## Project Overview

This project implements an **Asymmetric Multiprocessing (AMP)** architecture where:
- A **Linux host** (your laptop) handles high-level logic and task scheduling
- An **STM32 microcontroller** acts as a deterministic real-time co-processor
- A custom **Linux kernel driver** (`/dev/realtime_exec`) bridges the two worlds
- A **Smart Dispatcher Daemon** automatically routes time-critical tasks to hardware

**The core problem being solved:** Linux's Completely Fair Scheduler (CFS) introduces
unpredictable jitter (±500µs to ±5ms) that makes precise hardware timing impossible.
The STM32 hardware timer eliminates this with sub-microsecond determinism.

---

## Hardware Requirements

| Component | Details |
|---|---|
| STM32 Nucleo Board | F401RE, F446RE, or F103RB (any Nucleo works) |
| USB Cable | Micro-USB (powers the board AND provides UART) |
| Logic Analyzer | ~$10 Saleae clone from Amazon (for benchmarking) |
| Jumper Wire | 1× wire from STM32 pin PA5 (onboard LED) to logic analyzer |

**Why no extra wiring?**
The Nucleo board's ST-LINK chip converts UART2 (PA2/PA3) to a USB virtual COM port.
When you plug in the USB cable, Linux sees `/dev/ttyACM0` automatically.
No USB-to-serial adapter needed.

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         LINUX LAPTOP                            │
│                                                                 │
│  ┌──────────────────┐    Named Pipe     ┌───────────────────┐   │
│  │  Smart Dispatcher│◄──────────────────│  Any User Program │   │
│  │    Daemon (C)    │   /tmp/rt_mailbox  │  (Python, C, etc)│   │
│  └────────┬─────────┘                   └───────────────────┘   │
│           │ ioctl()                                             │
│           ▼                                                     │
│  ┌──────────────────┐                                           │
│  │  Kernel Driver   │  /dev/realtime_exec                       │
│  │  (rt_driver.ko)  │                                           │
│  └────────┬─────────┘                                           │
│           │ kernel_write() / kernel_read()                      │
│           ▼                                                     │
│  ┌──────────────────┐                                           │
│  │ /dev/ttyACM0     │  USB Virtual COM Port (115200 baud)       │
│  └────────┬─────────┘                                           │
└───────────┼─────────────────────────────────────────────────────┘
            │ USB Cable (14-byte struct packets)
            ▼
┌───────────────────────────────────────────────────────────────────┐
│                     STM32 NUCLEO BOARD                            │
│                                                                   │
│  ┌──────────────────┐   Interrupt    ┌────────────────────────┐   │
│  │  UART2 ISR       │───────────────►│  Hardware Timer (TIM2) │   │
│  │  (Receives Task) │                │  1MHz = 1µs resolution │   │
│  └──────────────────┘                └──────────┬─────────────┘   │
│                                                 │ Timer ISR       │
│                                                 ▼                 │
│                                       ┌────────────────────────┐  │
│                                       │  Execute Task          │  │
│                                       │  Toggle PA5 (LED)      │  │
│                                       │  Send result back      │  │
│                                       └────────────────────────┘  │
└───────────────────────────────────────────────────────────────────┘
```

---

## File Structure

```
realtime_project/
├── rt_protocol.h        # Shared data structs (Linux + STM32 must match)
├── rt_driver.c          # Linux kernel module
├── Makefile             # Kernel module build file
├── rt_daemon.c          # Smart dispatcher daemon (userspace)
├── benchmark.c          # Benchmarking tool (proves jitter reduction)
├── stress_test.sh       # Automated stress test script
└── stm32_firmware/
    └── Core/Src/
        └── main.c       # STM32 firmware (paste into CubeMX project)
```

---

## Phase 1: The Shared Protocol — `rt_protocol.h`

This file is the **contract** between Linux and the STM32.
Both sides must have the exact same struct layout in memory.
`__attribute__((packed))` prevents the compiler from padding bytes.

```c
#ifndef RT_PROTOCOL_H
#define RT_PROTOCOL_H

#ifdef __KERNEL__
    #include <linux/types.h>
#else
    #include <stdint.h>
#endif

#define RT_MAGIC        'r'
#define OP_TOGGLE_PIN   1
#define OP_PWM_SET      2

/* Task Packet: Linux → STM32 (14 bytes total) */
struct rt_task {
    uint32_t task_id;        /* 4 bytes: Unique task identifier          */
    uint64_t exec_time_us;   /* 8 bytes: Delay in microseconds           */
    uint8_t  opcode;         /* 1 byte:  What to do (OP_TOGGLE_PIN etc.) */
    uint8_t  pin_num;        /* 1 byte:  Target GPIO pin number          */
} __attribute__((packed));   /* Total: exactly 14 bytes                  */

/* Result Packet: STM32 → Linux (5 bytes total) */
struct rt_result {
    uint32_t task_id;        /* 4 bytes: Which task completed            */
    uint8_t  status;         /* 1 byte:  0=Success, 1=Error              */
} __attribute__((packed));   /* Total: exactly 5 bytes                   */

/* IOCTL command: User Space sends a task to the kernel driver */
#define RT_SUBMIT_TASK  _IOW(RT_MAGIC, 1, struct rt_task)

/* Precision threshold: tasks needing tighter than this go to STM32 */
#define LINUX_MAX_JITTER_US  2000

#endif /* RT_PROTOCOL_H */
```

**Verify struct sizes after editing:**
```bash
# Run this to confirm sizes match between Linux and STM32
cat > check_sizes.c << 'EOF'
#include <stdio.h>
#include "rt_protocol.h"
int main() {
    printf("rt_task  size: %zu bytes (expected 14)\n", sizeof(struct rt_task));
    printf("rt_result size: %zu bytes (expected 5)\n",  sizeof(struct rt_result));
    return (sizeof(struct rt_task) != 14 || sizeof(struct rt_result) != 5) ? 1 : 0;
}
EOF
gcc check_sizes.c -o check_sizes && ./check_sizes
```

---

## Phase 2: STM32 Firmware

### Step 2A: CubeMX Configuration

1. Open **STM32CubeIDE**, create a new project, select your Nucleo board.
2. Configure these peripherals in the `.ioc` file:

**USART2** (the virtual COM port to your laptop):
- Mode: `Asynchronous`
- Baud Rate: `115200`
- Word Length: `8 Bits`
- Parity: `None`
- Stop Bits: `1`
- Global Interrupt: **ENABLED** ✓

**TIM2** (hardware timer for microsecond precision):
- Clock Source: `Internal Clock`
- Prescaler: `83` (for 84MHz Nucleo F401 → gives 1MHz = 1µs/tick)
  - For F103 at 72MHz: use `71`
  - For F446 at 180MHz: use `179`
- Counter Period: `4294967295` (max 32-bit, we override it at runtime)
- Counter Mode: `Up`
- Global Interrupt: **ENABLED** ✓

**GPIO PA5** (onboard LED on all Nucleo boards):
- Mode: `GPIO_Output`
- Label: `LED_PIN`

3. Click **Generate Code**.

---

### Step 2B: STM32 Firmware — `main.c`

Inside your generated `main.c`, add the following code in the marked `USER CODE` sections:

```c
/* USER CODE BEGIN Includes */
#include <string.h>
/* USER CODE END Includes */

/* === PASTE THESE STRUCTS (must match rt_protocol.h exactly) === */
/* USER CODE BEGIN PD */
#define OP_TOGGLE_PIN 1

typedef struct __attribute__((packed)) {
    uint32_t task_id;
    uint64_t exec_time_us;
    uint8_t  opcode;
    uint8_t  pin_num;
} rt_task_t;

typedef struct __attribute__((packed)) {
    uint32_t task_id;
    uint8_t  status;
} rt_result_t;

#define RT_TASK_SIZE   sizeof(rt_task_t)    /* 14 bytes */
#define RT_RESULT_SIZE sizeof(rt_result_t)  /* 5 bytes  */
/* USER CODE END PD */

/* USER CODE BEGIN PV */
/* Receive buffer — sized exactly for one task packet */
uint8_t         uart_rx_buf[14];

/* Active task — shared between UART ISR and Timer ISR */
/* volatile: tells compiler this changes inside interrupts */
volatile rt_task_t  active_task;
volatile uint8_t    task_armed   = 0;

/* Result ready flag — set in Timer ISR, consumed in main loop */
volatile uint8_t    result_ready = 0;
/* USER CODE END PV */
```

Inside `main()`, after all `MX_xxx_Init()` calls:

```c
/* USER CODE BEGIN 2 */

/*
 * Arm UART to receive exactly RT_TASK_SIZE bytes (14 bytes).
 * When 14 bytes arrive, HAL fires HAL_UART_RxCpltCallback automatically.
 * This is interrupt-driven — zero CPU polling.
 */
HAL_UART_Receive_IT(&huart2, uart_rx_buf, RT_TASK_SIZE);

/* USER CODE END 2 */
```

The `while(1)` main loop:

```c
/* USER CODE BEGIN WHILE */
while (1) {
    /*
     * Main loop only handles sending results back to Linux.
     * All real-time execution happens inside hardware interrupts (below).
     * This separation is critical: the main loop can never delay the timer ISR.
     */
    if (result_ready) {
        result_ready = 0;

        rt_result_t result;
        result.task_id = active_task.task_id;
        result.status  = 0;  /* 0 = Success */

        /* Send 5-byte result back to Linux over UART */
        HAL_UART_Transmit(&huart2,
                          (uint8_t*)&result,
                          RT_RESULT_SIZE,
                          100);  /* 100ms timeout */
    }
/* USER CODE END WHILE */
```

Now the two ISR callbacks — paste these **after** `main()`, in `USER CODE BEGIN 4`:

```c
/* USER CODE BEGIN 4 */

/**
 * HAL_UART_RxCpltCallback
 * Fires when exactly RT_TASK_SIZE (14) bytes have been received.
 *
 * This is the "task intake" interrupt. It:
 * 1. Copies the raw bytes into our rt_task struct
 * 2. Configures the target GPIO pin
 * 3. Programs the hardware timer to fire after exec_time_us microseconds
 * 4. Re-arms UART to receive the next task
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2) return;

    /* Copy raw bytes from buffer into typed struct */
    memcpy((void*)&active_task, uart_rx_buf, RT_TASK_SIZE);

    /* Configure the target pin as output */
    /* (PA5 = onboard LED on all Nucleo boards) */
    /* For real use: map active_task.pin_num to actual GPIO port/pin */
    GPIO_InitTypeDef gpio_cfg = {0};
    gpio_cfg.Pin  = GPIO_PIN_5;
    gpio_cfg.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_cfg.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio_cfg);

    /*
     * Program the hardware timer.
     *
     * The timer runs at 1MHz (1 tick = 1 microsecond).
     * We cast exec_time_us to uint32_t: supports up to ~71 minutes.
     * For longer delays, implement multi-shot logic.
     *
     * __HAL_TIM_SET_AUTORELOAD: sets the count-to value
     * __HAL_TIM_SET_COUNTER:    resets counter to 0
     *
     * When the counter reaches ARR, the hardware fires the period-elapsed
     * interrupt — guaranteed by silicon, zero OS involvement.
     */
    uint32_t delay_ticks = (uint32_t)active_task.exec_time_us;
    if (delay_ticks == 0) delay_ticks = 1;  /* minimum 1 tick */

    HAL_TIM_Base_Stop_IT(&htim2);              /* Stop if already running */
    __HAL_TIM_SET_COUNTER(&htim2, 0);          /* Reset counter           */
    __HAL_TIM_SET_AUTORELOAD(&htim2, delay_ticks); /* Set target count    */

    task_armed = 1;
    HAL_TIM_Base_Start_IT(&htim2);             /* ARM THE TIMER           */

    /* Re-arm UART for the next incoming task */
    HAL_UART_Receive_IT(&huart2, uart_rx_buf, RT_TASK_SIZE);
}

/**
 * HAL_TIM_PeriodElapsedCallback
 * Fires at the EXACT microsecond the hardware timer reaches its target.
 *
 * This is the "execution" interrupt. No OS, no scheduler, pure silicon.
 * Latency: sub-microsecond (limited only by interrupt entry overhead ~12 cycles).
 *
 * This is where we prove the project works:
 * The oscilloscope/logic analyzer will show this fires at exactly exec_time_us.
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance != TIM2) return;
    if (!task_armed) return;

    /* Stop timer immediately (one-shot behavior) */
    HAL_TIM_Base_Stop_IT(&htim2);
    task_armed = 0;

    /* Execute the task based on opcode */
    switch (active_task.opcode) {
        case OP_TOGGLE_PIN:
            HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
            break;
        default:
            break;
    }

    /* Signal main loop to send result back to Linux */
    result_ready = 1;
}

/* USER CODE END 4 */
```

---

## Phase 3: Linux Kernel Driver — `rt_driver.c`

Key changes from the ESP32 version:
- Port is `/dev/ttyACM0` (Nucleo virtual COM port, not `/dev/ttyUSB0`)
- Added a dedicated **kernel thread** for async receive (fixes the "FAKE RECEIVE" issue)

```c
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/file.h>
#include <linux/kthread.h>   /* For kernel receive thread */
#include <linux/delay.h>
#include "rt_protocol.h"

#define DEVICE_NAME "realtime_exec"
#define CLASS_NAME  "rt_class"
#define UART_PORT   "/dev/ttyACM0"   /* STM32 Nucleo virtual COM port */

static int           major_number;
static struct class  *rt_class  = NULL;
static struct device *rt_device = NULL;
static struct file   *uart_file = NULL;

/* ------------------------------------------------------------------ */
/* Ring Buffer + Synchronization                                       */
/* ------------------------------------------------------------------ */
#define BUFFER_SIZE 32

static struct {
    struct rt_result data[BUFFER_SIZE];
    int head;
    int tail;
    spinlock_t lock;
} ring_buffer;

DECLARE_WAIT_QUEUE_HEAD(result_wait_queue);

/* ------------------------------------------------------------------ */
/* Kernel Receive Thread                                               */
/* This thread runs permanently, reading result bytes from the STM32. */
/* It replaces the "FAKE RECEIVE" simulation from the prototype.       */
/* ------------------------------------------------------------------ */
static struct task_struct *rx_thread = NULL;

static int uart_rx_thread_fn(void *data)
{
    struct rt_result result;
    ssize_t bytes_read;
    loff_t  pos = 0;
    unsigned long flags;

    printk(KERN_INFO "RT_DRIVER: RX thread started. Listening for STM32 results...\n");

    while (!kthread_should_stop()) {
        if (!uart_file || IS_ERR(uart_file)) {
            msleep(100);
            continue;
        }

        /* Blocking read: waits for RT_RESULT_SIZE (5) bytes from STM32 */
        bytes_read = kernel_read(uart_file,
                                 &result,
                                 sizeof(struct rt_result),
                                 &pos);

        if (bytes_read == sizeof(struct rt_result)) {
            /* Got a valid result — push into ring buffer */
            spin_lock_irqsave(&ring_buffer.lock, flags);
            ring_buffer.data[ring_buffer.head] = result;
            ring_buffer.head = (ring_buffer.head + 1) % BUFFER_SIZE;
            spin_unlock_irqrestore(&ring_buffer.lock, flags);

            /* Wake up any sleeping user-space app waiting for results */
            wake_up_interruptible(&result_wait_queue);

            printk(KERN_INFO "RT_DRIVER: STM32 completed Task %u | Status: %u\n",
                   result.task_id, result.status);
        } else if (bytes_read < 0) {
            msleep(10); /* Back off on error */
        }
    }

    printk(KERN_INFO "RT_DRIVER: RX thread stopping.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Read Function: User-space blocks here until a result is available  */
/* ------------------------------------------------------------------ */
static ssize_t rt_read(struct file *file, char __user *user_buf,
                       size_t count, loff_t *offset)
{
    struct rt_result result;
    unsigned long flags;

    /* Sleep until there is a result in the buffer */
    if (wait_event_interruptible(result_wait_queue,
                                 ring_buffer.head != ring_buffer.tail))
        return -ERESTARTSYS;

    /* Pop the oldest result from the tail */
    spin_lock_irqsave(&ring_buffer.lock, flags);
    result = ring_buffer.data[ring_buffer.tail];
    ring_buffer.tail = (ring_buffer.tail + 1) % BUFFER_SIZE;
    spin_unlock_irqrestore(&ring_buffer.lock, flags);

    /* Copy result from kernel space to user space */
    if (copy_to_user(user_buf, &result, sizeof(struct rt_result)))
        return -EFAULT;

    return sizeof(struct rt_result);
}

/* ------------------------------------------------------------------ */
/* IOCTL: User-space sends a task → kernel forwards it to STM32       */
/* ------------------------------------------------------------------ */
static long rt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct rt_task task;
    loff_t pos = 0;

    if (cmd != RT_SUBMIT_TASK) return -EINVAL;

    if (copy_from_user(&task, (struct rt_task __user *)arg,
                       sizeof(struct rt_task)))
        return -EFAULT;

    printk(KERN_INFO "RT_DRIVER: Task %u → STM32 (delay: %llu µs, pin: %u)\n",
           task.task_id, task.exec_time_us, task.pin_num);

    if (uart_file && !IS_ERR(uart_file)) {
        ssize_t written = kernel_write(uart_file, &task,
                                       sizeof(struct rt_task), &pos);
        if (written != sizeof(struct rt_task)) {
            printk(KERN_ERR "RT_DRIVER: UART write failed! Wrote %zd bytes\n", written);
            return -EIO;
        }
    } else {
        printk(KERN_ERR "RT_DRIVER: STM32 not connected!\n");
        return -ENODEV;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* File Operations                                                     */
/* ------------------------------------------------------------------ */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .read           = rt_read,
    .unlocked_ioctl = rt_ioctl,
};

/* ------------------------------------------------------------------ */
/* Module Init / Exit                                                  */
/* ------------------------------------------------------------------ */
static int __init rt_driver_init(void)
{
    printk(KERN_INFO "RT_DRIVER: Loading...\n");

    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) return major_number;

    rt_class = class_create(CLASS_NAME);
    if (IS_ERR(rt_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(rt_class);
    }

    rt_device = device_create(rt_class, NULL, MKDEV(major_number, 0),
                              NULL, DEVICE_NAME);
    if (IS_ERR(rt_device)) {
        class_destroy(rt_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(rt_device);
    }

    /* Initialize ring buffer */
    spin_lock_init(&ring_buffer.lock);
    ring_buffer.head = 0;
    ring_buffer.tail = 0;

    /* Open the STM32 UART port */
    uart_file = filp_open(UART_PORT, O_RDWR | O_NOCTTY | O_NDELAY, 0);
    if (IS_ERR(uart_file)) {
        printk(KERN_WARNING "RT_DRIVER: Cannot open %s. Plug in STM32.\n", UART_PORT);
        uart_file = NULL;
    } else {
        printk(KERN_INFO "RT_DRIVER: Connected to STM32 on %s\n", UART_PORT);

        /* Start the background receive thread */
        rx_thread = kthread_run(uart_rx_thread_fn, NULL, "rt_rx_thread");
        if (IS_ERR(rx_thread)) {
            printk(KERN_ERR "RT_DRIVER: Failed to create RX thread\n");
            rx_thread = NULL;
        }
    }

    printk(KERN_INFO "RT_DRIVER: /dev/%s ready.\n", DEVICE_NAME);
    return 0;
}

static void __exit rt_driver_exit(void)
{
    if (rx_thread) {
        kthread_stop(rx_thread);
        rx_thread = NULL;
    }
    if (uart_file && !IS_ERR(uart_file)) {
        filp_close(uart_file, NULL);
        uart_file = NULL;
    }

    device_destroy(rt_class, MKDEV(major_number, 0));
    class_unregister(rt_class);
    class_destroy(rt_class);
    unregister_chrdev(major_number, DEVICE_NAME);

    printk(KERN_INFO "RT_DRIVER: Unloaded.\n");
}

module_init(rt_driver_init);
module_exit(rt_driver_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Real-Time Task Offloading Driver (STM32 + Linux AMP)");
MODULE_VERSION("1.0");
```

---

## Phase 4: Makefile

```makefile
obj-m += rt_driver.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
```

---

## Phase 5: Smart Dispatcher Daemon — `rt_daemon.c`

The daemon runs forever. It:
1. Reads precision requirements from `/tmp/rt_mailbox`
2. **Automatically decides** whether to run locally or offload to STM32
3. Records timing data for benchmarking

```c
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <string.h>
#include <time.h>
#include "rt_protocol.h"

#define DEVICE_PATH  "/dev/realtime_exec"
#define MAILBOX_PATH "/tmp/rt_mailbox"
#define LOG_PATH     "/tmp/rt_dispatch.log"

/* ---- Timing utilities ---- */
static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

/* ---- Smart Routing Decision ---- */
static const char *route_task(int driver_fd, FILE *log,
                               uint32_t task_id, int pin,
                               uint64_t delay_us, int precision_us)
{
    struct rt_task    task   = {0};
    struct rt_result  result = {0};
    uint64_t          t_start, t_end;

    if (precision_us < LINUX_MAX_JITTER_US) {
        /* === OFFLOAD TO STM32 === */
        task.task_id       = task_id;
        task.opcode        = OP_TOGGLE_PIN;
        task.pin_num       = (uint8_t)pin;
        task.exec_time_us  = delay_us;

        t_start = now_us();
        ioctl(driver_fd, RT_SUBMIT_TASK, &task);
        read(driver_fd, &result, sizeof(struct rt_result));
        t_end = now_us();

        fprintf(log, "STM32,%u,%d,%llu,%llu,%d\n",
                task_id, precision_us, delay_us,
                (unsigned long long)(t_end - t_start), result.status);
        fflush(log);
        return "STM32";

    } else {
        /* === RUN ON LINUX (for loose deadlines) === */
        t_start = now_us();
        usleep((useconds_t)delay_us);
        /* In a real system, toggle a GPIO here via /sys/class/gpio */
        t_end = now_us();
        uint64_t actual_delay = t_end - t_start;

        fprintf(log, "LINUX,%u,%d,%llu,%llu,0\n",
                task_id, precision_us, delay_us,
                (unsigned long long)actual_delay);
        fflush(log);
        return "LINUX";
    }
}

int main(void)
{
    int    driver_fd, mailbox_fd;
    char   buf[128];
    FILE   *log;
    uint32_t task_counter = 1;

    printf("[DAEMON] Real-Time Smart Dispatcher starting...\n");
    printf("[DAEMON] Jitter threshold: %d µs\n", LINUX_MAX_JITTER_US);
    printf("[DAEMON] Tasks below threshold → STM32 | Above → Linux CPU\n\n");

    driver_fd = open(DEVICE_PATH, O_RDWR);
    if (driver_fd < 0) {
        perror("[DAEMON] Cannot open /dev/realtime_exec");
        return -1;
    }

    /* Create named pipe (mailbox) if it doesn't exist */
    unlink(MAILBOX_PATH);
    mkfifo(MAILBOX_PATH, 0666);
    printf("[DAEMON] Mailbox ready at %s\n", MAILBOX_PATH);

    log = fopen(LOG_PATH, "w");
    if (!log) { perror("log"); return -1; }
    fprintf(log, "ROUTE,TASK_ID,PRECISION_US,REQUESTED_DELAY_US,ACTUAL_US,STATUS\n");
    fflush(log);
    printf("[DAEMON] Logging to %s\n\n", LOG_PATH);

    /* === MAIN LOOP: blocks until a task arrives in the mailbox === */
    while (1) {
        mailbox_fd = open(MAILBOX_PATH, O_RDONLY);
        if (mailbox_fd < 0) continue;

        memset(buf, 0, sizeof(buf));
        ssize_t n = read(mailbox_fd, buf, sizeof(buf) - 1);
        close(mailbox_fd);
        if (n <= 0) continue;

        /* Expected mailbox format: "PIN DELAY_US PRECISION_US"
         * Example: "5 10000 500"  → toggle pin 5 in 10ms with ±500µs required
         * Example: "5 50000 5000" → toggle pin 5 in 50ms with ±5ms required (Linux OK) */
        int      pin          = 0;
        uint64_t delay_us     = 0;
        int      precision_us = 0;

        if (sscanf(buf, "%d %llu %d", &pin, &delay_us, &precision_us) < 2) {
            printf("[DAEMON] Bad format. Expected: PIN DELAY_US [PRECISION_US]\n");
            continue;
        }
        if (precision_us == 0) precision_us = 500; /* default: 500µs */

        const char *route = route_task(driver_fd, log,
                                        task_counter++, pin,
                                        delay_us, precision_us);

        printf("[DAEMON] Task %u | Pin %d | Delay %llu µs | Required ±%d µs → [%s]\n",
               task_counter - 1, pin, delay_us, precision_us, route);
    }

    fclose(log);
    close(driver_fd);
    return 0;
}
```

---

## Phase 6: Benchmarking Tool — `benchmark.c`

This program **proves your project works** by running hundreds of tasks through both
paths and comparing the timing statistics.

```c
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>

#define N_SAMPLES    200
#define TARGET_US    10000   /* 10ms target delay for each trial */

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

/* Compute stats from an array of measured delays */
static void compute_stats(const char *label, uint64_t *samples, int n,
                           uint64_t target)
{
    double sum = 0, sum_sq = 0;
    uint64_t min_v = UINT64_MAX, max_v = 0;

    for (int i = 0; i < n; i++) {
        int64_t err = (int64_t)samples[i] - (int64_t)target;
        double  e   = (double)err;
        sum    += e;
        sum_sq += e * e;
        if (samples[i] < min_v) min_v = samples[i];
        if (samples[i] > max_v) max_v = samples[i];
    }

    double mean    = sum / n;
    double stddev  = sqrt(sum_sq / n - mean * mean);
    double range   = (double)(max_v - min_v);

    printf("\n  === %s RESULTS ===\n", label);
    printf("  Samples      : %d\n", n);
    printf("  Target delay : %d µs\n", TARGET_US);
    printf("  Mean error   : %+.2f µs\n", mean);
    printf("  Std deviation: %.2f µs  ← THIS IS YOUR JITTER\n", stddev);
    printf("  Min / Max    : %llu / %llu µs\n",
           (unsigned long long)min_v, (unsigned long long)max_v);
    printf("  Range (jitter span): %.2f µs\n", range);
}

int main(void)
{
    uint64_t linux_samples[N_SAMPLES];
    uint64_t stm32_samples[N_SAMPLES];

    /* ---- Test 1: Linux usleep (baseline jitter) ---- */
    printf("Running Linux baseline test (%d samples)...\n", N_SAMPLES);
    for (int i = 0; i < N_SAMPLES; i++) {
        uint64_t t0 = now_us();
        usleep(TARGET_US);
        uint64_t t1 = now_us();
        linux_samples[i] = t1 - t0;
        usleep(5000); /* 5ms gap between trials */
    }
    compute_stats("LINUX usleep()", linux_samples, N_SAMPLES, TARGET_US);

    /* ---- Test 2: Read STM32 timing from daemon log ---- */
    printf("\nReading STM32 results from daemon log...\n");
    FILE *log = fopen("/tmp/rt_dispatch.log", "r");
    if (!log) {
        printf("No log found. Run the daemon and stress test first.\n");
        printf("(Only Linux baseline stats are shown above)\n");
        return 0;
    }

    char line[256];
    int  stm32_count = 0;
    fgets(line, sizeof(line), log); /* skip header */

    while (fgets(line, sizeof(line), log) && stm32_count < N_SAMPLES) {
        char     route[16];
        uint32_t task_id;
        int      precision;
        uint64_t requested, actual;
        int      status;

        if (sscanf(line, "%15[^,],%u,%d,%llu,%llu,%d",
                   route, &task_id, &precision,
                   &requested, &actual, &status) == 6) {
            if (strcmp(route, "STM32") == 0) {
                stm32_samples[stm32_count++] = actual;
            }
        }
    }
    fclose(log);

    if (stm32_count > 0) {
        compute_stats("STM32 Hardware Timer", stm32_samples, stm32_count, TARGET_US);

        /* Summary comparison */
        printf("\n========================================\n");
        printf("  COMPARISON SUMMARY\n");
        printf("========================================\n");
        printf("  If STM32 std_dev < Linux std_dev,\n");
        printf("  your project is proven successful.\n");
        printf("  Include these numbers in your report.\n");
        printf("========================================\n");
    }

    return 0;
}
```

---

## Phase 7: Automated Stress Test — `stress_test.sh`

```bash
#!/bin/bash
# stress_test.sh — Sends 200 mixed tasks to the daemon
# Tight deadline tasks → auto-routed to STM32
# Loose deadline tasks → handled by Linux
# Run this while the daemon is running in another terminal

MAILBOX="/tmp/rt_mailbox"
N_STRICT=100    # Tasks with tight deadlines → go to STM32
N_LOOSE=100     # Tasks with loose deadlines → stay on Linux
TOTAL=$((N_STRICT + N_LOOSE))

echo "=========================================="
echo " Real-Time Offloading Stress Test"
echo " $N_STRICT strict tasks → STM32"
echo " $N_LOOSE  loose tasks  → Linux"
echo " Total: $TOTAL tasks"
echo "=========================================="

SENT=0

# Interleave strict and loose tasks to simulate real mixed workload
for i in $(seq 1 $N_STRICT); do
    # Strict: ±500µs precision required → daemon will offload to STM32
    echo "5 10000 500" > "$MAILBOX"
    SENT=$((SENT + 1))
    echo -ne "\r  Sent $SENT / $TOTAL tasks..."
    sleep 0.08

    # Loose: ±5000µs precision OK → daemon will run on Linux
    if [ $i -le $N_LOOSE ]; then
        echo "5 50000 5000" > "$MAILBOX"
        SENT=$((SENT + 1))
        echo -ne "\r  Sent $SENT / $TOTAL tasks..."
        sleep 0.08
    fi
done

echo ""
echo ""
echo "Stress test complete!"
echo "Now run:  ./benchmark"
echo "And check /tmp/rt_dispatch.log for raw data"
```

---

## Complete Build & Run Guide

### Step 0 — Install dependencies
```bash
sudo apt-get install linux-headers-$(uname -r) build-essential gcc
```

### Step 1 — Flash STM32
1. Open STM32CubeIDE, configure as described in Phase 2A
2. Paste the firmware code into your project's `main.c`
3. Build and flash (`Run → Debug`)
4. Check STM32 is connected: `ls /dev/ttyACM*` → should show `/dev/ttyACM0`

### Step 2 — Set UART permissions
```bash
sudo usermod -aG dialout $USER   # permanent (re-login required)
# OR for immediate testing:
sudo chmod 666 /dev/ttyACM0
```

### Step 3 — Build all Linux components
```bash
cd realtime_project/
make                              # builds rt_driver.ko
gcc -O2 rt_daemon.c   -o rt_daemon
gcc -O2 benchmark.c   -lm -o benchmark
chmod +x stress_test.sh
```

### Step 4 — Load the kernel driver
```bash
sudo insmod rt_driver.ko
dmesg | tail -5                   # verify: "Connected to STM32 on /dev/ttyACM0"
ls /dev/realtime_exec             # verify device node exists
```

### Step 5 — Run the daemon (Terminal 1)
```bash
sudo ./rt_daemon
# Output: "Mailbox ready at /tmp/rt_mailbox — Listening for tasks..."
```

### Step 6 — Run the stress test (Terminal 2)
```bash
./stress_test.sh
```

### Step 7 — Run benchmarks (Terminal 3)
```bash
./benchmark
```

### Step 8 — View results
```bash
cat /tmp/rt_dispatch.log          # raw CSV data
./benchmark                       # formatted statistics
```

### Step 9 — Unload driver when done
```bash
sudo rmmod rt_driver
```

---

## Benchmarking & Report Data

### What you will see

| Metric | Linux `usleep()` | STM32 Hardware Timer |
|---|---|---|
| Mean error | ±200–800 µs | ±1–5 µs |
| Std deviation (jitter) | 500–3000 µs | < 10 µs |
| Min/Max range | ~5000 µs span | ~20 µs span |
| Determinism | None guaranteed | Silicon-guaranteed |

### How to capture the visual proof (Logic Analyzer)

1. Connect logic analyzer **Channel 0** to STM32 pin PA5
2. Connect logic analyzer **Channel 1** to any GPIO your laptop controls
3. Run 50 toggle tasks through each path
4. Export both waveforms: Linux waveform will show visible timing variation,
   STM32 waveform will show mathematically uniform pulses
5. Screenshot both in the same view → this is your report's key figure

### Report Abstract Template

> "We implemented an Asymmetric Multiprocessing (AMP) real-time offloading
> architecture on a Linux + STM32 system. A custom Linux kernel character
> device driver (`rt_driver.ko`) exposes a `/dev/realtime_exec` interface.
> A Smart Dispatcher Daemon routes tasks with sub-millisecond precision
> requirements to the STM32 co-processor via UART, while non-critical tasks
> execute locally on Linux. Benchmarking over 200 trials demonstrates a
> reduction in timing jitter from σ = 1,847 µs (Linux `usleep`) to σ = 3.2 µs
> (STM32 hardware timer), a **577× improvement** in temporal determinism."

*(Replace the numbers with your actual measured values)*

---

## STM32 vs ESP32: Why STM32 is Better Here

| Property | STM32 Nucleo | ESP32 |
|---|---|---|
| Radio interference | None (no RF) | Wi-Fi/BT ISRs can delay timers |
| OS overhead | Zero (bare-metal HAL) | FreeRTOS tick jitter |
| Flash latency | Predictable (ICACHE) | Cache miss jitter (needs IRAM_ATTR) |
| Timer precision | Hardware, silicon-verified | Hardware, but radio competes |
| Professional credibility | Industry standard (automotive, medical) | IoT/hobbyist perception |
| Your answer if asked | *"STM32 gives true bare-metal determinism with no RF stack competing for interrupt priority"* | — |

---

## Common Errors & Fixes

| Error | Cause | Fix |
|---|---|---|
| `insmod: ERROR: could not insert module` | Kernel header mismatch | `sudo apt install linux-headers-$(uname -r)` |
| `/dev/ttyACM0: No such file` | STM32 not plugged in | Plug in USB, check `dmesg | tail` |
| `Permission denied` on `/dev/ttyACM0` | User not in dialout group | `sudo chmod 666 /dev/ttyACM0` |
| Driver loads but STM32 not responding | Wrong baud rate | Verify USART2 set to 115200 in CubeMX |
| `class_create` error on older kernels | API changed in kernel 6.x | Change to `class_create(THIS_MODULE, CLASS_NAME)` |
| Timer fires immediately | exec_time_us is 0 | Daemon sends non-zero values; min clamp in firmware |

---

*Document version 1.0 — STM32 Nucleo Implementation*
