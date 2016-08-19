!
!	setup.s		(C) 1991 Linus Torvalds
!
! setup.s is responsible for getting the system data from the BIOS,
! and putting them into the appropriate places in system memory.
! both setup.s and system has been loaded by the bootblock.
!
! This code asks the bios for memory/disk/other parameters, and
! puts them in a "safe" place: 0x90000-0x901FF, ie where the
! boot-block used to be. It is then up to the protected mode
! system to read them from there before the area is overwritten
! for buffer-blocks.
!

! NOTE! These had better be the same as in bootsect.s!

! 总的来说，这段代码做了如下几件事：
! 0. 获取了一些硬件参数的信息保存到了 0x90000 - 0x90200 这一段空间。具体看代码。
! 1. 把 system 模块从 0x10000 - 0x8ffff 的位置复制到了 0x00000 - 0x7ffff
! 2. 初始化了一个空的 中断向量 表并安装
! 3. 安装了一个 GDT 表。含有一个代码段表项和一个数据段表项。
! 4. 修改了 8259A 能够发出的中断向量号，防止和 CPU 的异常向量号冲突。
! 5. 进入进入保护模式，并跳转到绝对地址 0x00000000 处。

INITSEG  = 0x9000	! we move boot here - out of the way
SYSSEG   = 0x1000	! system loaded at 0x10000 (65536).
SETUPSEG = 0x9020	! this is the current segment

.globl begtext, begdata, begbss, endtext, enddata, endbss
.text
begtext:
.data
begdata:
.bss
begbss:
.text

entry start
start:

! ok, the read went well so we get current cursor position and save it for
! posterity.

	! 设置 ds = 0x9000 尽管这个工作在 bootsect 中已经做了。
	mov	ax,#INITSEG	! this is done in bootsect already, but...
	mov	ds,ax
	! 10H 中断的 03H 服务，读取光标位置到 0x9000:0, 即 0x90000 处，这个位置本来是bootsect的代码，但是现在已经没有用了。
	mov	ah,#0x03	! read cursor pos
	xor	bh,bh
	int	0x10		! save it in known place, con_init fetches

	! 程序中跟踪可以看到 dx = 0x1300，即第19行第0列。情况确实如此。
	mov	[0],dx		! it from 0x90000.
! Get memory size (extended mem, kB)

	! 15H 号中断（杂项系统服务） 88H 号服务。
	! 功能：读取扩展内存大小
	! 入口参数：AH = 88H
	! 出口参数：AX = 扩展内存字节数，单位是 KB 
	! 程序中设置的内存总大小为16MB，所以扩展内存为 15MB。结果显示 AX=0x3c00 正好是15MB
	mov	ah,#0x88
	int	0x15
	mov	[2],ax

! Get video-card data:
	! 取当前显未器模式
	! 出口参数：AH = 屏幕字符的列数 AL = 显示模式 BH = 页码
	mov	ah,#0x0f
	int	0x10
	mov	[4],bx		! bh = display page
	mov	[6],ax		! al = video mode, ah = window width

! check for EGA/VGA and some config parameters

	mov	ah,#0x12
	mov	bl,#0x10
	int	0x10
	mov	[8],ax
	mov	[10],bx
	mov	[12],cx

! Get hd0 data
	! 取第一个硬盘的信息，硬盘参数表在中断向量表中。参数表的长度是 16 个字节
	! 第一个硬盘参数表的首地址是中断向量表中 0x41号索引处的向量值！
	! 
	mov	ax,#0x0000
	mov	ds,ax

	! lds reg, mem ，把 mem 指向的连续 4 字节的数据低 2 字节放在 reg 中，  高 2 字节放在 ds 中，
	! 等价于 ds:reg = dword ptr ds:[mem]
	lds	si,[4*0x41]

	
	! 实际环境中得到的值是 0x9fc0003d，然后从这个地址处的内容的连续16字节复制到 0x9000:0x0080 即 0x90080 处
	! 参数表的内容是：
	! cc 00 10 00 00 ff ff 00 
	! c8 00 00 00 cc 00 26 00
	mov	ax,#INITSEG
	mov	es,ax
	mov	di,#0x0080
	mov	cx,#0x10
	rep
	movsb

