/*
 *  linux/kernel/traps.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'Traps.c' handles hardware traps and faults after we have saved some
 * state in 'asm.s'. Currently mostly a debugging-aid, will be extended
 * to mainly kill the offending process (probably by giving it a signal,
 * but possibly by killing it outright if necessary).
 */
#include <string.h> 

#include <linux/head.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/io.h>

#define get_seg_byte(seg,addr) ({ \
register char __res; \
__asm__("push %%fs;mov %%ax,%%fs;movb %%fs:%2,%%al;pop %%fs" \
	:"=a" (__res):"0" (seg),"m" (*(addr))); \
__res;})

#define get_seg_long(seg,addr) ({ \
register unsigned long __res; \
__asm__("push %%fs;mov %%ax,%%fs;movl %%fs:%2,%%eax;pop %%fs" \
	:"=a" (__res):"0" (seg),"m" (*(addr))); \
__res;})

#define _fs() ({ \
register unsigned short __res; \
__asm__("mov %%fs,%%ax":"=a" (__res):); \
__res;})

int do_exit(long code);

void page_exception(void);

/* 
	下面这些函数几乎都在在 kernel/asm.s 中
	device_not_available/coprocessor_error/parallel_interrupt 在 kernel/system_call.s 中
	page_fault 在 mm/page.s 中
 */
void divide_error(void);  
void debug(void);
void nmi(void);
void int3(void);
void overflow(void);
void bounds(void);
void invalid_op(void);
void device_not_available(void);
void double_fault(void);
void coprocessor_segment_overrun(void);
void invalid_TSS(void);
void segment_not_present(void);
void stack_segment(void);
void general_protection(void);
void page_fault(void);
void coprocessor_error(void);
void reserved(void);
void parallel_interrupt(void);
void irq13(void);
/*
 	根据处理程序的特权级别，从当前任务的 tss 中取得栈段选择子的指针。
 	处理器把旧栈的选择子和栈指针压入新栈。毕竟，中断处理程序也是当前任务的一部分。
 	处理器把 EFLAGS、CS和EIP的当前状态压入新栈。
 	对于有错误代码的异常，处理器还要把错误代码压入新栈，紧挨着EIP之后。
	
	特权级变化：
	有错误代码              没有错误代码

	|错误代码| <- esp[0]    |   EIP  | <- esp[0]
	|   EIP  | <- esp[1]    |   CS   | <- esp[1]
	|   CS   | <- esp[2]    | EFLAGS | <- esp[2]
	| EFLAGS | <- esp[3]    |   ESP  | <- esp[3]
	|   ESP  | <- esp[4]    |   SS   | <- esp[4]
	|   SS   | <- esp[5]    |   ...  |
	|   ...  |
	    图1                      图2

 	如果中断处理程序的特权级别和当前特权级别一致，则不用转换栈。
 	处理器把EFLAGS、CS和EIP的当前状态压入当前栈。
 	对于有错误代码的异常，处理器还要把错误代码压入新栈，紧挨着EIP之后。

	特权级不变：
	有错误代码              没有错误代码

 	|错误代码| <- esp[0]    |   EIP  | <- esp[0]
	|   EIP  | <- esp[1]    |   CS   | <- esp[1]
	|   CS   | <- esp[2]    | EFLAGS | <- esp[2]
	| EFLAGS | <- esp[3]    |   ...  | 
	|   ...  |
        图3                     图4
 */
