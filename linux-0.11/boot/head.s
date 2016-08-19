/*
 *  linux/boot/head.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  head.s contains the 32-bit startup code.
 *
 * NOTE!!! Startup happens at absolute address 0x00000000, which is also where
 * the page directory will exist. The startup code will be overwritten by
 * the page directory.
 * 注意！！！32 位启动代码是从绝对地址 0x00000000 开始的，这里也同样是页目录将存在的地方
 * 因此这里的启动代码将被页目录覆盖掉
 */

/*
   总体上看，这 head.s 干了这么几件事：
   0. 重新安装了 idt 表和 gdt 表
   1. 检测 A20 地址线是否已经开启
   2. 检测数学协处理器是否可用
   3. 安装页目录和页表并开启分页，把线性地址 0x00000000-0x00ffffff 映射到物理地址的 0x00000000-0x00ffffff
   4. 跳转到 init/main.c 中的 main 函数去执行。
 */

.text
.globl idt,gdt,pg_dir,tmp_floppy_area
pg_dir:
.globl startup_32
startup_32:
	/* 设置 ds, es, fs, gs 为 0x10, 即GDT表中的第2项，RPL = 0，数据段选择子*/
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	mov %ax,%fs
	mov %ax,%gs

	/* 表示把 _stack_start 的值加载到 ss:esp, 用于设置系统堆栈。stack_start定义在 kernel/sched.c 中
	 * ss = 0x10 esp = 0x00021f00
	 * esp 的值是 user_stack 这个数组最后一个元素的下一个位置的地址
	 * long user_stack[2048] 也在 kernel/sched.c 中定义，esp 指向的是 user_stack + 2048
	 */
	lss stack_start,%esp

	/* 重新安装 idt 表和 gdt 表 */
	call setup_idt
	call setup_gdt

	/* 重新加载段寄存器 */
	/* 
		使用 bochs 跟踪观察，如果不对CS再次执行加载，那么在执行到 26 行时 CS 代码段
		不可见部分中的限长还是 8MB。这样看来应该要重新加载 CS。但 setup.s 中的内核代码
		段描述符与新的 gdt 表中的描述符除了段限长以外其余部分完全一样，8MB 的限长在内
		核初始化阶段不会有问题。而且在以后内核执行过程中段间跳转时会重新加载 CS 。因此
		这里没有加载它，程序也不会出错。
		针对这个问题，目前的内核在这里添加了一条工跳转指令 ljmp $(_KERNEL_CS), $1f 
		以确保CS被重新加载
	*/
	movl $0x10,%eax		# reload all the segment registers
	mov %ax,%ds		# after changing gdt. CS was already
	mov %ax,%es		# reloaded in 'setup_gdt'
	mov %ax,%fs
	mov %ax,%gs
	lss stack_start,%esp

	/* 检测 A20 地址线是否已经开启，功能和下面这段 C 代码一样。 */
	/* 
		如果地址 0x000000 和 0x100000 处的内容一直一样，说明 A20 地址线没有开启
		eax = 0;
		do {
			++eax;
			*(0x000000) = eax;
		}while(*(0x100000) == *(0x000000));
	 */
	xorl %eax,%eax
1:	incl %eax		# check that A20 really IS enabled
	movl %eax,0x000000	# loop forever if it isn't
	cmpl %eax,0x100000
	je 1b

/*
 * NOTE! 486 should set bit 16, to check for write-protect in supervisor
 * mode. Then it would be unnecessary with the "verify_area()"-calls.
 * 486 users probably want to set the NE (#5) bit also, so as to use
 * int 16 for math errors.
 */

 /*
 	cr0 = 0x60000011
 	下面这段程序把 cr0 的 MP 位置 1 后调用  check_x87
  */
	movl %cr0,%eax		# check math chip
	andl $0x80000011,%eax	# Save PG,PE,ET
/* "orl $0x10020,%eax" here for 486 might be good */
	orl $2,%eax		# set MP
	movl %eax,%cr0
	call check_x87
	jmp after_page_tables

/*
 * We depend on ET to be correct. This checks for 287/387.
 */
