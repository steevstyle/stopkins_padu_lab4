#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>

/* Traffic Light module: mytraffic */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Paul Adu, Steven Hopkins");
MODULE_DESCRIPTION("Linux traffic light module");

/* Defintions for GPIO pins */
#define RED 67
#define YELLOW 68
#define GREEN 44
#define TOGGLE_BTN 26 /* To switch modes...to be implemented later */
#define PED_BTN 46 /* Pedestrian crossing button */

/* TRAFFIC LIGHT MODES:
    Normal: Green -> Green -> Green -> Yellow -> Red -> Red ->
    Flashing Red: Red -> Off -> Red -> Off ->
    FLashing Yellow: Yellow -> Off -> Yellow -> Off ->
*/
enum mode {
    NORMAL,
    FLASHING_RED,
    FLASHING_YELLOW
};

/* Traffic Light State */
struct traffic_light {
    enum mode current_mode;
    struct timer_list timer;
    int cycle_count;
    int ped_cycle_count;
    unsigned int rate;
    bool red_active;
    bool yellow_active;
    bool green_active;
    bool ped_requested; // Pedestrian crossing requested
    bool ped_crossing; // Pedestrian crossing in progress
    int irq_toggle; // Interrupt request number
    int irq_ped;
};

/* Global variables */
static int mytraffic_major = 61;
static struct traffic_light *t_light;

/* Function Declarations */
static int mytraffic_init(void);
static void mytraffic_exit(void);
static int mytraffic_open(struct inode *inode, struct file *filp);
static ssize_t mytraffic_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static int mytraffic_release(struct inode *inode, struct file *filp);
static ssize_t mytraffic_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
static void timer_callback(struct timer_list *t);

/* File operations */
/* Additional feature: add mytimer_write to write new rate from user space */
static struct file_operations mytraffic_fops = {
    owner: THIS_MODULE,
    read: mytraffic_read,
    write: mytraffic_write,
    open: mytraffic_open,
    release: mytraffic_release
};

/* Init/Exit functions */
module_init(mytraffic_init);
module_exit(mytraffic_exit);

static int mytraffic_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int mytraffic_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/* mytraffic_read (below) NEEDS TO BE COMPLETED per LAB REQUIREMENTS */
/* Readable Character Device:

    Your module should set up a character device at /dev/mytraffic that returns the following information when read:
    (e.g., using cat /dev/mytraffic):
        Current operational mode (i.e., “normal”, “flashing-red”, or “flashing-yellow”)
        Current cycle rate (e.g., “1 Hz”, “2 Hz”, etc.)
        Current status of each light (e.g., “red off, yellow off, green on”)
        Whether or not a pedestrian is “present” (i.e., currently crossing or waiting to cross after pressing the call button)
 */
static ssize_t mytraffic_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
	    char kbuf[256];
    int len = 0;
    int result;

    /* Prepare status string */
    len += snprintf(kbuf + len, sizeof(kbuf) - len,
                    "Mode: %s\n",
                    (t_light->current_mode == NORMAL) ? "normal" :
                    (t_light->current_mode == FLASHING_RED) ? "flashing-red" : "flashing-yellow");
    len += snprintf(kbuf + len, sizeof(kbuf) - len,
                    "Cycle Rate: %u Hz\n", t_light->rate);
    len += snprintf(kbuf + len, sizeof(kbuf) - len,
                    "Lights: red %s, yellow %s, green %s\n",
                    t_light->red_active ? "on" : "off",
                    t_light->yellow_active ? "on" : "off",
                    t_light->green_active ? "on" : "off");
    len += snprintf(kbuf + len, sizeof(kbuf) - len,
                    "Pedestrian: %s\n",
                    (t_light->ped_requested || t_light->ped_crossing) ? "present" : "not present");

    /* Handle read offset */
    if (*f_pos >= len) {
        return 0; // EOF
    }

    /* Adjust count if needed */
    if (count > len - *f_pos) {
        count = len - *f_pos;
    }

    /* Transfer data to user space */
    if (copy_to_user(buf, kbuf + *f_pos, count)) {
        return -EFAULT;
    }

    *f_pos += count;
    result = count;
    return result;
}

static ssize_t mytraffic_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    /* Allow user to write new rate as a string representing an integer
       Valid values between 1 and 9, inclusive */
    char kbuf[16];
    unsigned int new_rate;
    if (count >= sizeof(kbuf)) {
        return -EINVAL; // Input too long
    }
    if (copy_from_user(kbuf, buf, count)) {
        return -EFAULT;
    }
    kbuf[count] = '\0';
    if (sscanf(kbuf, "%u", &new_rate) == 1) {
        if (new_rate >= 1 && new_rate <= 9) {
            t_light->rate = new_rate;
            printk(KERN_INFO "mytraffic: Cycle rate updated to %u Hz\n", new_rate);
        }
    }
    return count;
}

