/*
 *  linux/kernel/system_call.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  system_call.s  contains the system-call low-level handling routines.
 * This also contains the timer-interrupt handler, as some of the code is
 * the same. The hd- and flopppy-interrupts are also here.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call. Ordinary interrupts
 * don't handle signal-recognition, as that would clutter them up totally
 * unnecessarily.
 *
 * Stack layout in 'ret_from_system_call':
 *
 *	 0(%esp) - %eax
 *	 4(%esp) - %ebx
 *	 8(%esp) - %ecx
 *	 C(%esp) - %edx
 *	10(%esp) - %fs
 *	14(%esp) - %es
 *	18(%esp) - %ds
 *	1C(%esp) - %eip
 *	20(%esp) - %cs
 *	24(%esp) - %eflags
 *	28(%esp) - %oldesp
 *	2C(%esp) - %oldss
 */

SIG_CHLD	= 17

EAX		= 0x00
EBX		= 0x04
ECX		= 0x08
EDX		= 0x0C
FS		= 0x10
ES		= 0x14
DS		= 0x18
EIP		= 0x1C
CS		= 0x20
EFLAGS		= 0x24
OLDESP		= 0x28
OLDSS		= 0x2C

state	= 0		# these are offsets into the task-struct.
counter	= 4
priority = 8
signal	= 12
sigaction = 16		# MUST be 16 (=len of sigaction)
blocked = (33*16)

# offsets within sigaction
sa_handler = 0
sa_mask = 4
sa_flags = 8
sa_restorer = 12

nr_system_calls = 72

/*
 * Ok, I get parallel printer interrupts while using the floppy for some
 * strange reason. Urgel. Now I just ignore them.
 */
.globl system_call,sys_fork,timer_interrupt,sys_execve
.globl hd_interrupt,floppy_interrupt,parallel_interrupt
.globl device_not_available, coprocessor_error

.align 2
bad_sys_call:
	movl $-1,%eax
	iret
.align 2
reschedule:
	pushl $ret_from_sys_call
	jmp schedule

/*
	 内核栈的样子：
	|   EIP  | <- esp[0]             esp + 0
	|   CS   | <- esp[1]             esp + 4
	| EFLAGS | <- esp[2]             esp + 8
	|   ESP  | <- esp[3]             esp + c
	|   SS   | <- esp[4]             esp + 10    用户态的栈
	|   ...  | <- esp[5]             esp + 14

	    图1  
*/
	
.align 2
/* 在kernel/sched.c sched_init 中安装的。进入 system_call 时，已经进入内核栈，如上图1  */
system_call:
	cmpl $nr_system_calls-1,%eax # 比较服务号有没有超过最大系统服务号，返回值置 -1 
	ja bad_sys_call # 如果超过，跳转到 bad_sys_call
	push %ds #这些都是用户态的选择子，先保存到内核栈里
	push %es
	push %fs
	pushl %edx
	pushl %ecx		# push %ebx,%ecx,%edx as parameters
	pushl %ebx		# to the system call
/*
	内核栈的样子：
	|   EBX  | <- esp[0]             esp + 0
	|   ECX  | <- esp[1]             esp + 4
	|   EDX  | <- esp[2]             esp + 8
	|   FS   | <- esp[3]             esp + c     这些都是用户态的选择子，先保存到内核栈里
	|   ES   | <- esp[4]             esp + 10
	|   DS   | <- esp[5]             esp + 14
	|   EIP  | <- esp[6]             esp + 18
	|   CS   | <- esp[7]             esp + 1c
	| EFLAGS | <- esp[8]             esp + 20
	|   ESP  | <- esp[9]             esp + 24
	|   SS   | <- esp[10]            esp + 28
	|   ...  | <- esp[11]            esp + 2c

	    图2  
*/
	
	movl $0x10,%edx		# set up ds,es to kernel space  把数据段选择子设置到内核空间
	mov %dx,%ds
	mov %dx,%es
	movl $0x17,%edx		# fs points to local data space 然而 fs 仍然指向用户数据段选择子，这样可以在内核态去访问用户数据空间
	mov %dx,%fs
	call sys_call_table(,%eax,4)   # 调用系统服务
	pushl %EAX                     # 返回值压栈
	movl current,%eax              # 判断当前进程状态是不是 TASK_RUNNING
	cmpl $0,state(%eax)		# state
	jne reschedule                 # 如果不是，重新调度
	cmpl $0,counter(%eax)		# counter 判断时间片是否用完，如果用完就调度
	je reschedule

