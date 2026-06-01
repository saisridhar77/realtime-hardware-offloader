#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include "rt_protocol.h"

uint64_t get_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL);
}

int main() {
    printf("[DAEMON] Opening real-time execution fast-track...\n");
    int fd = open("/dev/realtime_exec", O_RDWR);
    if (fd < 0) {
        perror("[DAEMON] FATAL: Could not open /dev/realtime_exec");
        return EXIT_FAILURE;
    }

    printf("[DAEMON] Bridge online. Beginning Benchmark.\n\n");
    printf("Task ID    | Target Delay      | Round-Trip Time \n");
    printf("---------------------------------------------------------\n");

    uint32_t current_id = 100;
    struct rt_task tight_task;
    struct rt_result result_packet;

    while (1) {
        tight_task.task_id = current_id;
        tight_task.exec_time_us = 500;
        tight_task.opcode = OP_TOGGLE_PIN;
        tight_task.pin_num = 5;

        uint64_t t_start = get_time_us();

        if (ioctl(fd, RT_SUBMIT_TASK, &tight_task) < 0) {
            perror("IOCTL Failed");
            break;
        }

        ssize_t bytes_read = read(fd, &result_packet, sizeof(struct rt_result));
        
        if (bytes_read == sizeof(struct rt_result)) {
            uint64_t t_end = get_time_us();
            uint64_t round_trip = t_end - t_start;

            printf("Task #%-4u | %-4llu            us | %-5llu          us\n", 
                   result_packet.task_id, tight_task.exec_time_us, round_trip);
        } else {
            printf("[DAEMON] Read Error or Desync.\n");
        }

        current_id++;
        usleep(1000000); // Wait 1 second before next task
    }

    close(fd);
    return 0;
}