! Get hd1 data
	! 取hd1的参数表 
	! 实际环境中得到的值是 0x9fc0004d 
	mov	ax,#0x0000
	mov	ds,ax
	lds	si,[4*0x46]
	mov	ax,#INITSEG
	mov	es,ax
	mov	di,#0x0090
	mov	cx,#0x10
	rep
	movsb

! Check that there IS a hd1 :-)
	! 检查是否有第2块硬盘，如果没有则把第2个表清空
	! 13H 号中断 15H 号服务：读取磁盘类型
	! 入口参数：AH = 15H DL = 驱动器，00 ~ 7F：软盘；80 ~ FF:硬盘
	! 出口参数：如果 CF = 1 失败 AH = 状态代码 否则 CF = 0
	!  AH = 00H 未安装驱动器 AH = 01H 无改变线支持的软盘驱动器 AH = 02H 带有改变线支持的软盘驱动器 AH = 03H 硬盘， CX:DX = 512字节的扇区数
	! 实际环境中 CF = 1，表明没有这块磁盘。跳转到 no_disk1 清空第二张硬盘参数表。
	mov	ax,#0x01500
	mov	dl,#0x81
	int	0x13
	jc	no_disk1
	cmp	ah,#3
	je	is_disk1
no_disk1:
	mov	ax,#INITSEG
	mov	es,ax
	mov	di,#0x0090
	mov	cx,#0x10
	mov	ax,#0x00
	rep
	stosb
is_disk1:

! 下面准备进入保护模式！！！
! 关闭中断！！！！！！！！！
! now we want to move to protected mode ...

	cli			! no interrupts allowed !

! first we move the system to it's rightful place
	! 准备将 system 模块从 0x10000-0x90000 移动到 0x00000-0x80000的位置.注意这个步骤把BIOS中断向量表覆盖掉了。
	! while(ax + 0x1000 != 0x9000) {
	!	es = ax;
	!	ds = ax + 0x1000;
	!	copy from ds:[0000-ffff] to es:[0000-ffff]
	! }
	mov	ax,#0x0000
	cld			! 'direction'=0, movs moves forward
do_move:
	mov	es,ax		! destination segment
	add	ax,#0x1000
	cmp	ax,#0x9000
	jz	end_move
	mov	ds,ax		! source segment
	sub	di,di
	sub	si,si
	! 0x8000 * 2 = 0x10000 一次复制一个段 64KB 
	mov 	cx,#0x8000
	rep
	movsw
	jmp	do_move

! then we load the segment descriptors

end_move:
	! SETUPSEG = 0x9020 让 ds = 0x9020，因为后面要用到代码段的数据
	mov	ax,#SETUPSEG	! right, forgot this at first. didn't work :-)
	mov	ds,ax

	! [idt_48] = 00 00 00 00 00 00
	!            ^     ^
	!            limit base
	! lidt ds:[idt_48]

	lidt	idt_48		! load idt with 0,0

	! [gdt_48] = 00 08 14 03 09 00
	!            ^     ^
	!            limit base
	! limit = 0x0800 base = 0x00090314，允许安装256个表项
	! 为什么界限值不是 0x7ff ?
	! 这个表暂时有两个表项，gdt[1]是DPL=0的8MB代码段，段基址是0 gdt[2]是DPL=0的8MB数据段，段基址是0
	! lidt ds:[idt_48]
	lgdt	gdt_48		! load gdt with whatever appropriate

! that was painless, now we enable A20

	! 测试键盘缓冲区是否空，只有空的时候，这个函数才返回
	call	empty_8042
	! 向端口 0x64 写命令 0xd1 (11010001) 
	mov	al,#0xD1		! command write
	out	#0x64,al
	! 等待缓冲器空，看命令是否被接受
	call	empty_8042 

	! 开启A20地址线，向端中 0x60 写入命令 11011111
	mov	al,#0xDF		! A20 on
	out	#0x60,al
	call	empty_8042