/*
	内核栈的样子：
	|   EAX  | <- esp[0]             esp + 0
	|   EBX  | <- esp[1]             esp + 4
	|   ECX  | <- esp[2]             esp + 8
	|   EDX  | <- esp[3]             esp + c
	|   FS   | <- esp[4]             esp + 10     这些都是用户态的选择子，先保存到内核栈里
	|   ES   | <- esp[5]             esp + 14
	|   DS   | <- esp[6]             esp + 18
	|   EIP  | <- esp[7]             esp + 1c
	|   CS   | <- esp[8]             esp + 20
	| EFLAGS | <- esp[9]             esp + 24
	|   ESP  | <- esp[10]            esp + 28
	|   SS   | <- esp[11]            esp + 2c
	|   ...  | <- esp[12]            esp + 30

	    图3  
	以下这段代码执行从系统调用 C 函数返回后，对信号进行识别处理。其他中断服务程序退出时将跳到这里进行处理后才退出中断过程。
 */
ret_from_sys_call:
	movl current,%eax		# task[0] cannot have signals task[0] 不进行信号处理
	cmpl task,%eax
	je 3f
	cmpw $0x0f,CS(%esp)		# was old code segment supervisor ? esp + 0x20 判断是不是用户任务，如果不是直接退出中断
	jne 3f
	cmpw $0x17,OLDSS(%esp)		# was stack segment = 0x17 ? esp + 0x2c  如果堆栈不在用户段，也不是用户任务。
	jne 3f
	movl signal(%eax),%ebx  # eax + 0xc task.signal 把当前任务的 signal 位图放到 ebx 中
	movl blocked(%eax),%ecx # 把忽略的信号位图取出
	notl %ecx               # 各位取反
	andl %ebx,%ecx          # 屏蔽掉不考虑的信号
	bsfl %ecx,%ecx          # 从源操作数的的最低位向高位搜索(bsr相反)，将遇到的第一个“1”所在的位序号存入目标寄存器中。若所有位是 0，则ZF=1
	je 3f
	btrl %ecx,%ebx          # 复位该信号，即把 ebx 中该位信号置 0
	movl %ebx,signal(%eax)  # 重新放入当前任务的位图中。
	incl %ecx               # 把信号值调整到 1-32
	pushl %ecx              # 调用 do_signal
	call do_signal
	popl %eax               # 弹出入栈的信号值
3:	popl %eax               # 弹出系统调用的返回值
	popl %ebx
	popl %ecx
	popl %edx
	pop %fs
	pop %es
	pop %ds
	iret                    # 返回到中断调用前

.align 2
coprocessor_error:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	jmp math_error

.align 2
device_not_available:
	push %ds
	push %es
	push %fs
	pushl %edx
	pushl %ecx
	pushl %ebx
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	pushl $ret_from_sys_call
	clts				# clear TS so that we can use math
	movl %cr0,%eax
	testl $0x4,%eax			# EM (math emulation bit)
	je math_state_restore
	pushl %ebp
	pushl %esi
	pushl %edi
	call math_emulate
	popl %edi
	popl %esi
	popl %ebp
	ret

.align 2
/* 
	int 0x20 时钟中断处理程序，频率 100Hz 
	定时芯片 8253、8254 是在 kernel/sched.c 的 sched_init 函数中初始化的。因此这里 jiffies 每 10ms 加 1.
	这段代码将 jiffies 增 1，发送结束中断指令给 8259 控制器，然后用当前特权级作为参数调用 C 函数 do_timer(long CPL)
	当调用返回时转去检测并处理信号
 */
