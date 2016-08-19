/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline _syscall0(int,fork)
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

static char printbuf[1024];

extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm * tm);
extern long startup_time;

/*
 * This is set up by the setup-routine at boot-time
 * 扩展内存的大小，15MB
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)

/*
 * 第 1 个硬盘的参数表
 */
#define DRIVE_INFO (*(struct drive_info *)0x90080)

/*
 * 当时在 bootsect 的第508 字节处，设置了一个根设备号，这个508字节的位置实际就是在 0x901FC 处。
 */
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

static void time_init(void)
{
	struct tm time;

	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
	startup_time = kernel_mktime(&time);
}

static long memory_end = 0;
static long buffer_memory_end = 0;
static long main_memory_start = 0;

struct drive_info { char dummy[32]; } drive_info;

void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
    /* 到这里的调试方法 先启动 dbg-c，然后在另一个终端启动 rungdb 就可以了*/
 	/* ROOT_DEV = 0x301 */
 	ROOT_DEV = ORIG_ROOT_DEV;
 	/*
 	硬盘参数表：
 	 cc 00 10 00 00 ff ff 00 
	 c8 00 00 00 cc 00 26 00
	 这里 DRIVE_INFO = 0x90080 当时在 setup.s 里把信息获取到放到了这里
 	 */
 	drive_info = DRIVE_INFO;

 	/* 
 	 * EXT_MEM_K = *0x90002 = 0x3c00 单位是 KB, 当时在 setup.s 里把扩展内存的大小放在了这里，那个值是 0x3c00 正好是 15MB 
 	 * 注意这里 EXT_MEM_K 是个宏
 	 * 下面这句话是获取整个内存的大小，一共是 16MB
 	 * memory_end = 2^20 B + 0x3c00 * 1024 B = 16 * 2^20 = 0x01000000
 	 */
	memory_end = (1<<20) + (EXT_MEM_K<<10);
	/* 对于不到 4KB 的内存，忽略掉。实际上就是把这个数字修正到4KB的整数倍 */
	memory_end &= 0xfffff000;

	/* 
	 * 如果 memory_end 大于16MB 那就取 memory_end 为 16MB
	 * 如果 memory_end 大于12MB 则 buffer_memory_end = 4MB
	 * 实际环境中，memory_end = 16MB buffer_memory_end = 4MB
	 * buffer_memory_end 是缓冲区末端
	 */
	if (memory_end > 16*1024*1024)
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024) 
		buffer_memory_end = 4*1024*1024;
	else if (memory_end > 6*1024*1024)
		buffer_memory_end = 2*1024*1024;
	else
		buffer_memory_end = 1*1024*1024;
	/*
	 * 设置主内存的起始位置在缓冲区末端 main_memory_start = 4MB = 0x00400000
	 */
	main_memory_start = buffer_memory_end;

	/* 
	 * 如果在 Makefile 文件中定义了内存虚拟盘符符号 RAMDISK, 则初始化虚拟盘
	 * 实际调试过程中，下面这一句没有执行。
	 */
#ifdef RAMDISK
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);
#endif
	/*
	 * 主内存区初始化 mm/memory.c 
	 */
	mem_init(main_memory_start,memory_end);
	/*
	 * 在这个位置，在gdb print /d mem_map到这里会提示 768 个 100 和 3072 次个 0
	 */

	/*
	 * 陷阱门(硬件中断向量)初始化 kernel/traps.c
	 */
	trap_init();

	/*
	 * 块设备初始化 kernel/blk_drv/ll_rw_blk.c
	 */
	blk_dev_init();

	/*
	 * 字符设备初始化 kernel/ch_drv/tty_io.c
	 * 这个函数貌似什么都没做
	 */
	chr_dev_init();

	/*
	 * tty初始化 kernel/ch_drv/tty_io.c
	 */
	tty_init();

	/*
	 * 设置开机启动时间 startup_time
	 */
	time_init();

	/*
	 * 调度程序初始化 kernel/sched.c
	 */
	sched_init();

	/*
	 * 缓冲管理初始化 fs/buffer.c
	 */
	buffer_init(buffer_memory_end);

	/*
	 * 硬盘初始化 kernel/blk_drv/hd.c
	 */
	hd_init();

	/*
	 * 软驱初始化 kernel/blk_drv/floppy.c
	 */
	floppy_init();

	/*
	 * 所有初始化完毕，开中断
	 */
	sti();

	/*
	   下面过程通过在堆栈中设置的参数，利用中断返回指令启动任务0执行。
	 	#define move_to_user_mode() \
			__asm__ ("movl %%esp,%%eax\n\t" \
				"pushl $0x17\n\t" \
				"pushl %%eax\n\t" \
				"pushfl\n\t" \
				"pushl $0x0f\n\t" \
				"pushl $1f\n\t" \   标签 1 的指令地址，f = forward 向前。
				"iret\n" \
				"1:\tmovl $0x17,%%eax\n\t" \
				"movw %%ax,%%ds\n\t" \
				"movw %%ax,%%es\n\t" \
				"movw %%ax,%%fs\n\t" \
				"movw %%ax,%%gs" \
				:::"ax")
		这段代码翻译出的汇编是这样的
		mov %esp, %eax
		push $0x17   局部任务数据段
		push %eax
		pushf
		push $0xf    局部任务代码段
		push $0x68e3
		iret                 假装从调用门返回。
		mov $0x17, %eax
		mov %eax, %ds
		mov %eax, %es
		mov %eax, %fs
		mov %eax, %gs

		即构造出这样一个栈，然后执行 iret 
		   | 0x68e3 |  EIP
		   |   0xf  |  CS          局部描述符的第一个描述符选择子。
		   | EFLAGS |  EFLAGS
		 --|  esp   |  ESP
		|  |  0x17  |  SS
		`->|  ...   |
	 */
	move_to_user_mode(); /* 移到用户模式下执行 */
	if (!fork()) {		/* we count on this going ok */
		init(); /* 在新建的子进程（任务1）中执行 */
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
	for(;;) pause();
}

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

void init(void)
{
	int pid,i;

	setup((void *) &drive_info);
	(void) open("/dev/tty0",O_RDWR,0);
	(void) dup(0);
	(void) dup(0);
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);
	if (!(pid=fork())) {
		close(0);
		if (open("/etc/rc",O_RDONLY,0))
			_exit(1);
		execve("/bin/sh",argv_rc,envp_rc);
		_exit(2);
	}
	if (pid>0)
		while (pid != wait(&i))
			/* nothing */;
	while (1) {
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {
			close(0);close(1);close(2);
			setsid();
			(void) open("/dev/tty0",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));
		}
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