/* Timer callback function */
static void timer_callback(struct timer_list *t)
{
    struct traffic_light *light = from_timer(light, t, timer);
    int cycle_position; // Position in the current cycle

    /* NORMAL OPERATION */
    light->cycle_count++;

    switch (light->current_mode) {
        case NORMAL:
            /* First check for active pedestrian crossing. If true, ped mode for 5 cycles */
            if (light->ped_crossing) {
                /* Logic for pedestrian mode: Red & Yellow lit for 5 cycles */
                if (light->ped_cycle_count < 5) {
                    light->red_active = true;
                    light->yellow_active = true;
                    light->green_active = false;
                    light->ped_cycle_count++;
                    printk(KERN_INFO "mytraffic: Pedestrian crossing in progress (%d/5)\n", light->ped_cycle_count);

                } else { /* Pedestrian Crossing completion -> back to Green */
                    light->ped_crossing = false;
                    light->ped_requested = false;
                    light->ped_cycle_count = 0;
                    light->cycle_count = 0; // Reset cycle to beginning
                    printk(KERN_INFO "mytraffic: Pedestrian crossing complete, resuming normal operation\n");
                    light->red_active = false;
                    light->yellow_active = false;
                    light->green_active = true;
                }
            } else {

                cycle_position = light->cycle_count % 6;

                /* Update state based on cycle position */
                if (cycle_position == 3) {
                    /* Yellow Light */
                    light->red_active = false;
                    light->yellow_active = true;
                    light->green_active = false;

                } else if (cycle_position == 4 || cycle_position == 5) {
                    /* Red Light */
                    light->red_active = true;
                    light->yellow_active = false;
                    light->green_active = false;

                    if (light->ped_requested) {
                        /* If pedestrian mode requested, this stop cycle given to pedestrian mode */
                        light->ped_crossing = true;
                        light->ped_requested = false;
                        light->ped_cycle_count = 0;
                        printk(KERN_INFO "mytraffic: Starting pedestrian mode\n");

                        light->yellow_active = true;

                    }

                } else {
                    /* GREEN LIGHT */
                    light->red_active = false;
                    light->yellow_active = false;
                    light->green_active = true;
                }
            }
            break;

        case FLASHING_RED:
            light->red_active = (light->cycle_count % 2 == 1);
            light->yellow_active = false;
            light->green_active = false;
            break;

        case FLASHING_YELLOW:
            light->red_active = false;
            light->yellow_active = (light->cycle_count % 2 == 1);
            light->green_active = false;
            break;

    }
    /* Set GPIO pins according to state */
    gpio_set_value(RED, light->red_active);
    gpio_set_value(YELLOW, light->yellow_active);
    gpio_set_value(GREEN, light->green_active);

    /* Update timer */
    mod_timer(&light->timer, jiffies + HZ / light->rate);
}

/* Interrupt Request Handler for Toggle Button */
static irqreturn_t toggle_interrupt_handler(int irq, void *dev_id)
{
    struct traffic_light *light = (struct traffic_light *)dev_id;

    /* Debouncing to handle successive button presses */
    static unsigned long most_recent_interrupt = 0;
    unsigned long current_interrupt = jiffies;
    if (current_interrupt - most_recent_interrupt < msecs_to_jiffies(250)) {
        return IRQ_HANDLED; /* Ignore if within debounce period */
    }
    most_recent_interrupt = current_interrupt;

    switch (light->current_mode) {
        case NORMAL:
            light->current_mode = FLASHING_RED;
            printk(KERN_INFO "mytraffic: Switched to FLASHING RED mode\n");
            break;
        case FLASHING_RED:
            light->current_mode = FLASHING_YELLOW;
            printk(KERN_INFO "mytraffic: Switched to FLASHING YELLOW mode\n");
            break;
        case FLASHING_YELLOW:
            light->current_mode = NORMAL;
            printk(KERN_INFO "mytraffic: Switched to NORMAL mode\n");
            break;

    }
    light->cycle_count = 0;
    return IRQ_HANDLED;
}

/* Interrupt request handler for pedestrian button
    when pressed in NORMAL mode, set the ped_requested flag to true */
static irqreturn_t ped_interrupt_handler(int irq, void *dev_id)
{
    struct traffic_light* light = (struct traffic_light *)dev_id;
    static unsigned long most_recent_interrupt = 0;
    unsigned long current_interrupt = jiffies;
    if (current_interrupt - most_recent_interrupt < msecs_to_jiffies(250)) {
        return IRQ_HANDLED; /* Ignore if within debounce period */
    }
    most_recent_interrupt = current_interrupt;

    if (light->current_mode == NORMAL) {
        light->ped_requested = true;
        printk(KERN_INFO "mytraffic: Pedestrian crossing requested\n");
    }
    return IRQ_HANDLED;
}