timer_interrupt:
	/* 保护现场 */
	push %ds		# save ds,es and put kernel data space
	push %es		# into them. %fs is used by _system_call
	push %fs
	pushl %edx		# we save %eax,%ecx,%edx as gcc doesn't
	pushl %ecx		# save those across function calls. %ebx
	pushl %ebx		# is saved as we use that in ret_sys_call
	pushl %eax
	/* ds = 0x10, es = 0x10, fs = 0x17 */
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	/* jiffies++ */
	incl jiffies
	/* 
		一时响应了中断，8259 中断控制器无法知道该中断什么时候才能处理结束。同时，如果不清除相应的位，下次从同一个引脚出现的中断将得不到
		处理。在这种情况下，需要中断处理过程显示地对 8259 芯片发送中断结束命令(End Of Interrupt, EOI).
		中断结束命令的代码是 0x20，把这个命令发送给端口 0x20 就行了。如果外部中断是从片处理的，把 0x20 发送给端口 0xa0 
	 */
	movb $0x20,%al		# EOI to interrupt controller #1
	outb %al,$0x20

	/* #define CS 0x20 , 取栈中的 CS 段寄存器的值。*/
	movl CS(%esp),%eax
	/* 拿到 CPL 的值 放到 eax */
	andl $3,%eax		# %eax is CPL (0 or 3, 0=supervisor)
	/* 调用 do_timer(long CPL) 函数，在kernel/sched.c 中实现 */ 
	pushl %eax
	call do_timer		# 'do_timer(long CPL)' does everything from
	addl $4,%esp		# task switching to accounting ...
	/* ret_from_sys_call 在前面定义的一个标签 */
	jmp ret_from_sys_call

.align 2
sys_execve:
	lea EIP(%esp),%eax
	pushl %eax
	call do_execve
	addl $4,%esp
	ret

.align 2
/* 
	sys_fork 调用，用于创建子进程，是system_call的2号服务。原型在include/linux/sys.h中 
	首先调用 C 函数find_empty_process(), 取得一个进程号 pid。若返回负数，则说明目前任务数组已经满。然后调用 copy_process() 复制进程。
		内核栈的样子：
	|   EBX  | <- esp[0]             esp + 0
	|   ECX  | <- esp[1]             esp + 4
	|   EDX  | <- esp[2]             esp + 8
	|   FS   | <- esp[3]             esp + c     这些都是用户态的选择子，先保存到内核栈里
	|   ES   | <- esp[4]             esp + 10
	|   DS   | <- esp[5]             esp + 14
	|   EIP  | <- esp[6]             esp + 18
	|   CS   | <- esp[7]             esp + 1c
	| EFLAGS | <- esp[8]             esp + 20
	|   ESP  | <- esp[9]             esp + 24
	|   SS   | <- esp[10]            esp + 28
	|   ...  | <- esp[11]            esp + 2c

	    图1
 */
sys_fork:
	call find_empty_process #调用 find_empty_process() kernel/fork.c
	testl %eax,%eax #判断返回值是否为 0 。如果为负数，直接退出。
	js 1f
	push %gs     
	pushl %esi
	pushl %edi
	pushl %ebp
	pushl %eax
/* 
		内核栈的样子：
	|   EAX  | <- esp[0]             esp + 0      这些都是用户态的寄存器，先保存到内核栈里
	|   EBP  | <- esp[1]             esp + 4
	|   EDI  | <- esp[2]             esp + 8
	|   ESI  | <- esp[3]             esp + c     
	|   GS   | <- esp[4]             esp + 10
	|   EBX  | <- esp[5]             esp + 14
	|   ECX  | <- esp[6]             esp + 18
	|   EDX  | <- esp[7]             esp + 1c
	|   FS   | <- esp[8]             esp + 20     这些都是用户态的选择子，先保存到内核栈里
	|   ES   | <- esp[9]             esp + 24
	|   DS   | <- esp[10]            esp + 28
	|   EIP  | <- esp[11]            esp + 2c
	|   CS   | <- esp[12]            esp + 30
	| EFLAGS | <- esp[13]            esp + 34
	|   ESP  | <- esp[14]            esp + 38
	|   SS   | <- esp[15]            esp + 3c
	|   ...  | <- esp[16]            esp + 40

	    图1
 */	
	call copy_process
	addl $20,%esp
1:	ret

hd_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0xA0		# EOI to interrupt controller #1
	jmp 1f			# give port chance to breathe
1:	jmp 1f
1:	xorl %edx,%edx
	xchgl do_hd,%edx
	testl %edx,%edx
	jne 1f
	movl $unexpected_hd_interrupt,%edx
1:	outb %al,$0x20
	call *%edx		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

floppy_interrupt:
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	movl $0x17,%eax
	mov %ax,%fs
	movb $0x20,%al
	outb %al,$0x20		# EOI to interrupt controller #1
	xorl %eax,%eax
	xchgl do_floppy,%eax
	testl %eax,%eax
	jne 1f
	movl $unexpected_floppy_interrupt,%eax
1:	call *%eax		# "interesting" way of handling intr.
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret

parallel_interrupt:
	pushl %eax
	movb $0x20,%al
	outb %al,$0x20
	popl %eax
	iret