check_x87:
	fninit  /* 向协处理器发出初始化指令 */
	fstsw %ax    /* 取协处理器状态字到 ax 寄存器中 */
	cmpb $0,%al /* 初始化后状态字应该为 0, 否则说明协处理器不存在。存在就前跳到标号 1 处，否则改写 cr0。实际环境中是存在的。 */
	je 1f			/* no coprocessor: have to set bits */
	movl %cr0,%eax
	xorl $6,%eax		/* reset MP, set EM  设置 EM=1 MP=0*/
	movl %eax,%cr0
	ret
.align 2
1:	.byte 0xDB,0xE4		/* fsetpm for 287, ignored by 387。287 协处理码*/
	ret

/*
 *  setup_idt
 *
 *  sets up a idt with 256 entries pointing to
 *  ignore_int, interrupt gates. It then loads
 *  idt. Everything that wants to install itself
 *  in the idt-table may do so themselves. Interrupts
 *  are enabled elsewhere, when we can be relatively
 *  sure everything is ok. This routine will be over-
 *  written by the page tables.
 */
setup_idt:
	/*
	                  |-中断门-|
	 |    高 2 字节   |8---E---|                          |    低 2 字节   |
	 |<-中断过程偏移->|10001110|000-----|<---段选择子---->|<-中断过程偏移->|
	 |----------------|--------|--------|-----------------|----------------|
	                   ^^ ^        ^     
	                   P|DPL|      |不使用	
	   TYPE 字段：
	   5: 任务门
	   C: 调用门
	   E: 中断门
	   F: 陷阱门

	 */
	/* 取 ignore_int 函数的地址，放到 edx，ignore_int 在后面有定义。*/
	lea ignore_int,%edx
	movl $0x00080000,%eax
	movw %dx,%ax		/* selector = 0x0008 = cs */
	/* 到这里 eax = 0x00085428 ，0x5428 是ignore_int函数的地址，eax 将组成 idt 表项的低 4 字节*/ 

	/* dx = 0x8e00 ， edx 将组成 idt 表项的高 4 字节 */
	movw $0x8E00,%dx	/* interrupt gate - dpl=0, present */


	/* 拿到 idt 表的地址，放入 edi, &idt = 0x54b8 */
	lea idt,%edi

	/* 执行 256 次循环，填充 idt 表项 */
	mov $256,%ecx
rp_sidt:
	movl %eax,(%edi)
	movl %edx,4(%edi)
	addl $8,%edi
	dec %ecx
	jne rp_sidt

	/* 把 idt 表的信息加载到 idtr 寄存器，完成后 idtr:base=0x000054b8, limit=0x07ff */
	lidt idt_descr
	ret

/*
 *  setup_gdt
 *
 *  This routines sets up a new gdt and loads it.
 *  Only two entries are currently built, the same
 *  ones that were built in init.s. The routine
 *  is VERY complicated at two whole lines, so this
 *  rather long comment is certainly needed :-).
 *  This routine will beoverwritten by the page tables.
 */
setup_gdt:
	/* 安装 gdt 表，把 gdt 表的信息加载到 gdtr 中，完成后，gdtr:base=0x00005cb8, limit=0x07ff */
	lgdt gdt_descr
	ret

/*
 * I put the kernel page tables right after the page directory,
 * using 4 of them to span 16 Mb of physical memory. People with
 * more than 16MB will have to expand this.
 */
.org 0x1000
pg0:

.org 0x2000
pg1:

.org 0x3000
pg2:

.org 0x4000
pg3:

.org 0x5000
/*
 * tmp_floppy_area is used by the floppy-driver when DMA cannot
 * reach to a buffer-block. It needs to be aligned, so that it isn't
 * on a 64kB border.
 */
tmp_floppy_area:
	.fill 1024,1,0

