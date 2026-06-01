#ifndef RT_PROTOCOL_H
#define RT_PROTOCOL_H

#include <linux/ioctl.h>

#ifdef __KERNEL__
    #include <linux/types.h>
#else
    #include <stdint.h>
#endif

#define RT_MAGIC 'r'

/* Exactly 14 bytes */
struct rt_task {
    uint32_t task_id;
    uint64_t exec_time_us;
    uint8_t opcode;
    uint8_t pin_num;
} __attribute__((packed));

/* Exactly 5 bytes */
struct rt_result {
    uint32_t task_id;
    uint8_t status;
} __attribute__((packed));

#define RT_SUBMIT_TASK _IOW(RT_MAGIC, 1, struct rt_task)
#define OP_TOGGLE_PIN 1

#endif // RT_PROTOCOL_H