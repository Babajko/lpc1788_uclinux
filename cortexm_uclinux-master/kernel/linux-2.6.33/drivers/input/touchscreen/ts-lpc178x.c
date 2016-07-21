

#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <asm/irq.h>


#define LPC1788_UART_RX			0x0
#define LPC1788_UART_TX			0x0
#define LPC1788_UART_DLL		0x0
#define LPC1788_UART_DLM		0x4
#define LPC1788_UART_IER		0x4
#define LPC1788_UART_IIR		0x8
#define LPC1788_UART_FCR		0x8
#define LPC1788_UART_LCR		0xC
#define LPC1788_UART_LSR		0x14
#define LPC1788_UART_SCR		0x1C
#define LPC1788_UART_ACR		0x20
#define LPC1788_UART_FDR		0x28
#define LPC1788_UART_TER		0x30


static int ts_lpc1788_debug = 4;

#define TS_LPC1788_DEBUG 1

#if defined(TS_LPC1788_DEBUG)

#define ts_printk(level, fmt, args...)					\
	if (ts_lpc178x_debug >= level) printk(KERN_INFO "%s: " fmt,	\
				       	   __func__, ## args)

#else

#define ts_printk(level, fmt, args...)

#endif

struct ts_event {
	/* For portability, we can't read 12 bit values using SPI (which
	 * would make the controller deliver them as native byteorder u16
	 * with msbs zeroed).  Instead, we read them as two 8-bit values,
	 * *** WHICH NEED BYTESWAPPING *** and range adjustment.
	 */
	u16 status;
	u16	x;
	u16	y;
	u16	z1, z2;
	int	ignore;
};

struct lpc178x_packet {
	u8			read_x, read_y, read_z1, read_z2, pwrdown;
	u16			dummy;		/* for the pwrdown read */
	struct ts_event		tc;
};

struct lpc178x_ts {
	struct input_dev	*input;
	char			phys[32];
	char			name[32];

	u16			model;
	u16			vref_mv;
	u16			vref_delay_usecs;
	u16			x_plate_ohms;
	u16			pressure_max;

	bool			swap_xy;

	struct lpc178x_packet	*packet;

	int			msg_idx;
	int			read_cnt;
	int			read_rep;
	int			last_read;

	u16			debounce_max;
	u16			debounce_tol;
	u16			debounce_rep;

	u16			penirq_recheck_delay_usecs;

	spinlock_t		lock;
	struct hrtimer		timer;
	unsigned		pendown:1;	/* P: lock */
	unsigned		pending:1;	/* P: lock */
// FIXME remove "irq_disabled"
	unsigned		irq_disabled:1;	/* P: lock */
	unsigned		disabled:1;
	unsigned		is_suspended:1;

	int			(*filter)(void *data, int data_idx, int *val);
	void			*filter_data;
	void			(*filter_cleanup)(void *data);
	int			(*get_pendown_state)(void);
	int			gpio_pendown;

	void			(*wait_for_sync)(void);
};



struct lpc178x_uart {

	void __iomem		*reg_base;
	int			irq;
	spinlock_t              lock;
	struct hrtimer          timer;
};

static void ts_lpc178x_rx(void *ads)
{
	struct lpc178x_ts		*ts = ads;
	struct lpc178x_packet	*packet = ts->packet;

	u16			x, y, z1, z2, status;

	/* ads7846_rx_val() did in-place conversion (including byteswap) from
	 * on-the-wire format as part of debouncing to get stable readings.
	 */
	x = packet->tc.x;
	y = packet->tc.y;
	z1 = packet->tc.z1;
	z2 = packet->tc.z2;
	status = packet->tc.status;

	if (x == MAX_12BIT)
		x = 0;

	struct input_dev *input = ts->input;

	if(status & 0x81){
		input_report_key(input, BTN_TOUCH, 1);
		input_report_abs(input, ABS_X, x);
		input_report_abs(input, ABS_Y, y);
		input_report_abs(input, ABS_PRESSURE, 50);
	}else{
		input_report_key(input, BTN_TOUCH, 0);
		input_report_abs(input, ABS_PRESSURE, 0);
		input_sync(input);
	}
	input_sync(input);

}


static irqreturn_t ts_lpc1788_handler(int this_irq, void *dev_id)
{


}


static __devinit ts_lpc178x_probe(struct platform_device *pdev)
{
	struct lpc178x_ts			*ts;
	struct lpc178x_packet		*packet;
	struct lpc178x_uart *uart;
	struct resource *regs;
	struct input_dev		*input_dev;
	int ret;


	uart = kzalloc(sizeof(struct lpc178x_uart), GFP_KERNEL);
	if (!gpio) {
		ts_printk(1, "Error allocating memory\n");
		ret = -ENOMEM;
		goto Error_release_nothing;
	}


	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (! regs) {
		ts_printk(1, "no register base for gpio contoller\n");
		ret = -ENXIO;
		goto Error_release_nothing;
	}

	uart->reg_base = ioremap(regs->start, regs->end - regs->start + 1);
	if (!reg_base) {
		ts_printk(1, "unable to map register for gpio controller base=%08x\n", regs->start);
		ret = -EINVAL;
		goto Error_release_nothing;
	}

	uart->irq = platform_get_irq(pdev, 0);
	if ((irq < 0) || (irq >= NR_IRQS)) {
		ts_printk(1, "error getting irq\n");
		ret = -ENXIO;
		goto Error_release_nothing;
	}

	ret = request_irq(uart->irq, ts_lpc1788_handler, IRQF_DISABLED, "lpc178x-ts", uart);
	if (ret){
		ts_printk(1, "request irq failed\n");
		ret = -ENXIO;
		goto Error_release_nothing;
	}

	ts = kzalloc(sizeof(struct lpc178x_ts), GFP_KERNEL);
	packet = kzalloc(sizeof(struct lpc178x_packet), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!ts || !packet || !input_dev) {
		ret = -ENOMEM;
		goto Error_release_nothing;
	}

	ts->packet = packet;
	ts->input = input_dev;

	spin_lock_init(&ts->lock);

	ts->model = 7777;
	ts->vref_delay_usecs =100;
	ts->x_plate_ohms = 400;
	ts->pressure_max = ~0;

	snprintf(ts->phys, sizeof(ts->phys), "%s/input0", "lpc178x");
	snprintf(ts->name, sizeof(ts->name), "ADS%d Touchscreen", ts->model);
	
	input_dev->name = ts->name;
	input_dev->phys = ts->phys;
	//input_dev->dev.parent = &spi->dev;

	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	input_set_abs_params(input_dev, ABS_X,
			pdata->x_min ? : 0,
			pdata->x_max ? : MAX_12BIT,
			0, 0);
	input_set_abs_params(input_dev, ABS_Y,
			pdata->y_min ? : 0,
			pdata->y_max ? : MAX_12BIT,
			0, 0);
	input_set_abs_params(input_dev, ABS_PRESSURE,
			0, 100, 0, 0);
	
	ret = input_register_device(input_dev);
	if (ret){
		goto Error_release_nothing;
	}


Error_release_nothing:
Done:
	return ret;


}

static int ts_lpc178x_remove(struct platform_device *dev)
{

}

static struct platform_driver ts_lpc178x_driver = {
	.probe = ts_lpc178x_probe,
	.remove = ts_lpc178x_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "ts-lpc178x",
	},
};


static int __init ts_lpc178x_init(void)
{
	return platform_driver_register(&ts_lpc178x_driver);
}
module_init(ts_lpc178x_init);

static void __exit ts_lpc178x_exit(void)
{
	platform_driver_unregister(&ts_lpc178x_driver);
}
module_exit(ts_lpc178x_exit);

MODULE_DESCRIPTION("LPC178X TouchScreen Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("lpc178x:touchscreen");