after_page_tables:
	/*
	stack:

	| main|<- esp
	|  L6 |<- esp + 4
	|  0  |<- esp + 8
	|  0  |<- esp + a
	|  0  |<- esp + c
	| ... |
	main = 0x00006684
	L6 =   0x00005412
	*/
	pushl $0		# These are the parameters to main :-)
	pushl $0        #这些是main函数的参数，init/main.c
	pushl $0
	pushl $L6		# return address for main, if it decides to.
	pushl $main
	jmp setup_paging #跳转到 setup_paging
L6:
	jmp L6			# main should never return here, but
				# just in case, we know what happens.

/* This is the default interrupt "handler" :-) */
int_msg:
	.asciz "Unknown interrupt\n\r"
.align 2
ignore_int:
	/* 现场保护 */
	pushl %eax
	pushl %ecx
	pushl %edx
	push %ds
	push %es
	push %fs
	/* 调用 printk 函数，打印 int_msg */
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	mov %ax,%fs
	pushl $int_msg
	call printk
	popl %eax

	/* 恢复现场 */
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret


/*
 * Setup_paging
 *
 * This routine sets up paging by setting the page bit
 * in cr0. The page tables are set up, identity-mapping
 * the first 16MB. The pager assumes that no illegal
 * addresses are produced (ie >4Mb on a 4Mb machine).
 *
 * NOTE! Although all physical memory should be identity
 * mapped by this routine, only the kernel page functions
 * use the >1Mb addresses directly. All "normal" functions
 * use just the lower 1Mb, or the local data space, which
 * will be mapped to some other place - mm keeps track of
 * that.
 *
 * For those with more memory than 16 Mb - tough luck. I've
 * not got it, why should you :-) The source is here. Change
 * it. (Seriously - it shouldn't be too difficult. Mostly
 * change some constants etc. I left it at 16Mb, as my machine
 * even cannot be extended past that (ok, but it was cheap :-)
 * I've tried to show which constants to change by having
 * some kind of marker at them (search for "16Mb"), but I
 * won't guarantee that's all :-( )
 */
.align 2
setup_paging:
	movl $1024*5,%ecx		/* 5 pages - pg_dir+4 page tables 一共5页，页目录 + 4 张页表，一张页表有 1024 个页表项。一个页表项是 4 字节*/
	xorl %eax,%eax
	xorl %edi,%edi			/* pg_dir is at 0x000 准备把页目录设置在 0x00000000 处*/
	cld;rep;stosl /* 把这 5 页初始化为 0 */
	/*
	 线性地址的 10-10-12 机制
	 高 10 位是在页目录中的索引号，中间 10 位是在页表中的索引号，低 12 位是页内偏移。
	 无论是页目录还是页表，表项中存储的都是页基址，因为页表页目录也是在页中存放的啊。
	 当然这4字节中，高 20 位是页基址的高 20 位，剩下的低 12 位是属性。
	 页基址的低 12 位一定是 0 .所以，无论是页目录还是页表中的项，和 0xfffff000 相与后就是 页基址。
	 利用页目录和页表，可以找到内存页的基址。然后把这个内存页基址+12位页内偏移，就可以找到物理地址。

	 pg0 0x1000
	 pg1 0x2000
	 pg2 0x3000
	 pg3 0x4000

	 这些标签在前面已经定义了

	 页表项的结构：
	  31                                   12       8 7
	 |- - - - - - - -|- - - - - - - -|- - - -|- - - -|- - - - - - - -|
	 |--------页表物理基址的 31 - 12 --------|A V L|G|0 D A P P U R P
	                                                        C W S W
	                                                        D T

	 页属性
	 US = 用户/管理位。为 1 时，允许所有特权级别的程序访问；为 0 时，特权级 3 的程序不能访问
	 RW = 为 0 表示只读，为 1 表示可读可写
	 P = 存在位。1 表示页位于内存中，0 表示不在内存中。
	 */
	movl $pg0+7,pg_dir		/* set present bit/user r/w 加 7 表示把属性中的 US RW 和 P 设置为1*/
	movl $pg1+7,pg_dir+4		/*  --------- " " --------- */
	movl $pg2+7,pg_dir+8		/*  --------- " " --------- */
	movl $pg3+7,pg_dir+12		/*  --------- " " --------- */
	movl $pg3+4092,%edi
	movl $0xfff007,%eax		/*  16Mb - 4096 + 7 (r/w user,p) 页表基址 0x00fff000，即 16MB - 4096B 的位置。。*/
	/* 每个页表可以索引 1024 页，即4MB的空间，16MB的空间正好用4个页表就可以完全索引。
	/*从 pg3 的最后一个表项开始填充，一直把 pg0-pg3的所有表项填满*/
	std /* 每执行一次 edi 减 4 */
	/*  等价于 
		mov dword ptr es:[edi], eax
		sub edi, 4 
		Intel 汇编语法 
	*/
