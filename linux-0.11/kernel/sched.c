/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define _S(nr) (1<<((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	i=0;
	while (i<j && !((char *)(p+1))[i])
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i,j);
}

void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void); /* 实现在 kernel/system_call.s */
extern int system_call(void);

union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];
};

static union task_union init_task = {INIT_TASK,};

long volatile jiffies=0;
long startup_time=0;
struct task_struct *current = &(init_task.task);
struct task_struct *last_task_used_math = NULL;

struct task_struct * task[NR_TASKS] = {&(init_task.task), };

long user_stack [ PAGE_SIZE>>2 ] ; // 用户栈，以后每次 fork 出来的代码空间里，都用这个。

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math=1;
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
void schedule(void)
{
	int i,next,c;
	struct task_struct ** p;

/* check alarm, wake up any interruptible tasks that have got a signal */

	/* 
		#define FIRST_TASK task[0]
		#define LAST_TASK task[NR_TASKS-1]
	 */

	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			/* 
				如果设置过任务定时值，并且已经过期，则在信号位图中置 SIGALRM 信号，然后清空 alarm。task.alarm 中存放的是滴答数， 
				如果 jiffies 超过了这个滴答数，说明 alarm 到期
			 */
			if ((*p)->alarm && (*p)->alarm < jiffies) {
					(*p)->signal |= (1<<(SIGALRM-1));
					(*p)->alarm = 0;
				}

			/*
				如果信号中除了被阻塞的信号(就是不关心的那些信号，但
				SIGKILL和SIGSTOP是不能被忽略的)外还有其他信号，
				并且任务处于可中断睡眠状态，则置任务为就绪态。
			 */
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING; /* 置于就绪态 */
		}

/* this is the scheduler proper: */
/* 这里调度器部分 */
	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		/* 找出就绪态中剩余时间片最大的那个任务，即 counter 值最大的任务. counter 也可以理解成优先级 */
		while (--i) {
			if (!*--p) /* *--p 相当于 *(--p)，先减，再解引用。直到找到一个不空的任务 */
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}

		/* 如果 c 不等于0，break, switch_next 就直接切换到 counter 最大的那个任务 */
		/* 如果没有任务处于就绪态，这时系统处于空闲状态，直接执行任务 0 */
		/* 如果所有的就绪态任务时间片都是 0，则重新调整所有任务的时间片值 */
		if (c) break;

		/* 重新调整剩余时间片的值 counter = counter / 2 + priority */
		/* IO进程，可以认为是前台进程，这些前台进程优先级会越来越高，最大为2*priority，照顾了IO-bound进程*/
		/* C(t) = c(t-1)/2+p = p+p/2+p/4+p/8 +...+ p/2^n + ... = 2p */
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
		/* 完成后接着循环 */
	}
	switch_to(next);

	/*
		#define switch_to(n) {\
		struct {long a,b;} __tmp; \
		__asm__("cmpl %%ecx,current\n\t" \
			"je 1f\n\t" \
			"movw %%dx,%1\n\t" \
			"xchgl %%ecx,current\n\t" \
			"ljmp *%0\n\t" \
			"cmpl %%ecx,last_task_used_math\n\t" \
			"jne 1f\n\t" \
			"clts\n" \
			"1:" \
			::"m" (*&__tmp.a),"m" (*&__tmp.b), \
			"d" (_TSS(n)),"c" ((long) task[n])); \
		}
		
		mov _TSS(n), %edx             取到 任务 n 的 TSS 的选择子放到 edx
		mov task[n], %ecx             取到 任务 n 的 指针
		cmp %ecx, 0x1b140             查看 任务 n 是不是当前任务，如果是就结束
		je end
		mov %dx, 0x4(%esp)            把选择子复制到 __tmp.b 中去
		xchg %ecx, 0x1b140			  current = task[n], ecx = current
		ljmp *(%esp)                  ljmp __tmp  执行任务切换...注意，到这里已经跳转走了，只有任务再次切换回来，才会继续执行下面的语句
		cmp %ecx, 0x1dee8             ecx == last_task_used_math ? clts : jmp end; 上个任务使用过协处理吗？是的就清cr0中的任务切换标志
		jne end
		clts
	 */
}

int sys_pause(void)
{
	/* 下次调度的话，一定不会再次调度到它。puase 是可被中断的睡眠状态，也就是说遇到中断可被唤醒 */
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

void sleep_on(struct task_struct **p)
{
	/* p 是任务队列的头指针。指针是含有一个变量地址的变量。这个队列表示的是等待同一个资源的任务 */
	struct task_struct *tmp;

	if (!p)
		return;
	/* 任务0是不能进入睡眠状态的 */
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");

	/*  
		tmp = 队头任务
		*p = 当前任务
		这时候队头是 current, tmp 是旧的队头任务
	 */
	tmp = *p; /* 世界上最隐蔽的队列，仔细体会。这个 tmp 在当前进程的内核栈中。 */
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE; /* 当前任务置为不可中断的睡眠状态 */

	/* 切换出去 */
	schedule();

	/* 已经调度回来了，处于运行态 */
	/* 既然大家都在等待同样的资源，那就应该唤醒所有等待的该资源的任务 */
	if (tmp)
		tmp->state=0;
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p; /* 世界上最隐蔽的队列，仔细体会。这个 tmp 在当前进程的内核栈中。 */
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE; /* 当前任务置为可中断的睡眠状态 */
	/* 切换出去 */
	schedule();

	/* 某个时刻回来了 */
	/* 被唤醒处于运行态 */

	/* 如果当前队头任务不是本任务，说明以有新的任务被插入等待队列前部。因此应该先唤醒它们，并让自己继续等待 */
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	*p=NULL; /* 当前任务是队头任务，已经被唤醒，开始唤醒队列中其余任务 */
	if (tmp) /* 唤醒旧的队头任务 */
		tmp->state=0;
}

void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;
		*p=NULL; 
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

static struct timer_list {
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

void do_timer(long cpl)
{
	/* cpl 表示时钟中断是正在被执行的代码选择段选择子的特权级 */

	extern int beepcount; /* 扬声器发声时间滴答数 kernel/chr_drv/console.c 中定义 */
	extern void sysbeepstop(void); /* 关闭扬声器 kernel/chr_drv/console.c 中定义 */

	/* 如果发声计数的次数到了，则关闭发声 */
	if (beepcount)
		if (!--beepcount)
			sysbeepstop();

	/* 
		如果 cpl 为 0，表示内核程序在运行，把当前进程的 stime 递增，Linux 把内核程序称为 supervisor(超级用户) 
		如果 cpl > 0, 表明是一般用户，增加当前进程的 utime
	*/
	if (cpl)
		current->utime++;
	else
		current->stime++;

	/*
		static struct timer_list {
			long jiffies;
			void (*fn)();
			struct timer_list * next;
		} timer_list[TIME_REQUESTS], * next_timer = NULL;

		这是一个链表，串接了所有的定时器。
		如果有定时器存在，则将链表中第1个定时器的值减 1，如果已经等于 0，则调用相应的处理程序，并将
		该处理程序指针置为空。然后去掉该定时器。next_timer 是定时器链表的头指针。
	 */
	if (next_timer) {
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}

	/* 如果当前软盘控制器 FDC 的数字输出寄存器中马达启动位有置位的，则执行软盘定时程序 */
	if (current_DOR & 0xf0)
		do_floppy_timer();

	/* 如果当前进程时间还没完，直接返回，否则强制调用执行调度函数 */
	if ((--current->counter)>0) return;
	current->counter=0;

	/* 内核态下，不依赖counter值进行调度 */
	if (!cpl) return;

	/* 执行调度 */
	schedule();
}

int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

void sched_init(void)
{
	/* 调试程序初始化子程序 */

	/*
		typedef struct desc_struct {
			unsigned long a,b;
		} desc_table[256];
	 */
	int i;
	struct desc_struct * p;

	/* 
		Linux 系统开发之初，内核不成熟。内核代码会被经常修改。
		Linux 怕自己无意中修改了这些关键性的数据结构，造成与 
		POSIX 标准的不兼容。这里加入下面这个判断语句并无必要，
		纯粹是为了提醒自己以及其他修改内核代码的人
	 */
	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	/* 
		FIRST_TSS_ENTRY = 4, gdt 表在 head.s 中定义 基地址是 0x5cb8 
		FIRST_LDT_ENTRY = 5
		在 gdt[4] 处安装 tss（任务状态段），这里需要 tss 的基址和界限
		在 gdt[5] 处安装 ldt
		这是 gdt 描述符中的 S 位置 1，表示这是系统段
		同时， TYPE = 9 表示 TSS   TYPE = 2 表示 ldt 表
		gdt[4] = 0x0000 8901 a428 0068 base = 0x0001 a428 limit =  0x68 = 105
		gdt[5] = 0x0000 8201 a410 0068 base = 0x0001 a410 limit =  0x68 = 105
		界限值至少是 103，任何小于该值的 TSS，在执行任务切换时，都会引发处理器异常。


		#define NR_TASKS 64
		#define PAGE_SIZE 4096
		union task_union {
			struct task_struct task;
			char stack[PAGE_SIZE];
		};

		static union task_union init_task = {INIT_TASK,};

		struct task_struct *current = &(init_task.task);

		struct task_struct *last_task_used_math = NULL;

		struct task_struct * task[NR_TASKS] = {&(init_task.task), };
	 */
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));

	/* #define NR_TASKS 64 */
	/* p = &gdt[6] */
	/* 清空任务数组和描述符表项。注意 i = 1, 也就是初始任务不会清空 */
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
/* 
	清除 NT 标志，Nest Task, 嵌套任务标志。每个任务的 TSS 中都有一个任务链接域(指向前一个任务的指针)，可以填写
	前一个任务的 TSS 描述符选择子。如果当前任务NT位是1，则表示当前正在执行的任务嵌套于其他任务内，并且能够通过
	TSS 任务链接域的指针返回到前一个任务。
	无论何时处理器碰到 iret 指令，都要检查 NT 位，如果此位是 0，表明是一般的中断过程，按一般的中断返回处理
	如果此位是 1，表明当前任务之所以能够正在执行，是因为中断了别的任务，因此，应当返回原先被中断的任务继续执行。
	换句话说，NT指明当前 TSS 的任务链接域 back_link 是否有效。
 */
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");

/*
	#define _TSS(n) ((((unsigned long) n)<<4)+(FIRST_TSS_ENTRY<<3))  -->  n*2^4 + 4*2^3 = 16n + 32
	#define _LDT(n) ((((unsigned long) n)<<4)+(FIRST_LDT_ENTRY<<3))  -->  n*2^4 + 5*2^3 = 16n + 40
	#define ltr(n) __asm__("ltr %%ax"::"a" (_TSS(n)))
	#define lldt(n) __asm__("lldt %%ax"::"a" (_LDT(n)))

	TR   |TSS选择子|
	LDTR |LDT选择子|

	gdt 表大概是这个样子
	|    空描述符    |
	|内核代码段描述符|
	|内核数据段描述符|
	|    空描述符    |
	|任务0 TSS 描述符| <- gdt[4]
	|任务0 LDT 描述符| <- gdt[5]
	|任务1 TSS 描述符| <- gdt[6]
	|任务1 LDT 描述符|
	|任务2 TSS 描述符|
	|任务2 LDT 描述符|
	|任务3 TSS 描述符|
	|任务3 LDT 描述符|
	|      ...       |
 */
	
	ltr(0); /* 这句执行后，并不执行任务的切换 */
	lldt(0);

	/* 初始化 8253 定时器，通道 0，选择工作方式 3，二进制计数方式。通道 0 的输出引脚接在中断控制芯片的 IRQ0 上，每 10ms 发出一个IRQ0请求。LATCH是初始的定时计数值 */
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	set_intr_gate(0x20,&timer_interrupt);
	outb(inb_p(0x21)&~0x01,0x21);
	set_system_gate(0x80,&system_call);
}


/*
	为方便对照，把定义贴在这里. 后面的值表示的是 init_task 的值


struct task_struct {
/* these are hardcoded - don't touch 
	long state;	/* -1 unrunnable, 0 runnable, >0 stopped                 0
	long counter;                                                        15
	long priority;                                                       15
	long signal;                                                         0
	struct sigaction sigaction[32];                                      {{},}
	long blocked;	/* bitmap of masked signals                          0
/* various fields 
	int exit_code;                                                       0
	unsigned long start_code,end_code,end_data,brk,start_stack;          0, 0, 0, 0, 0
	long pid,father,pgrp,session,leader;                                 0, -1, 0, 0, 0
	unsigned short uid,euid,suid;                                        0, 0, 0
	unsigned short gid,egid,sgid;                                        0, 0, 0
	long alarm;                                                          0, 
	long utime,stime,cutime,cstime,start_time;                           0, 0, 0, 0, 0
	unsigned short used_math;                                            0
/* file system info 
	int tty;		/* -1 if no tty, so it must be signed                -1
	unsigned short umask;                                                0022
	struct m_inode * pwd;                                                NULL
	struct m_inode * root;                                               NULL
	struct m_inode * executable;                                         NULL
	unsigned long close_on_exec;                                         0
	struct file * filp[NR_OPEN];                                         {NULL, }
/* ldt for this task 0 - zero 1 - cs 2 - ds&ss 
	struct desc_struct ldt[3];                                           {0x00000000 00000000, 0x00c0fa00 0000009f, 0x00c0f200 0000009f} 640KB, base = 0
/* tss for this task 
	struct tss_struct tss;                                               
};

struct tss_struct {
	long	back_link;	         0
	long	esp0;                PAGE_SIZE+(long)&init_task   // 一个 task_union 占用一页，其中前面一部分是 task,剩余部分用作栈。栈顶设置在这个页的末尾。
	long	ss0;		         0x10                         // 内核数据段选择子
	long	esp1;                0
	long	ss1;		         0
	long	esp2;                0
	long	ss2;		         0
	long	cr3;                 (long)&pg_dir
	long	eip;                 0
	long	eflags;              0
	long	eax,ecx,edx,ebx;     0, 0, 0, 0
	long	esp;                 0                           // 对于手工创建的第一个任务，这个值实际没什么用。
	long	ebp;                 0
	long	esi;                 0
	long	edi;                 0
	long	es;		             0x17
	long	cs;		             0x17
	long	ss;		             0x17
	long	ds;		             0x17
	long	fs;		             0x17
	long	gs;		             0x17
	long	ldt;		         _LDT(0)                    // 任务 0 的 ldt 描述符选择子
	long	trace_bitmap;	     0x80000000
	struct i387_struct i387;     {}
};
*/