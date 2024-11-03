#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>

#define GPIO_BUTTON 72   // GPIO pin for the button
#define DEBOUNCE_DELAY 50 // Debounce delay in milliseconds

static struct timer_list debounce_timer;
static unsigned long last_irq_time = 0;
static int toggle_state = 0;
static int gpio_toggle = -1;
static int irq_number;

static void toggle_gpio(void)
{
    toggle_state = !toggle_state;
    gpio_set_value(gpio_toggle, toggle_state);
    printk(KERN_INFO "GPIO %d toggled to %d\n", gpio_toggle, toggle_state);
}

static void debounce_func(struct timer_list *t)
{
    unsigned long current_time = jiffies;

    // Check if enough time has passed to consider the interrupt valid
    if (time_after(current_time, last_irq_time + msecs_to_jiffies(DEBOUNCE_DELAY))) {
        toggle_gpio();
    }
}

static irqreturn_t gpio_irq_handler(int irq, void *dev_id)
{
    last_irq_time = jiffies; // Update the last interrupt time
    mod_timer(&debounce_timer, jiffies + msecs_to_jiffies(DEBOUNCE_DELAY));
    return IRQ_HANDLED; // Indicate that the interrupt has been handled
}

static int gpio_toggle_probe(struct platform_device *pdev)
{
    int result;
    struct device_node *np = pdev->dev.of_node;

    // Read the GPIO pin from the device tree
    if (of_property_read_u32(np, "pin", &gpio_toggle)) {
        printk(KERN_ERR "Failed to read GPIO pin from device tree\n");
        return -EINVAL;
    }


    // Request GPIOs
    if (!gpio_is_valid(GPIO_BUTTON) || !gpio_is_valid(gpio_toggle)) {
        printk(KERN_ERR "Invalid GPIOs\n");
        return -ENODEV;
    }

    // Set GPIO for the button as input
    gpio_request(GPIO_BUTTON, "GPIO_BUTTON");
    gpio_direction_input(GPIO_BUTTON);

    // Set GPIO for toggle as output
    gpio_request(gpio_toggle, "GPIO_TOGGLE");
    gpio_direction_output(gpio_toggle, 0); // Initialize to low

    // Request IRQ for GPIO_BUTTON
    irq_number = gpio_to_irq(GPIO_BUTTON);
    printk(KERN_INFO "Probed BUTTON, GPIO pin %d assigned to IRQ %d\n", GPIO_BUTTON, irq_number);
    result = request_irq(irq_number, gpio_irq_handler, IRQF_TRIGGER_RISING, "gpio_irq_handler", NULL);
    if (result) {
        printk(KERN_ERR "Failed to request IRQ: %d\n", result);
        gpio_free(GPIO_BUTTON);
        gpio_free(gpio_toggle);
        return result;
    }

    // Initialize timer
    timer_setup(&debounce_timer, debounce_func, 0);
    
    
    return 0; // Module loaded successfully
}

static int gpio_toggle_remove(struct platform_device *pdev)
{
    del_timer(&debounce_timer); // Delete timer
    free_irq(gpio_to_irq(GPIO_BUTTON), NULL); // Free IRQ
    gpio_free(GPIO_BUTTON); // Free GPIOs
    gpio_free(gpio_toggle);
    printk(KERN_INFO "GPIO Toggle Module Exited\n");
    return 0;
}

static const struct of_device_id gpio_toggle_of_match[] = {
    { .compatible = "gpio_device_pump", },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gpio_toggle_of_match);

static struct platform_driver gpio_toggle_driver = {
    .probe = gpio_toggle_probe,
    .remove = gpio_toggle_remove,
    .driver = {
        .name = "gpio_toggle",
        .of_match_table = gpio_toggle_of_match,
    },
};

module_platform_driver(gpio_toggle_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("GPIO Toggle Module");
MODULE_AUTHOR("TOMER");
