/*
 *  linux/kernel/serial.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *	serial.c
 *
 * This module implements the rs232 io functions
 *	void rs_write(struct tty_struct * queue);
 *	void rs_init(void);
 * and all interrupts pertaining to serial IO.
 */

#include <linux/tty.h>
#include <linux/sched.h>
#include <asm/system.h>
#include <asm/io.h>

#define WAKEUP_CHARS (TTY_BUF_SIZE/4)

extern void rs1_interrupt(void);
extern void rs2_interrupt(void);

static void init(int port)
{
	outb_p(0x80,port+3);	/* set DLAB of line control reg */
	outb_p(0x30,port);	/* LS of divisor (48 -> 2400 bps */
	outb_p(0x00,port+1);	/* MS of divisor */
	outb_p(0x03,port+3);	/* reset DLAB */
	outb_p(0x0b,port+4);	/* set DTR,RTS, OUT_2 */
	outb_p(0x0d,port+1);	/* enable all intrs but writes */
	(void)inb(port);	/* read data port to reset things (?) */
}

void rs_init(void)
{
	/*
		p初始化串行中断程序和串行接口
		中断描述符表IDT中的门描述符设置宏 set_intr_gate() 在 include/asm/system.h 中实现
	 */
	set_intr_gate(0x24,rs1_interrupt); /* 设置串行口 1 的中断门向量(IRQ4信号) */
	set_intr_gate(0x23,rs2_interrupt); /* 设置串行口 2 的中断门向量(IRQ3信号) */
	init(tty_table[1].read_q.data); /* 初始化串行口 1 .data是端口基地址 */
	init(tty_table[2].read_q.data); /* 初始化串行口 2*/
	outb(inb_p(0x21)&0xE7,0x21); /* 允许8259A 响应 IRQ3、IRQ4 中断请求 */
}

/*
 * This routine gets called when tty_write has put something into
 * the write_queue. It must check wheter the queue is empty, and
 * set the interrupt register accordingly
 *
 *	void _rs_write(struct tty_struct * tty);
 */
void rs_write(struct tty_struct * tty)
{
	cli();
	if (!EMPTY(tty->write_q))
		outb(inb_p(tty->write_q.data+1)|0x02,tty->write_q.data+1);
	sti();
}