/* Init Function */
static int mytraffic_init(void) {
    int result;
    printk(KERN_INFO
    "mytraffic: Initializing traffic light module\n");

    /* Register the character device */
    result = register_chrdev(mytraffic_major, "mytraffic", &mytraffic_fops);
    if (result < 0) {
        printk(KERN_ALERT
        "mytraffic: cannot obtain major number %d\n", mytraffic_major);
        return result;
    }

    /* Allocate traffic light structure */
    t_light = kmalloc(sizeof(struct traffic_light), GFP_KERNEL);
    if (!t_light) {
        printk(KERN_ALERT
        "mytraffic: insufficient kernel memory\n");
        unregister_chrdev(mytraffic_major, "mytraffic");
        return -ENOMEM;
    }

    /* Initialize traffic light state */
    t_light->cycle_count = 0;
    t_light->rate = 1; // default to 1Hz, add add'l functionality later, time permitting
    t_light->red_active = false;
    t_light->yellow_active = false;
    t_light->green_active = true; // Start with state 0...green light
    t_light->ped_requested = false;
    t_light->ped_crossing = false;

    /* Setup GPIO pins */
    result = gpio_request(RED, "Red");
    if (result) {
        printk(KERN_ALERT "mytraffic: Failed to request GPIO %d\n", RED);
        kfree(t_light);
        unregister_chrdev(mytraffic_major, "mytraffic");
        return result;
    }
    gpio_direction_output(RED, 0);

    result = gpio_request(YELLOW, "Yellow");
    if (result) {
        printk(KERN_ALERT "mytraffic: Failed to request GPIO %d\n", YELLOW);
        gpio_free(RED);
        kfree(t_light);
        unregister_chrdev(mytraffic_major, "mytraffic");
        return result;
    }
    gpio_direction_output(YELLOW, 0);

    result = gpio_request(GREEN, "Green");
    if (result) {
        printk(KERN_ALERT "mytraffic: Failed to request GPIO %d\n", GREEN);
        gpio_free(YELLOW);
        gpio_free(RED);
        kfree(t_light);
        unregister_chrdev(mytraffic_major, "mytraffic");
        return result;
    }
    gpio_direction_output(GREEN, 1); // Start in state 0 with green light on

    t_light->current_mode = NORMAL;

    result = gpio_request(TOGGLE_BTN, "Toggle Button");
    if (result) {
        printk(KERN_ALERT "mytraffic: Failed to request GPIO %d\n", TOGGLE_BTN);
        gpio_free(GREEN);
        gpio_free(YELLOW);
        gpio_free(RED);
        kfree(t_light);
        unregister_chrdev(mytraffic_major, "mytraffic");
        return result;
    }
    gpio_direction_input(TOGGLE_BTN);
    printk(KERN_INFO "mytraffic: TOGGLE button GPIO %d requested\n", TOGGLE_BTN);

    t_light->irq_toggle = gpio_to_irq(TOGGLE_BTN);
    result = request_irq(t_light->irq_toggle,
                         toggle_interrupt_handler,
                         IRQF_TRIGGER_RISING,
                         "toggle_button_handler",
                         (void *)t_light);

    result = gpio_request(PED_BTN, "Pedestrian Button");
    if (result) {
        printk(KERN_ALERT "mytraffic: Failed to request GPIO %d\n", PED_BTN);
        gpio_free(GREEN); gpio_free(YELLOW); gpio_free(RED); gpio_free(TOGGLE_BTN);
        kfree(t_light);
        unregister_chrdev(mytraffic_major, "mytraffic");
        return result;
    }
    gpio_direction_input(PED_BTN);
    printk(KERN_INFO "mytraffic: PED button GPIO %d requested\n", PED_BTN);
    t_light->irq_ped = gpio_to_irq(PED_BTN);
    result = request_irq(t_light->irq_ped,
                         ped_interrupt_handler,
                         IRQF_TRIGGER_RISING,
                         "ped_button_handler",
                         (void *)t_light);


    timer_setup(&t_light->timer, timer_callback, 0);
    mod_timer(&t_light->timer, jiffies + t_light->rate);

    printk(KERN_INFO "mytraffic: Traffic light module initialized\n");
    return 0;
}

static void mytraffic_exit(void)
{
    printk(KERN_INFO "mytraffic: Cleaning up...\n");

    /* Remove timer */
    del_timer_sync(&t_light->timer);
    printk(KERN_INFO "mytraffic: Timer stopped\n");

    /* Free the Interrupts */
    free_irq(t_light->irq_toggle, (void *)t_light);
    free_irq(t_light->irq_ped, (void *)t_light);

    /* Turn off all LEDs */
    gpio_set_value(RED, 0);
    gpio_set_value(YELLOW, 0);
    gpio_set_value(GREEN, 0);

    /* Free the pins */
    gpio_free(TOGGLE_BTN);
    gpio_free(PED_BTN);
    gpio_free(RED);
    gpio_free(YELLOW);
    gpio_free(GREEN);
    printk(KERN_INFO "mytraffic: GPIOs freed\n");

    kfree(t_light);
    t_light = NULL;
    printk(KERN_INFO "mytraffic: Memory freed\n");

    /* Unregister character device */
    unregister_chrdev(mytraffic_major, "mytraffic");
    printk(KERN_INFO "mytraffic: Character device unregistered\n");
    printk(KERN_INFO "mytraffic: Module unloaded\n");
}