static void die(char * str,long esp_ptr,long nr)
{
	/*
	执行到这里，堆栈如图所示：
			|ret addr| <- esp + 0    返回到上层的 do_divide_error
			|"string"| <- esp + 4
			|  &EIP  | <- esp + 8
			|   0    | <- esp + c
			|CCCCCCCC| <- esp + 10   这一段是堆栈提升的空间，不用管	
			|CCCCCCCC| <- esp + 14
			|CCCCCCCC| <- esp + 18
			|CCCCCCCC| <- esp + 1c
			|ret addr| <- esp + 20   返回到上上层 asm.s中no_error_code 的 addl $8, %esp指令处
		  --|  &EIP  | <- esp + 24   这里存的是一个地址，具体看键头
		 |  |   0    | <- esp + 28   这里是自定义的一个 error_code 
		 |  |   fs   | <- esp + 2c
		 |  |   es   | <- esp + 30
		 |  |   ds   | <- esp + 34
		 |  |   ebp  | <- esp + 38
		 |  |   esi  | <- esp + 3c
		 |  |   edi  | <- esp + 40
		 |  |   edx  | <- esp + 44
		 |  |   ecx  | <- esp + 48
		 |  |   ebx  | <- esp + 4c                            
		 |  |   eax  | <- esp + 50  这里本是C函数的地址，和 eax 进行了交换, 这时 eax = &do_divide_error, 然后把这个单元变成了 eax 的值，变相的保存了 eax 的值
		 `->|   EIP  | <- esp + 54  
		    |   CS   | <- esp + 58
		    | EFLAGS | <- esp + 5c
		    |   ESP  | <- esp + 60
		    |   SS   | <- esp + 64

	注意参数： str = esp + 4	esp_ptr = esp + 8	nr = esp + c
	 */
	long * esp = (long *) esp_ptr; /* 这个指针指向 esp + 54 的位置 */
	int i;

	printk("%s: %04x\n\r",str,nr&0xffff);
	printk("EIP:\t%04x:%p\nEFLAGS:\t%p\nESP:\t%04x:%p\n",
		esp[1],esp[0],esp[2],esp[4],esp[3]);
	/*
		把 fs 的值取出来
		#define _fs() ({ \
			register unsigned short __res; \
			__asm__("mov %%fs,%%ax":"=a" (__res):); \
			__res;})
	 */
	printk("fs: %04x\n",_fs());

	printk("base: %p, limit: %p\n",get_base(current->ldt[1]),get_limit(0x17));

	/* 
	 * 如果说当前堆栈还是用户栈，那上面的堆栈图里就没有 ESP 和 SS 这两个内容。那如果判断当前是内核栈还是用户栈？
	 * 如果当前是内核栈，那么可以取到用户栈的 SS 的值，看是不是 0x17，如果是的，说明这是内核栈，还需要打印 16 字节的用户栈的内容。
	 */
	if (esp[4] == 0x17) {
		printk("Stack: ");
		for (i=0;i<4;i++)
			/*
				把 seg:addr 中的值取出来
				#define get_seg_long(seg,addr) ({ \
					register unsigned long __res; \
					__asm__("push %%fs;
					mov %%ax,%%fs;
					movl %%fs:%2,%%eax;
					pop %%fs" \
						:"=a" (__res):"0" (seg),"m" (*(addr))); \
					__res;})
			 */
			printk("%p ",get_seg_long(0x17,i+(long *)esp[3]));
		printk("\n");
	}

	/*
		取当前运行任务的任务号
		#define str(n) \
			__asm__("str %%ax\n\t" \
			"subl %2,%%eax\n\t" \
			"shrl $4,%%eax" \
			:"=a" (n) \
			:"a" (0),"i" (FIRST_TSS_ENTRY<<3))

		FIRST_TSS_ENTRY = 4
	*/
	str(i);
	printk("Pid: %d, process nr: %d\n\r",current->pid,0xffff & i);
	for(i=0;i<10;i++)
		printk("%02x ",0xff & get_seg_byte(esp[1],(i+(char *)esp[0])));
	printk("\n\r");
	do_exit(11);		/* play segment exception */
}

void do_double_fault(long esp, long error_code)
{
	die("double fault",esp,error_code);
}

void do_general_protection(long esp, long error_code)
{
	die("general protection",esp,error_code);
}

void do_divide_error(long esp, long error_code)
{
	/*

	执行到这里，堆栈如图所示：
			|ret addr| <- esp + 0   返回到上层 asm.s中no_error_code 的 addl $8, %esp指令处
		  --|  &EIP  | <- esp + 4   这里存的是一个地址，具体看键头
		 |  |   0    | <- esp + 8   这里是自定义的一个 error_code 
		 |  |   fs   | <- esp + c
		 |  |   es   | <- esp + 10
		 |  |   ds   | <- esp + 14
		 |  |   ebp  | <- esp + 18
		 |  |   esi  | <- esp + 1c
		 |  |   edi  | <- esp + 20
		 |  |   edx  | <- esp + 24
		 |  |   ecx  | <- esp + 28
		 |  |   ebx  | <- esp + 2c                            
		 |  |   eax  | <- esp + 30  这里本是C函数的地址，和 eax 进行了交换, 这时 eax = &do_divide_error, 然后把这个单元变成了 eax 的值，变相的保存了 eax 的值
		 `->|   EIP  | <- esp + 34  
		    |   CS   | <- esp + 38
		    | EFLAGS | <- esp + 3c
		    |   ESP  | <- esp + 40
		    |   SS   | <- esp + 44
	需要注意的是，无论是对于有错误码的异常还是无错误码的异常，到这里的栈并没有什么区别，在 asm.s 的 error_code 和 no_error_code 中把这两种情况统一了。
	本函数的汇编：
		sub $0x10, %esp
		pushl 0x18(%esp)    ( push [esp+0x18] ) 这是 error_code
		pushl 0x18(%esp)                        这是 esp
		push 0x18a14   这是"divide error"的地址
		call die
		add $0x1c, %esp
		ret
	 */
	die("divide error",esp,error_code);
}

