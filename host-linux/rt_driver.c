#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include "rt_protocol.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intzaar");
MODULE_DESCRIPTION("STM32 Real-Time Bridge");

static dev_t dev_num;
static struct cdev my_cdev;
static struct class *cls;
static struct file *uart_filp = NULL;

static int send_to_stm32(const uint8_t *buf, size_t len) {
    loff_t pos = 0;
    ssize_t bytes_written;
    if (!uart_filp || IS_ERR(uart_filp)) return -EIO;

    bytes_written = kernel_write(uart_filp, buf, len, &pos);
    if (bytes_written < 0) return bytes_written;
    return (bytes_written == len) ? 0 : -EIO;
}

static ssize_t rt_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    struct rt_result k_result;
    loff_t pos = 0;
    ssize_t bytes_read;

    if (!uart_filp || IS_ERR(uart_filp)) return -EIO;

    bytes_read = kernel_read(uart_filp, &k_result, sizeof(struct rt_result), &pos);
    if (bytes_read < 0) return bytes_read;

    if (copy_to_user(buf, &k_result, sizeof(struct rt_result))) {
        return -EFAULT;
    }
    return bytes_read;
}

static long rt_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct rt_task k_task;
    if (cmd == RT_SUBMIT_TASK) {
        if (copy_from_user(&k_task, (struct rt_task __user *)arg, sizeof(struct rt_task))) {
            return -EFAULT;
        }
        return send_to_stm32((uint8_t *)&k_task, sizeof(struct rt_task));
    }
    return -EINVAL;
}

static int rt_open(struct inode *inode, struct file *file) {
    if (!uart_filp) {
        uart_filp = filp_open("/dev/ttyACM0", O_RDWR | O_NOCTTY, 0);
        if (IS_ERR(uart_filp)) return PTR_ERR(uart_filp);
    }
    return 0;
}

static int rt_release(struct inode *inode, struct file *file) {
    if (uart_filp) {
        fput(uart_filp);
        uart_filp = NULL;
    }
    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = rt_open,
    .release = rt_release,
    .read = rt_read,
    .unlocked_ioctl = rt_ioctl,
};

static int __init rt_driver_init(void) {
    if (alloc_chrdev_region(&dev_num, 0, 1, "realtime_exec_dev") < 0) return -1;
    cls = class_create("realtime_class");
    if (IS_ERR(cls)) goto err_class;
    cdev_init(&my_cdev, &fops);
    if (cdev_add(&my_cdev, dev_num, 1) < 0) goto err_cdev;
    if (device_create(cls, NULL, dev_num, NULL, "realtime_exec") == NULL) goto err_dev;
    printk(KERN_INFO "RT_DRIVER: Bridge Ready.\n");
    return 0;

err_dev: cdev_del(&my_cdev);
err_cdev: class_destroy(cls);
err_class: unregister_chrdev_region(dev_num, 1);
    return -1;
}

static void __exit rt_driver_exit(void) {
    device_destroy(cls, dev_num);
    cdev_del(&my_cdev);
    class_destroy(cls);
    unregister_chrdev_region(dev_num, 1);
    printk(KERN_INFO "RT_DRIVER: Unloaded.\n");
}

module_init(rt_driver_init);
module_exit(rt_driver_exit);