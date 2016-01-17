/*
 *  am2301.c
 * The driver uses GPIO interrupts to read 1-wire data.
 * New data will be available as /proc/am2301.
 * The temperature is reported as Celsius degrees.
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/ktime.h>

/*   Host:         ~~~~|__|~~~
 *   Sensor ACK:              |__|~~|
 *   Sensor data:                    __|~~~|
 */
enum eState {
	READ_START,
	READ_START_HIGH,
	READ_BIT_START,
	READ_BIT_HIGH,
	READ_BIT_LOW,
	READ_STOP,
};

struct st_inf {
	int t;
	int rh;
};

#define SHORT_DELAY 3
//#define DEFAULT_DELAY 5
#define DEFAULT_DELAY 30
#define MODULE_NAME "am2301"
#define PROC_SUBDIR MODULE_NAME

static int _pin = 27;
static int _read_delay = DEFAULT_DELAY; /* in seconds */
static int _irq = -1;
static volatile int _read_req = READ_STOP;
static struct task_struct *ts = NULL;
static wait_queue_head_t _queue;
static ktime_t _old;
static volatile int _ulen;
static struct st_inf sns;
static unsigned int _reads[2] = {0, 0};
static unsigned char _data[5];

#ifdef CONFIG_PROC_FS
static struct proc_dir_entry *am231_dir = NULL, /* *pin_dir = NULL,*/ *temp_ent = NULL, *rh_ent = NULL, *reads_ent = NULL;

static int am231_show_temp(struct seq_file *m, void *v)
{
	if (_reads[1] < 2) { /* at least two consecutive readings OK */
		seq_printf(m,"NaN\n");
	} else {
		seq_printf(m, "%d.%d\n", sns.t / 10, (unsigned int) abs(sns.t%10));
	}
	return 0;
}

static int am231_open_temp(struct inode *inode, struct  file *file) {
	return single_open(file, am231_show_temp, NULL);
}

static const struct file_operations fops_temp = {
	.owner = THIS_MODULE,
	.open = am231_open_temp,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int am231_show_rh(struct seq_file *m, void *v)
{
	if (_reads[1] < 2) { /* at least two consecutive readings OK */
		seq_printf(m,"NaN\n");
	} else {
		seq_printf(m, "%d.%d\n", sns.rh / 10, sns.rh%10);
	}
	return 0;
}

static int am231_open_rh(struct inode *inode, struct  file *file) {
	return single_open(file, am231_show_rh, NULL);
}

static const struct file_operations fops_rh = {
	.owner = THIS_MODULE,
	.open = am231_open_rh,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int am231_show_reads(struct seq_file *m, void *v)
{
	seq_printf(m, "%d/%d\n", _reads[0], _reads[1]);
	return 0;
}

static int am231_open_reads(struct inode *inode, struct  file *file) {
	return single_open(file, am231_show_reads, NULL);
}

static const struct file_operations fops_reads = {
	.owner = THIS_MODULE,
	.open = am231_open_reads,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif


#define CHECK_RET(r) do { \
		if (r != 0) {			\
			return r;		\
		}				\
	} while (0)

/*
 * GPIO ISR
 * State machine for reading the sensor request.
 * Hopefuly the hardware performs some filtering.
 */
static irqreturn_t read_isr(int irq, void *data)
{
	ktime_t now = ktime_get_real();
	static int bit_count, char_count;

	switch (_read_req) {
	case READ_START:
		if (gpio_get_value(_pin) == 0) {
			_read_req = READ_START_HIGH;
		}
		break;
	case READ_START_HIGH:
		if (gpio_get_value(_pin) == 1) {
			_read_req = READ_BIT_START;
		}
		break;
	case READ_BIT_START:
		if (gpio_get_value(_pin) == 0) {
			_read_req = READ_BIT_HIGH;
			bit_count = 7;
			char_count = 0;
			memset(_data, 0, sizeof(_data));
		}
		break;
	case READ_BIT_HIGH:
		if (gpio_get_value(_pin) == 1) {
			_read_req = READ_BIT_LOW;
		}
		break;
	case READ_BIT_LOW:
		if (gpio_get_value(_pin) == 0) {
			_ulen = ktime_us_delta(now, _old);
			if (_ulen > 40) {
				_data[char_count] |= (1 << bit_count);
			}
			if (--bit_count < 0) {
				char_count++;
				bit_count = 7;
			}
			if (char_count == 5) {
				_read_req = READ_STOP;
				wake_up_interruptible(&_queue);
			} else {
				_read_req = READ_BIT_HIGH;
			}
		}
		break;
	case READ_STOP:
	default:
		break;
	}
	_old = now;
	return IRQ_HANDLED;
}

static int start_read(void)
{
	int ret;

	/*
	 * Set pin high and wait for two milliseconds.
	 */
 	ret = gpio_direction_output(_pin, 1);
	CHECK_RET(ret);

	udelay(2000);

	/*
	 * Set pin low and wait for at least 750 us.
	 * Set it high again, then wait for the sensor to put out a low pulse.
	 */
	gpio_set_value(_pin, 0);
	udelay(800);
	gpio_set_value(_pin, 1);

	_read_req = READ_START;

 	ret = gpio_direction_input(_pin);
	CHECK_RET(ret);

	return 0;
}

static int do_read_data(struct st_inf *s)
{
	unsigned char cks = 0;
	int max_u = 100;

 	if (!wait_event_interruptible_timeout(_queue, (_read_req == READ_STOP), max_u)) {
		_read_req = READ_STOP;
		return -1;
	}

	/*
	 * This seems to fail often.
	 * Assuming that sometimes one bit is lost and, if the values are low enough,
	 * the checksum is identical.
	 */
	cks = (_data[0] + _data[1] + _data[2] + _data[3]) & 0xFF;
	if (cks != _data[4]) {
		return -1;
	}

	/* ToDo: Check negative temperatures */
	s->rh = (int) (int16_t)(((uint16_t) _data[0] << 8) | (uint16_t) _data [1]);
	s->t  = (int) ((((uint16_t) _data[2] & 0x7F) << 8) | (uint16_t) _data [3]);
	if (_data[2] & 0x80) s->t = -s->t;
/*	if (s->rh > 1000 || s->rh < 0 || s->t > 800 || s->t < -400 ) {
		return -1;
	}*/
	return 0;
}

static int read_thread(void *data)
{
	int local_delay = 0;
	struct st_inf s;
	static struct st_inf sp;

        while (!kthread_should_stop()) {

		/*
		 * Do not sleep the whole chunk, otherwise if
		 *  the module is removed it will wait for that whole delay.
		 */
		if (local_delay != 0) {
			local_delay--;
			/* ToDo: Find a better interruptible delay implementation */
			wait_event_interruptible_timeout(_queue, 0, HZ);
			continue;
		}

		local_delay = _read_delay;
		_reads[0]++;

		if (start_read() != 0) {
			continue;
		}

		if (do_read_data(&s) != 0) {
			local_delay = SHORT_DELAY; /* Ignore this reading */
		}
		else {
			if (_reads[1] == 0) {
				local_delay = SHORT_DELAY;
				_reads[1]++ ;

			}
			else {
				if ((s.t - sp.t > 50) ||  /* 5 degrees difference */
				    (s.t - sp.t < -50) ||
				    (s.rh - sp.rh > 100) || /* or 10 RH differene */
				    (s.rh - sp.rh < -100))
				{
					/* Ignore this reading */
					local_delay = SHORT_DELAY;
				}
				else {
					sns = s;
					_reads[1]++;
					//printk(KERN_ERR "T     :\t\t%d.%d\n", sns.t / 10, sns.t%10);
					//printk(KERN_ERR "RH    :\t\t%d.%d\n", sns.rh / 10, sns.rh%10);
					//printk(KERN_ERR "QUAL  :\t\t%d/%d %d%c\n", _reads[1], _reads[0],
					//		_reads[1] * 100 / _reads[0], '\%');
				}
			}
			sp = s;
		}
        }
        return 0;
}

static int __init am2301_init(void)
{
	int ret;

	printk(KERN_INFO "Init " MODULE_NAME "\n");

	ret = gpio_request_one(_pin, GPIOF_OUT_INIT_HIGH, "AM2301");

	if (ret != 0) {
		printk(KERN_ERR "Unable to request GPIO, err: %d\n", ret);
		return ret;
	}

	_irq =  gpio_to_irq(_pin);
	if (_irq < 0) {
		printk(KERN_ERR MODULE_NAME ": Unable to create IRQ\n");
		goto _cleanup_1;

	}

	init_waitqueue_head(&_queue);

        ret = request_irq(_irq, read_isr,
			  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			  "read_isr", NULL);

	ts = kthread_create(read_thread, NULL, MODULE_NAME);

	if (ts) {
		wake_up_process(ts);
	} else {
		printk(KERN_ERR MODULE_NAME": Unable to create thread\n");
		goto _cleanup_2;
	}

#ifdef CONFIG_PROC_FS
	//am231_dir, pin_dir, temp_ent, rh_ent
	
	am231_dir = proc_mkdir(PROC_SUBDIR, NULL);
	if (!am231_dir) {
		printk(KERN_ERR MODULE_NAME ": Unable to create /proc/" PROC_SUBDIR "\n");
		goto _cleanup_3;
	}

	temp_ent = proc_create("temp", 0, am231_dir, &fops_temp);
	if (!temp_ent) {
		printk(KERN_ERR MODULE_NAME ": Unable to create /proc/" PROC_SUBDIR "/temp\n");
		goto _cleanup_4;
	}

	rh_ent = proc_create("rh", 0, am231_dir, &fops_rh);
	if (!rh_ent) {
		printk(KERN_ERR MODULE_NAME ": Unable to create /proc/" PROC_SUBDIR "/rh\n");
		goto _cleanup_4;
	}

	reads_ent = proc_create("reads", 0, am231_dir, &fops_reads);
	if (!reads_ent) {
		printk(KERN_ERR MODULE_NAME ": Unable to create /proc/" PROC_SUBDIR "/reads\n");
		goto _cleanup_4;
	}
#endif
	return 0;

_cleanup_4:
	if (am231_dir) {
		proc_remove(am231_dir);
		am231_dir = NULL;
	}
	if (temp_ent) {
		proc_remove(temp_ent);
		temp_ent = NULL;
	}
	if (rh_ent) {
		proc_remove(rh_ent);
		rh_ent = NULL;
	}
	if (reads_ent) {
		proc_remove(reads_ent);
		rh_ent = NULL;
	}
_cleanup_3:
	kthread_stop(ts);
_cleanup_2:
	free_irq(_irq, NULL);
_cleanup_1:
	gpio_free(_pin);

	return -1;
}

static void __exit am2301_exit(void)
{
	if (ts) {
                kthread_stop(ts);
        }

	if (_irq >= 0) {
		free_irq(_irq, NULL);
	}

 	(void) gpio_direction_output(_pin, 1);
	gpio_free(_pin);

	if (am231_dir) {
		proc_remove(am231_dir);
		am231_dir = NULL;
	}
	if (temp_ent) {
		proc_remove(temp_ent);
		temp_ent = NULL;
	}
	if (rh_ent) {
		proc_remove(rh_ent);
		rh_ent = NULL;
	}
	if (reads_ent) {
		proc_remove(reads_ent);
		rh_ent = NULL;
	}
	printk(KERN_INFO MODULE_NAME ": exit\n");
}

module_init(am2301_init);
module_exit(am2301_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Constantin Petra");
MODULE_DESCRIPTION("AM2301 driver");
module_param(_pin, int, S_IRUGO);
//MODULE_PARM(pin, "i");
MODULE_PARM_DESC(_pin, "Pin number - if not set, assuming 27");