void do_int3(long * esp, long error_code,
		long fs,long es,long ds,
		long ebp,long esi,long edi,
		long edx,long ecx,long ebx,long eax)
{
	int tr;

	__asm__("str %%ax":"=a" (tr):"0" (0));
	printk("eax\t\tebx\t\tecx\t\tedx\n\r%8x\t%8x\t%8x\t%8x\n\r",
		eax,ebx,ecx,edx);
	printk("esi\t\tedi\t\tebp\t\tesp\n\r%8x\t%8x\t%8x\t%8x\n\r",
		esi,edi,ebp,(long) esp);
	printk("\n\rds\tes\tfs\ttr\n\r%4x\t%4x\t%4x\t%4x\n\r",
		ds,es,fs,tr);
	printk("EIP: %8x   CS: %4x  EFLAGS: %8x\n\r",esp[0],esp[1],esp[2]);
}

void do_nmi(long esp, long error_code)
{
	die("nmi",esp,error_code);
}

void do_debug(long esp, long error_code)
{
	die("debug",esp,error_code);
}

void do_overflow(long esp, long error_code)
{
	die("overflow",esp,error_code);
}

void do_bounds(long esp, long error_code)
{
	die("bounds",esp,error_code);
}

void do_invalid_op(long esp, long error_code)
{
	die("invalid operand",esp,error_code);
}

void do_device_not_available(long esp, long error_code)
{
	die("device not available",esp,error_code);
}

void do_coprocessor_segment_overrun(long esp, long error_code)
{
	die("coprocessor segment overrun",esp,error_code);
}

void do_invalid_TSS(long esp,long error_code)
{
	die("invalid TSS",esp,error_code);
}

void do_segment_not_present(long esp,long error_code)
{
	die("segment not present",esp,error_code);
}

void do_stack_segment(long esp,long error_code)
{
	die("stack segment",esp,error_code);
}

void do_coprocessor_error(long esp, long error_code)
{
	if (last_task_used_math != current)
		return;
	die("coprocessor error",esp,error_code);
}

void do_reserved(long esp, long error_code)
{
	die("reserved (15,17-47) error",esp,error_code);
}

void trap_init(void)
{
	int i;

/*
	#define _set_gate(gate_addr,type,dpl,addr) \
		__asm__ ("movw %%dx,%%ax\n\t" \
		"movw %0,%%dx\n\t" \
		"movl %%eax,%1\n\t" \
		"movl %%edx,%2" \
		: \
		: "i" ((short) (0x8000+(dpl<<13)+(type<<8))), \
		"o" (*((char *) (gate_addr))), \
		"o" (*(4+(char *) (gate_addr))), \
		"d" ((char *) (addr)),"a" (0x00080000))

	#define set_trap_gate(n,addr) \
		_set_gate(&idt[n],15,0,addr)

	#define set_system_gate(n,addr) \
		_set_gate(&idt[n],15,3,addr)
 */
/*
	这部分主要是向 idt 表中安装一些中断处理过程。中断到陷阱门不会改变 EFLAGS 的 IF 位。
	set_trap_gate是安装陷阱门
	set_system_gate和set_trap_gate的区别只是特权级不一样，
	set_trap_gate的特权级是0，而set_system_gate是3.这是因为 int3, overflow(溢出)和bounds可以由任何程序产生
 */
	set_trap_gate(0,&divide_error);
/*
	上面这个宏将被翻译为以下汇编代码
	mov $0x7f4d, %edx  0x7f4d是 divide_error 函数的地址
	mov $0x80000, %eax
	mov %dx, %ax
	mov $0x8f00, %dx;
	mov %eax, 0x54b8    0x54b8是 idt[0] 的地址。
	mov %edx, 0x54bc	0x54bc是 idt[0] 地址的高4字地址
 */
	set_trap_gate(1,&debug);
	set_trap_gate(2,&nmi);
	set_system_gate(3,&int3);	/* int3-5 can be called from all */
	set_system_gate(4,&overflow);
	set_system_gate(5,&bounds);
	set_trap_gate(6,&invalid_op);
	set_trap_gate(7,&device_not_available);
	set_trap_gate(8,&double_fault);
	set_trap_gate(9,&coprocessor_segment_overrun);
	set_trap_gate(10,&invalid_TSS);
	set_trap_gate(11,&segment_not_present);
	set_trap_gate(12,&stack_segment);
	set_trap_gate(13,&general_protection);
	set_trap_gate(14,&page_fault);
	set_trap_gate(15,&reserved);
	set_trap_gate(16,&coprocessor_error);
	
	/* 把int 17-47 的陷阱门均设置为 reserved，以后硬件初始化时会重新设置自己的陷阱门 */
	for (i=17;i<48;i++)
		set_trap_gate(i,&reserved);

	/* 设置协处理器中断 0x2d(45) 陷阱门描述符，并允许其产生中断请求。设置并行口中断描述符 */
	set_trap_gate(45,&irq13);

	outb_p(inb_p(0x21)&0xfb,0x21); /* 允许8259A 主片 IRQ2 中断请求 */
	outb(inb_p(0xA1)&0xdf,0xA1); /* 允许8259A 从片 IRQ13 中断请求 */
	set_trap_gate(39,&parallel_interrupt); /* 设置并行口 1 的中断 0x27 陷阱门描述符 */
}