! well, that went ok, I hope. Now we have to reprogram the interrupts :-(
! we put them right after the intel-reserved hardware interrupts, at
! int 0x20-0x2F. There they won't mess up anything. Sadly IBM really
! messed this up with the original PC, and they haven't been able to
! rectify it afterwards. Thus the bios puts interrupts at 0x08-0x0f,
! which is used for the internal hardware interrupts as well. We just
! have to reprogram the 8259's, and it isn't fun.
! 首先 8259A 在这里是级联的，和处理器直接相连的是主片，和主片相连的是从片
! 主片的中断向量为 0x08-0x0f, 从片是 0x70-0x77。IBM把这些弄的乱七八糟。。。之后也没纠正
! 因为在8086为处理器的系统中，这没有什么问题。在32位处理器上，0x08-0x0f 已经被处理器用做异常向量。
! 好在8259A是可编程的，允许重新设置中断向量。
! 根据Intel公司的建议，中断向量 0x20-0xff（32-255）是用户可以自由分配的部分。
! 下面这段代码就是把主片的中断向量改为 0x20-0x27 从片改为 0x28-0x2f
	mov	al,#0x11		! initialization sequence
	out	#0x20,al		! send it to 8259A-1
	.word	0x00eb,0x00eb		! jmp $+2, jmp $+2
	out	#0xA0,al		! and to 8259A-2
	.word	0x00eb,0x00eb
	mov	al,#0x20		! start of hardware int's (0x20)
	out	#0x21,al
	.word	0x00eb,0x00eb
	mov	al,#0x28		! start of hardware int's 2 (0x28)
	out	#0xA1,al
	.word	0x00eb,0x00eb
	mov	al,#0x04		! 8259-1 is master
	out	#0x21,al
	.word	0x00eb,0x00eb
	mov	al,#0x02		! 8259-2 is slave
	out	#0xA1,al
	.word	0x00eb,0x00eb
	mov	al,#0x01		! 8086 mode for both
	out	#0x21,al
	.word	0x00eb,0x00eb
	out	#0xA1,al
	.word	0x00eb,0x00eb
	! 屏蔽主片和从片所有的中断请求
	mov	al,#0xFF		! mask off all interrupts for now
	out	#0x21,al
	.word	0x00eb,0x00eb
	out	#0xA1,al

! well, that certainly wasn't fun :-(. Hopefully it works, and we don't
! need no steenking BIOS anyway (except for the initial loading :-).
! The BIOS-routine wants lots of unnecessary data, and it's less
! "interesting" anyway. This is how REAL programmers do it.
!
! Well, now's the time to actually move into protected mode. To make
! things as simple as possible, we do no register set-up or anything,
! we let the gnu-compiled 32-bit programs do that. We just jump to
! absolute address 0x00000, in 32-bit protected mode.
	! lmsw Load Machine Status Word
	! Intel公司建议使用 mov cr0, ax 切换到保护模式
	! jmpi 远跳指令用于刷新CPU当前指令队列，并跳转到绝对地址 0x00000000 处。
	mov	ax,#0x0001	! protected mode (PE) bit
	! 执行完后 CR0 从 0x60000010 变成 0x60000011 
	lmsw	ax		! This is it!
	! 关键性的一跳，转入 head.s 模块。这个时候这个 8 已经代表段选择子了，不再是实模式下的左移16位的含义了。
	jmpi	0,8		! jmp offset 0 of segment 8 (cs)

! This routine checks that the keyboard command queue is empty
! No timeout is used - if this hangs there is something wrong with
! the machine, and we probably couldn't proceed anyway.
empty_8042:
	! 用来测试键盘缓冲区是否为空，如果不空就一直循环，直到为空时返回
	! 下面这两行就是 jmp 到下一条指令，相当于延时操作
	.word	0x00eb,0x00eb
	in	al,#0x64	! 8042 status port
	test	al,#2		! is input buffer full?
	jnz	empty_8042	! yes - loop
	ret

gdt:
	.word	0,0,0,0		! dummy

	.word	0x07FF		! 8Mb - limit=2047 (2048*4096=8Mb)
	.word	0x0000		! base address=0
	.word	0x9A00		! code read/exec
	.word	0x00C0		! granularity=4096, 386

	.word	0x07FF		! 8Mb - limit=2047 (2048*4096=8Mb)
	.word	0x0000		! base address=0
	.word	0x9200		! data read/write
	.word	0x00C0		! granularity=4096, 386

idt_48:
	.word	0			! idt limit=0
	.word	0,0			! idt base=0L

gdt_48:
	.word	0x800		! gdt limit=2048, 256 GDT entries 为什么不是 0x7ff
	.word	512+gdt,0x9	! gdt base = 0X9xxxx
	
.text
endtext:
.data
enddata:
.bss
endbss:
