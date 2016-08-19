!
! SYS_SIZE is the number of clicks (16 bytes) to be loaded.
! 0x3000 is 0x30000 bytes = 196kB, more than enough for current
! versions of linux
!
SYSSIZE = 0x3000
!
!	bootsect.s		(C) 1991 Linus Torvalds
!
! bootsect.s is loaded at 0x7c00 by the bios-startup routines, and moves
! iself out of the way to address 0x90000, and jumps there.
!
! It then loads 'setup' directly after itself (0x90200), and the system
! at 0x10000, using BIOS interrupts. 
!
! NOTE! currently system is at most 8*65536 bytes long. This should be no
! problem, even in the future. I want to keep it simple. This 512 kB
! kernel size should be enough, especially as this doesn't contain the
! buffer cache as in minix
!
! The loader has been made as simple as possible, and continuos
! read errors will result in a unbreakable loop. Reboot by hand. It
! loads pretty fast by getting whole sectors at a time whenever possible.
! 调试方法 run dbg，然后会在 bochs 里进行调试
!总体上来说，这个程序就是把 setup 模块读取到了 0x90200 处，把 system 读取到了 0x10000 - 0x3ffff 的位置

.globl begtext, begdata, begbss, endtext, enddata, endbss
.text
begtext:
.data
begdata:
.bss
begbss:
.text

SETUPLEN = 4				! nr of setup-sectors
BOOTSEG  = 0x07c0			! original address of boot-sector
INITSEG  = 0x9000			! we move boot here - out of the way
SETUPSEG = 0x9020			! setup starts here
SYSSEG   = 0x1000			! system loaded at 0x10000 (65536).
ENDSEG   = SYSSEG + SYSSIZE		! where to stop loading

! ROOT_DEV:	0x000 - same type of floppy as boot.
!		0x301 - first partition on first drive etc
ROOT_DEV = 0x306
! 设备号=主设备号 << 8 + 次设备号  相当于 dev_no = major << 8 + minor
! 1-内存 2-磁盘 3-硬盘 4-ttyx 5-tty 6-并行口 7-非命名管道
! 0x300 - /dev/hd0 整个第一个硬盘
! 0x301 - /dev/hd1 第一个盘第1个分区
! 0x302 - /dev/hd2 第一个盘第2个分区
! 0x303 - /dev/hd3 第一个盘第3个分区
! 0x304 - /dev/hd4 第一个盘第4个分区
! 0x305 - /dev/hd5 整个第二个硬盘
! 0x306 - /dev/hd6 第二个盘第1个分区
! 0x307 - /dev/hd7 第二个盘第2个分区
! 0x308 - /dev/hd8 第二个盘第3个分区
! 0x309 - /dev/hd9 第二个盘第4个分区
entry _start
_start:
	! 设置数据段为 0x07c0
	mov	ax,#BOOTSEG
	mov	ds,ax

	! 设置附加数据段为  0x9000
	mov	ax,#INITSEG
	mov	es,ax

	! 设置循环次数为 256
	mov	cx,#256
	sub	si,si
	sub	di,di

	! 重复执行 rep 后面的 movw cx 次, movw 等价于 mov word ptr es:[di], word ptr ds:[si]
	! 这句话的意思是说把从 0x7c00 处开始的 512 字节的内容 复制到 0x90000处
	rep
	movw

	! 跳转到 0x9000 : go 这个地方。这条指令执行完了，cs会变成0x9000
	jmpi	go,INITSEG

	!把 ds es ss 都设置成 0x9000
go:	mov	ax,cs
	mov	ds,ax
	mov	es,ax

! put stack at 0x9ff00. 设置当前栈顶为 0x9ff00, MBR的位置在 0x90000 - 0x90200，栈顶远远比这个高。
! 这里注意的是，栈的生长方向是往下生长的。即push一次后，栈顶 sp 的值会变小。
	mov	ss,ax
	mov	sp,#0xFF00		! arbitrary value >>512

! load the setup-sectors directly after the bootblock.
! Note that 'es' is already set up.

! 使用 13H 中断的 02H 服务 AH = 0x02 从第2扇区读入SETUPLEN=4个扇区的数据到 0x9000:0x0200处。CF=0操作成功。
! AL = 扇区数
! CH/CL = 磁道号/扇区号
! DH/DL = 磁头号 / 驱动器号
! ES:BX = 内存缓冲地址
load_setup:
	mov	dx,#0x0000		! drive 0, head 0
	mov	cx,#0x0002		! sector 2, track 0
	mov	bx,#0x0200		! address = 512, in INITSEG
	mov	ax,#0x0200+SETUPLEN	! service 2, nr of sectors
	int	0x13			! read it

	!如果 CF 标志为 1 表示读取失败， 重置后重新读取。
	jnc	ok_load_setup		! ok - continue
	mov	dx,#0x0000
	mov	ax,#0x0000		! reset the diskette
	int	0x13
	j	load_setup

ok_load_setup:

! Get disk drive parameters, specifically nr of sectors/track
	! 13H 号中断的 08H服务，读取驱动器参数
	! DL = 驱动器号， 00H ~ 7FH: 软盘  80H ~ FFH:硬盘
	! CF = 1 操作失败， AH=状态代码。
	! CH = 柱面数的低 8 位， CL的位7-6=柱面数的高2位。CL的位5-0=扇区数 DH=磁头数 DL = 驱动器数 ES:DI = 磁盘驱动器参数表地址
	mov	dl,#0x00
	mov	ax,#0x0800		! AH=8 is get drive parameters
	int	0x13
	mov	ch,#0x00
	! 后面2句等价于 mov cs:sectors, cx , sectors是在后面代码段中定义的一个标签。意思是把cx的内容(扇区数)复制到 sectors里去
	seg cs
	mov	sectors,cx

	!设置 es 为 0x9000
	mov	ax,#INITSEG
	mov	es,ax

! Print some inane message

	! 10H 号中断的 03H 服务，取光标位置 DH/DL  = 光标的起始行/列
	mov	ah,#0x03		! read cursor pos
	xor	bh,bh
	int	0x10
	
	! 10H 号中断的 13H 服务，从指定位置起显示msg1中的字符。
	! BH/BL = 显示页/属性 CX=字符串长度 DH/DL = 行/列 ES:BP=字符串起始逻辑地址。光标的位置在前面3句中已经获取到了
	! AL = 0，用BL属性，光标不动
	! AL = 1，用BL属性，光标移动
	! AL = 2，[字符，属性]，光标不动。表示显示属性和字符串中的字符写在一起。比如 41 07 42 07 表示以属性 07 显示字符A，以07显示字符B
	! AL = 3，[字符，属性]，光标移动动
	mov	cx,#24
	mov	bx,#0x0007		! page 0, attribute 7 (normal)
	mov	bp,#msg1
	mov	ax,#0x1301		! write string, move cursor
	int	0x10

! ok, we've written the message, now
! we want to load the system (at 0x10000)

	! 设置 es 为 0x1000
	mov	ax,#SYSSEG
	mov	es,ax		! segment of 0x010000
	! read_it 函数把磁盘后面（从当前磁道的第6扇区开始）尚未读取的数据读取到 0x1000:0x0000 - 0x3000:0xffff，一共有0x30000字节即192KB的数据。
	! 这部分是 system 模块。
	call	read_it

	! 关闭驱动器马达
	call	kill_motor

! After that we check which root-device to use. If the device is
! defined (!= 0), nothing is done and the given device is used.
! Otherwise, either /dev/PS0 (2,28) or /dev/at0 (2,8), depending
! on the number of sectors that the BIOS reports currently.
	
	! mov ax, cs:root_dev, 执行完后  ax = 0x306
	seg cs
	mov	ax,root_dev

	! 判断 root_dev 是否定义，如果为0，表示未定义。如果已经定义，跳转到 root_defined;
	cmp	ax,#0
	jne	root_defined

	! mov bx, cs:sectors 把当前磁道扇区总数保存到 bx 。
	! 如果根设备为 0x0208, 且当前扇区数为 15 , 则根设备定义为 0x0208
	! 否则如果根设备为 0x021c, 且当前扇区数为 18 , 则根设备定义为 0x021c
	! 如果还不满足，死机。
	seg cs
	mov	bx,sectors
	mov	ax,#0x0208		! /dev/ps0 - 1.2Mb
	cmp	bx,#15
	je	root_defined
	mov	ax,#0x021c		! /dev/PS0 - 1.44Mb
	cmp	bx,#18
	je	root_defined
undef_root:
	jmp undef_root
root_defined:
	seg cs
	mov	root_dev,ax

! after that (everyting loaded), we jump to
! the setup-routine loaded directly after
! the bootblock:

	! jmpi 0x9020:0 这个位置是 setup 模块的起始位置
	jmpi	0,SETUPSEG

! This routine loads the system at address 0x10000, making sure
! no 64kB boundaries are crossed. We try to load it as fast as
! possible, loading whole tracks whenever we can.
!
! in:	es - starting address segment (normally 0x1000)
!
sread:	.word 1+SETUPLEN	! sectors read of current track
head:	.word 0			! current head
track:	.word 0			! current track

read_it:
	! 测试 es = 0x1000 和 0x0fff 相与的结果是不是为0，也就是判断es的低12位是不是全为0，如果不全为0，程序将在 die 处终止
	! 程序要求 es 段必须要在 64KB 的边界上，也就是段起始地址必然在 0x????0000这样的地址的位置
	mov ax,es
	test ax,#0x0fff
die:	jne die			! es must be at 64kB boundary
	! 从此该起， bx 用作一个全局变量，保存 es 的段内偏移。
	xor bx,bx		! bx is starting address within segment
rp_read:
	! ENDSEG = 0x4000
	! 判断 es 是否等于 0x4000。如果 es 小于 0x4000, 跳转到 ok1_read处继续读取，否则退出子程序
	mov ax,es
	cmp ax,#ENDSEG		! have we loaded all yet?
	jb ok1_read
	ret
ok1_read:
	! 下两句相当于 mov ax, cs:sectors。 即拿到磁盘扇区总数，放入 ax
	seg cs
	mov ax,sectors

	! sread的初始值是 5 ，表示已经读取的扇区数。前面已经读取的 1 个扇区的 MBR + 4 个扇区的 SETUP，一共是5个扇区
	! 这两句的意思是计算当前磁道还有几个扇区没有读取，计算完后 cx = 0x0d ,也就是还有 13 个扇区的数据没读取。
	sub ax,sread
	mov cx,ax

	! 将 sectors - sread 的结果 逻辑左移 9 位，cx = 0x000d -> cx = 0x1a00，相当于cx = cx * 512
	！实际就是计算这 13 个扇区的数据的字节数
	shl cx,#9

	! bx为当前段内偏移，表示数据已经复制到了 es:bx 的位置了。
	! cx 表示当前磁道未读取的字节数，bx表示当前段内偏移，要记住，一个段最多只能保存 64KB 的数据。
	! 如果段内偏移 + 当前磁道里尚未读取的字节的总和超过 64KB，直接读取是危险的。
	! 这时候，再来计算，当前段能够承载的数据量是多少
	add cx,bx
	jnc ok2_read
	je ok2_read
	! 看当前段能够承载的数据量，保存到 ax 里。
	xor ax,ax
	sub ax,bx
	shr ax,#9
ok2_read:
	! 读取 track 磁道剩余未读取的扇区，即从 sectors + 1 处读取 al 个扇区。从当前环境看，应该是从第 6 个扇区读取 13 个扇区的数据。
	call read_track

	! 把当前读取的扇区数保存到 cx , 然后把 sread 的值累计到 ax, 这时候 ax = 13 + 5 = 18
	mov cx,ax
	add ax,sread

	! 后面 2 句等价于 cmp ax, cs:sectors. 判断累计读取的扇区数是不是正好为 18，如果不是，进入ok3_read。否则，读取下一个磁头，进入ok4_read
	seg cs
	cmp ax,sectors
	jne ok3_read
	! 因为磁头号不是 0 就是 1，如果当前磁头号是 1，下面 2 句执行后，下一个要读取的磁头号是 0.如果当前磁头号是 0， 下一次读取的磁头号是 1
	mov ax,#1
	sub ax,head
	jne ok4_read
	inc track
ok4_read:
	! 当前磁头号切换。
	mov head,ax
	xor ax,ax
ok3_read:
	! 更新已经读取的扇区。
	mov sread,ax

	! 在 ok2_read 后的第二句，cx的值被保存为当前读取的扇区数。这句把 cx 转换为字节数。
	shl cx,#9

	! 把读取的字节数累计到 bx, bx 表示的是 es 段内的偏移，只要这个累加没有超过 0xffff, 即未进位，那就跳到 rp_read 处继续。
	! 否则，es = es + 0x1000. 程序的意思是利用 es:bx 累计一共读到的数据的总的字节数。一旦 es = 0x4000 就不再读取。
	add bx,cx
	jnc rp_read
	mov ax,es
	add ax,#0x1000
	mov es,ax
	xor bx,bx
	jmp rp_read

read_track:
	! 以下4句是现场保护
	push ax
	push bx
	push cx
	push dx

	! 拿到当前磁道
	mov dx,track

	! 拿到当前已经读取的扇区数
	mov cx,sread
	inc cx

	! 使用 13H 中断的 02H 服务 AH = 0x02 从第 cl 扇区读入SETUPLEN=4个扇区的数据到 0x9000:0x0000处。CF=0操作成功。
	! AL = 扇区数, 这个参数
	! CH/CL = 磁道号/扇区号
	! DH/DL = 磁头号 / 驱动器号
	! ES:BX = 内存缓冲地址
	! 返回 AL = 传输的扇区数
	mov ch,dl
	mov dx,head
	mov dh,dl
	mov dl,#0
	and dx,#0x0100
	mov ah,#2
	int 0x13

	!如果CF=1则失败，跳转到 bad_rt 重置并重新调用子程序 read_track
	jc bad_rt
	pop dx
	pop cx
	pop bx
	pop ax
	ret
bad_rt:	mov ax,#0
	mov dx,#0
	int 0x13
	pop dx
	pop cx
	pop bx
	pop ax
	jmp read_track

!/*
! * This procedure turns off the floppy drive motor, so
! * that we enter the kernel in a known state, and
! * don't have to worry about it later.
! */
kill_motor:
	push dx
	mov dx,#0x3f2
	mov al,#0
	outb
	pop dx
	ret

sectors:
	.word 0

msg1:
	.byte 13,10
	.ascii "Loading system ..."
	.byte 13,10,13,10

.org 508
root_dev:
	.word ROOT_DEV
boot_flag:
	.word 0xAA55

.text
endtext:
.data
enddata:
.bss
endbss:
