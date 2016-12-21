/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

extern void write_verify(unsigned long address);

long last_pid=0;

void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000;
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}

int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;
	/* 取出代码段和数据段的限长及基址，然后比较是否相等，如果不相等则出错 */
	code_limit=get_limit(0x0f);
	data_limit=get_limit(0x17);
	old_code_base = get_base(current->ldt[1]);
	old_data_base = get_base(current->ldt[2]);
	if (old_data_base != old_code_base)
		panic("We don't support separate I&D");
	if (data_limit < code_limit)
		panic("Bad data_limit");

	/* 设置新的基址。每个任务的任务空间大小是 64MB。 分配虚存、建段表*/
	new_data_base = new_code_base = nr * 0x4000000;
	p->start_code = new_code_base;
	set_base(p->ldt[1],new_code_base);
	set_base(p->ldt[2],new_data_base);
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
		printk("free_page_tables: from copy_mem\n");
		free_page_tables(new_data_base,data_limit);
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
 /* 
		内核栈的样子：
	|ret addr| <- esp[0]             esp + 0
	|   EAX  | <- esp[1]             esp + 4      这些都是用户态的寄存器，先保存到内核栈里，这个参数对应 nr
	|   EBP  | <- esp[2]             esp + 8      这个对应参数 ebp，后面依此类推
	|   EDI  | <- esp[3]             esp + c
	|   ESI  | <- esp[4]             esp + 10     
	|   GS   | <- esp[5]             esp + 14
	|   EBX  | <- esp[6]             esp + 18
	|   ECX  | <- esp[7]             esp + 1c
	|   EDX  | <- esp[8]             esp + 20
	|   FS   | <- esp[9]             esp + 24     这些都是用户态的选择子，先保存到内核栈里
	|   ES   | <- esp[10]            esp + 28
	|   DS   | <- esp[11]            esp + 2c
	|   EIP  | <- esp[12]            esp + 30
	|   CS   | <- esp[13]            esp + 34
	| EFLAGS | <- esp[14]            esp + 38     通过陷阱门过来的，所以这里有 EFLAGS 
	|   ESP  | <- esp[15]            esp + 3c
	|   SS   | <- esp[16]            esp + 40
	|   ...  | <- esp[17]            esp + 44

	    图1
	copy_process 的参数请对照上图
 */
/*
	在 fork() 的执行过程中，即下面的 copy_process，内核并不会立刻为新进程分配代码和数据
	内存页。新进程将与父进程共同使用父进程已有的代码和数据内存页面。只有当以后执行过程中
	如果其中有一个进程以写的方式访问内存时被访问的内存页面才会在写操作前被复制到新申请的
	内存页面中。这种实现方式称为 copy on write (写时复制)
 */
int copy_process(int nr,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx,
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	/* nr是当前task中的空闲的项，由find_empty_process返回 */
	struct task_struct *p;
	int i;
	struct file *f;

	/* 这里获取到的是物理内存页，在 head.s 中已经过映射，从0-16MB的范围，物理地址和线性地址是一样的。 */
	p = (struct task_struct *) get_free_page();
	if (!p)
		return -EAGAIN;
	task[nr] = p;
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack */
	p->state = TASK_UNINTERRUPTIBLE; /* 先将新进程的状态置为不可中断等待状态，以防止内核调度其执行 */
	p->pid = last_pid; /* 新的进程号，由find_empty_process得到 */
	p->father = current->pid; /* 设置父进程号 */
	p->counter = p->priority; /* 运行时间片值 */
	p->signal = 0; /* 信号位图置 0 */
	p->alarm = 0; /* 报警定时值 */
	p->leader = 0;		/* process leadership doesn't inherit 进程的领导权是不能继承的 */
	p->utime = p->stime = 0; /* 用户态时间和核心态运行时间置 0 */
	p->cutime = p->cstime = 0; /* 子进程用户态和核心态运行时间 */
	p->start_time = jiffies; /* 进程开始运行时间 */
	p->tss.back_link = 0; /* 任务链接域为空 */
	p->tss.esp0 = PAGE_SIZE + (long) p; /* 内核运行时栈顶，该设置在该页的末尾 */
	p->tss.ss0 = 0x10; /* 内核态栈的段选择符 */
	p->tss.eip = eip; /* 指令地址 */
	p->tss.eflags = eflags; /* 标志寄存器 */
	p->tss.eax = 0; /* 这是子进程 fork返回 0 的原因所在 */
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	p->tss.ldt = _LDT(nr); /* 任务的局部描述符表的段选择子 */
	p->tss.trace_bitmap = 0x80000000;
	if (last_task_used_math == current)
		__asm__("clts ; fnsave %0"::"m" (p->tss.i387));
	/* 为新建进程开辟新的页表，把当前进程的页表项复制过去。这时新进程和当前进程指向相同的物理页。 */
	if (copy_mem(nr,p)) {
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}
	for (i=0; i<NR_OPEN;i++)
		if ((f=p->filp[i]))
			f->f_count++;
	if (current->pwd)
		current->pwd->i_count++;
	if (current->root)
		current->root->i_count++;
	if (current->executable)
		current->executable->i_count++;
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));


	/* do this last, just in case 已经可以被调度运行了 
	   有一点需要注意的是，进入 sys_fork 的一瞬间，进程的 ss esp eflags cs eip 会被 CPU 自动保存到内核栈上，
	   当 sys_fork 从内核返回(iretd 指令)，cpu 会根据 ss esp eflags cs eip 找到用户栈以及需要执行下一条用户态指令。

	   新进程被调度（ jmp tss ）的时候，从 TSS 结构体中取出进入 sys_fork 那一瞬间前的寄存器快照复制到 CPU 环境中，（注意这个 TSS 是通过人为构造的）
	   然而不同的是，eip 指令并不是快照，而是 sys_fork 的下一条指令的地址。

	   总结：fork 函数的目的，就是构造 sys_fork 下一条指令的执行时那一刻的 CPU 快照。
	   也就是 mov eax, 2; int 0x80; 执行完时的 CPU 快照。
	*/
	p->state = TASK_RUNNING;	
	return last_pid;
}

int find_empty_process(void)
{
	int i;

	/* 以下这段是获取新的进程号。 */
	repeat:
		if ((++last_pid)<0) last_pid=1; /* 如果last_pid为最大值，比如0x7fffffff, 这时候让last_pid = 1 */

		for(i=0 ; i<NR_TASKS ; i++)
			if (task[i] && task[i]->pid == last_pid) goto repeat;

	/* 寻找空闲项，这里略去了任务0 */
	for(i=1 ; i<NR_TASKS ; i++)
		if (!task[i])
			return i;
	return -EAGAIN;
}
