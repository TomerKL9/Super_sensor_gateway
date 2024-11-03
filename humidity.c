#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#define TIME_SLOT_MS 50
#define DEBOUNCE_TIME_MS 200 // Debounce time in milliseconds

static int irq_number;
static char value;
static int changed_value = 0; // Flag to indicate if the value has changed
static struct class* gpio_class = NULL;
static struct device* gpio_device = NULL;
static struct workqueue_struct *wq;
static struct work_struct work;
static unsigned long last_interrupt_time = 0;
static int GPIO_PIN; // Declare GPIO_PIN without initialization

static void work_handler(struct work_struct *work) {
    int i;
    char bits[4] = {0};
    struct timespec64 start_time, end_time;
    s64 elapsed_time_ms;

    ktime_get_real_ts64(&start_time); // Get the current time

    for (i = 0; i < 4; i++) {       
        bits[i] = gpio_get_value(GPIO_PIN); // Read the GPIO value
        //printk(KERN_INFO "humidity GPIO bit %d : %c\n", i, bits[i] ? '1' : '0');
        msleep(TIME_SLOT_MS); // Wait for each subsequent time slot
    }

    ktime_get_real_ts64(&end_time); // Get the end time
    elapsed_time_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 +
                      (end_time.tv_nsec - start_time.tv_nsec) / 1000000;

    value = (bits[3] << 3) | (bits[2] << 2) | (bits[1] << 1) | bits[0];
    changed_value = 1; // Set the changed_value flag
    printk(KERN_INFO "Humidity value: %d\n", value);
}

static irqreturn_t gpio_irq_handler(int irq, void *dev_id) {
    unsigned long current_time = jiffies;
    unsigned long time_diff = current_time - last_interrupt_time;

    if (time_diff < msecs_to_jiffies(DEBOUNCE_TIME_MS)) {
        return IRQ_HANDLED; // Ignore the interrupt if it's within the debounce period
    }

    last_interrupt_time = current_time;
    queue_work(wq, &work); // Schedule work to be done in a thread context
    return IRQ_HANDLED;
}

static ssize_t value_show(struct device* dev, struct device_attribute* attr, char* buf) {
    return sprintf(buf, "%d\n", value);
}

static ssize_t changed_value_show(struct device* dev, struct device_attribute* attr, char* buf) {
    return sprintf(buf, "%d\n", changed_value);
}

static ssize_t changed_value_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
    if (buf[0] == '0') {
        changed_value = 0; // Clear the flag if user writes '0'
    }
    return count; // Return count to indicate number of bytes written
}

static DEVICE_ATTR(value, 0444, value_show, NULL);
static DEVICE_ATTR(changed_value, 0644, changed_value_show, changed_value_store);

static int gpio_humidity_probe(struct platform_device *pdev) {
    struct device *dev = &pdev->dev;
    int result;

    // Get GPIO pin from device tree
    result = of_property_read_u32(dev->of_node, "pin", &GPIO_PIN);
    if (result) {
        dev_err(dev, "Failed to get GPIO pin from device tree\n");
        return result;
    }

    // Get device name from device tree
    const char *device_name;
    result = of_property_read_string(dev->of_node, "device-name", &device_name);
    if (result) {
        dev_err(dev, "Failed to get device name from device tree\n");
        return result;
    }

    // Get class name from device tree
    const char *class_name;
    result = of_property_read_string(dev->of_node, "class-name", &class_name);
    if (result) {
        dev_err(dev, "Failed to get class name from device tree\n");
        return result;
    }

    // Create class
    gpio_class = class_create(THIS_MODULE, class_name);
    if (IS_ERR(gpio_class)) {
        dev_err(dev, "Failed to create class\n");
        return PTR_ERR(gpio_class);
    }

    // Create device
    gpio_device = device_create(gpio_class, NULL, 0, NULL, device_name);
    if (IS_ERR(gpio_device)) {
        class_destroy(gpio_class);
        dev_err(dev, "Failed to create device\n");
        return PTR_ERR(gpio_device);
    }

    result = device_create_file(gpio_device, &dev_attr_value);
    if (result) {
        device_destroy(gpio_class, 0);
        class_destroy(gpio_class);
        dev_err(dev, "Failed to create device file\n");
        return result;
    }

    result = device_create_file(gpio_device, &dev_attr_changed_value);
    if (result) {
        device_remove_file(gpio_device, &dev_attr_value);
        device_destroy(gpio_class, 0);
        class_destroy(gpio_class);
        dev_err(dev, "Failed to create changed_value device file\n");
        return result;
    }

    gpio_request(GPIO_PIN, "sysfs");
    gpio_direction_input(GPIO_PIN);
    irq_number = gpio_to_irq(GPIO_PIN);
    printk(KERN_INFO "Probed, GPIO pin %d assigned to IRQ %d\n", GPIO_PIN, irq_number);
    result = request_irq(irq_number, gpio_irq_handler, IRQF_TRIGGER_FALLING, "gpio_humidity_handler", NULL);
    if (result < 0) {
        dev_err(dev, "Failed to request IRQ %d: %d\n", irq_number, result);
        gpio_free(GPIO_PIN);
        class_destroy(gpio_class);
        return result;
    }
    
    wq = create_singlethread_workqueue("gpio_humidity_wq");
    INIT_WORK(&work, work_handler);

    return 0;
}

static int gpio_humidity_remove(struct platform_device *pdev) {
    printk(KERN_INFO "Freeing GPIO pin %d and IRQ %d\n", GPIO_PIN, irq_number);
    free_irq(irq_number, NULL);
    gpio_free(GPIO_PIN);
    destroy_workqueue(wq);
    device_remove_file(gpio_device, &dev_attr_changed_value);
    device_remove_file(gpio_device, &dev_attr_value);
    device_destroy(gpio_class, 0);
    class_unregister(gpio_class);
    class_destroy(gpio_class);
    return 0;
}

static const struct of_device_id gpio_humidity_of_match[] = {
    { .compatible = "gpio_device_humidity", },
    {},
};
MODULE_DEVICE_TABLE(of, gpio_humidity_of_match);

static struct platform_driver gpio_humidity_driver = {
    .driver = {
        .name = "gpio_humidity_driver",
        .of_match_table = gpio_humidity_of_match,
    },
    .probe = gpio_humidity_probe,
    .remove = gpio_humidity_remove,
};

module_platform_driver(gpio_humidity_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TOMER");
MODULE_DESCRIPTION("A GPIO interrupt kernel module for sensor");
MODULE_VERSION("0.1");