1:	stosl			/* fill pages backwards - more efficient :-) */

	subl $0x1000,%eax
	jge 1b

	/* 这时候在 bochs 里 info tab 会提示页机制被关闭*/
	xorl %eax,%eax		/* pg_dir is at 0x0000 */
	movl %eax,%cr3		/* cr3 - page directory start */
	movl %cr0,%eax
	orl $0x80000000,%eax
	movl %eax,%cr0		/* set paging (PG) bit */
	/* 到这里，线性地址 0x00000000-0x00ffffff ->  0x00000000-0x00ffffff 物理地址*/
	ret			/* this also flushes prefetch-queue */
	/* 刷新CPU的指令队列 这里用的 ret ,注意，这时候栈顶存放的是 init/main.c中的main函数的地址，所以ret后就进入 main 函数了*/
	/*
	 ret 前的堆栈：
		| main|<- esp
		|  L6 |<- esp + 4
		|  0  |<- esp + 8
		|  0  |<- esp + a
		|  0  |<- esp + c
		| ... |
	 ret 后的堆栈，同时程序进入main函数：
		|  L6 |<- esp
		|  0  |<- esp + 4
		|  0  |<- esp + 8
		|  0  |<- esp + a
		| ... |
	 这时候，可以看到栈顶为 L6，这是一个死循环函数的地址，也是 main 函数 return 后的地址。
	 也就是说，main 函数如果返回了，就死循环了。
	*/
.align 2
.word 0
idt_descr:
	/*限长：总的字节数减一，这里一共可以装 256 个表项*/
	.word 256*8-1		# idt contains 256 entries

	/* idt 表基址 */
	.long idt
.align 2
.word 0
gdt_descr:
	/* 这里可以装 256 个表项 */
	.word 256*8-1		# so does gdt (not that that's any
	/* gdt 表基址 */
	.long gdt		# magic number, but it works for me :^)

	.align 8
	/* 填充 256 个项，每个项 8 字节，全部填 0
idt:	.fill 256,8,0		# idt is uninitialized

gdt:	
	/*
	                 |C       0       9       A      |
	                 |1 1 0 0 0 0 0 0 1 0 0 1 1 0 1 0|                 31           24 23           16 15            8 7             0
	 |- - - - - - - -|- - - - - - - -|- - - - - - - -|- - - - - - - -|- - - - - - - -|- - - - - - - -|- - - - - - - -|- - - - - - - -|
	                  ^ ^ ^ ^ ^       ^ ^   ^ ^    
	 |------base-----|G D L A l       P D   S T      |----------------------base---------------------|-------------limit-------------|
	                    /   V i         P     Y
	                    B   L m         L     P
	                          i               E
	                          t
	 
	  TYPE字段：
	  0: 只读
	  2: 读写
	  4: 只读，向下扩展
	  6: 读写，向下扩展

	  8: 只执行
	  A: 可读可执行
	  C: 只执行，一致代码段
	  E: 可读可执行，一致代码段
	 */

	.quad 0x0000000000000000	/* NULL descriptor */
	.quad 0x00c09a0000000fff	/* 16Mb base=0x00000000 code 可读可执行 */
	.quad 0x00c0920000000fff	/* 16Mb base=0x00000000 data 可读可写*/
	.quad 0x0000000000000000	/* TEMPORARY - don't use */
	/*           ^          */
	/*           这个位置是TYPE字段         */
	.fill 252,8,0			/* space for LDT's and TSS's etc */
