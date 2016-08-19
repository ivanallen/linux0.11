/*
 *  linux/kernel/asm.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * asm.s contains the low-level code for most hardware faults.
 * page_exception is handled by the mm, so that isn't here. This
 * file also handles (hopefully) fpu-exceptions due to TS-bit, as
 * the fpu must be properly saved/resored. This hasn't been tested.
 */

.globl divide_error,debug,nmi,int3,overflow,bounds,invalid_op
.globl double_fault,coprocessor_segment_overrun
.globl invalid_TSS,segment_not_present,stack_segment
.globl general_protection,coprocessor_error,irq13,reserved

/*
	有错误代码：            无错误代码：
	|错误代码| <- esp[0]    |   EIP  | <- esp[0]             esp + 0
	|   EIP  | <- esp[1]    |   CS   | <- esp[1]             esp + 4
	|   CS   | <- esp[2]    | EFLAGS | <- esp[2]             esp + 8
	| EFLAGS | <- esp[3]    |   ESP  | <- esp[3]             esp + c
	|   ESP  | <- esp[4]    |   SS   | <- esp[4]             esp + 10
	|   SS   | <- esp[5]    |   ...  | <- esp[5]             esp + 14
	|   ...  |
	    图1                      图2
*/

/* 进入到 pushl $do_divide_error 这个位置的时候， esp指向错误代码处，看图2 */
divide_error:
	pushl $do_divide_error


/* 这是没有错误码的异常的入口 */
no_error_code:
	xchgl %eax,(%esp)
	pushl %ebx
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp
	push %ds
	push %es
	push %fs
	pushl $0		# "error code"
	lea 44(%esp),%edx   # esp + 2c 的位置， edx = esp + 2c，就是 EIP 那个位置
/*
	|   0    | <- esp + 0   这里是自定义的一个 error_code
	|   fs   | <- esp + 4
	|   es   | <- esp + 8
	|   ds   | <- esp + c
	|   ebp  | <- esp + 10
	|   esi  | <- esp + 14
 	|   edi  | <- esp + 18
	|   edx  | <- esp + 1c
	|   ecx  | <- esp + 20
	|   ebx  | <- esp + 24                             
	|   eax  | <- esp + 28  这里是C函数的地址。这里和 eax 进行了交换, 这时 eax = &divide_error, 这个单元变成了 eax，变相的保存了 eax 的值
	|   EIP  | <- esp + 2c  
	|   CS   | <- esp + 30
	| EFLAGS | <- esp + 34
	|   ESP  | <- esp + 38
	|   SS   | <- esp + 3c
	        图3

*/
	pushl %edx
/*
执行到这里，堆栈如图所示：

  --|  &EIP  | <- esp + 0   这里存的是一个地址，具体看键头
 |  |   0    | <- esp + 4   这里是自定义的一个 error_code 
 |  |   fs   | <- esp + 8
 |  |   es   | <- esp + c
 |  |   ds   | <- esp + 10
 |  |   ebp  | <- esp + 14
 |  |   esi  | <- esp + 18
 |  |   edi  | <- esp + 1c
 |  |   edx  | <- esp + 20
 |  |   ecx  | <- esp + 24
 |  |   ebx  | <- esp + 28                             
 |  |   eax  | <- esp + 2c  这里本是C函数的地址，和 eax 进行了交换, 这时 eax = &do_divide_error, 然后把这个单元变成了 eax 的值，变相的保存了 eax 的值
 `->|   EIP  | <- esp + 30  
    |   CS   | <- esp + 34
    | EFLAGS | <- esp + 38
    |   ESP  | <- esp + 3c
    |   SS   | <- esp + 40
            图4
*/

/* 设置ds es fs 的选择子为 0x10，这是内核数据段选择子 */
	movl $0x10,%edx
	mov %dx,%ds
	mov %dx,%es
	mov %dx,%fs


	call *%eax     /* 因为 eax 保存了 divide_error 的地址，所以这里调用 do_divide_error，进入这个函数的堆栈将是图4的样子，记得进入后把addl $8, %esp 指令的地址压栈 */	
	addl $8,%esp   /* 堆栈平衡，这时候 esp 指向的是 fs 那个位置 */	

	/* 恢复现场 */
	pop %fs
	pop %es
	pop %ds
	popl %ebp
	popl %esi
	popl %edi
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax

/*
	到这里，堆栈已经恢复。
	无错误代码：
	|   EIP  | <- esp[0]             esp + 0
	|   CS   | <- esp[1]             esp + 4
	| EFLAGS | <- esp[2]             esp + 8
	|   ESP  | <- esp[3]             esp + c
	|   SS   | <- esp[4]             esp + 10
	|   ...  | <- esp[5]             esp + 14
*/	
	iret

debug:
	pushl $do_int3		# _do_debug
	jmp no_error_code

nmi:
	pushl $do_nmi
	jmp no_error_code

int3:
	pushl $do_int3
	jmp no_error_code

overflow:
	pushl $do_overflow
	jmp no_error_code

bounds:
	pushl $do_bounds
	jmp no_error_code

invalid_op:
	pushl $do_invalid_op
	jmp no_error_code

coprocessor_segment_overrun:
	pushl $do_coprocessor_segment_overrun
	jmp no_error_code

reserved:
	pushl $do_reserved
	jmp no_error_code

irq13:
	pushl %eax
	xorb %al,%al
	outb %al,$0xF0
	movb $0x20,%al
	outb %al,$0x20
	jmp 1f
1:	jmp 1f
1:	outb %al,$0xA0
	popl %eax
	jmp coprocessor_error

double_fault:
	pushl $do_double_fault

/*这是有错误码的异常的入口
error_code:
	xchgl %eax,4(%esp)		# error code <-> %eax
	xchgl %ebx,(%esp)		# &function <-> %ebx
	pushl %ecx
	pushl %edx
	pushl %edi
	pushl %esi
	pushl %ebp
	push %ds
	push %es
	push %fs
	pushl %eax			# error code
	lea 44(%esp),%eax		# offset
	pushl %eax
	movl $0x10,%eax
	mov %ax,%ds
	mov %ax,%es
	mov %ax,%fs
	call *%ebx
	addl $8,%esp
	pop %fs
	pop %es
	pop %ds
	popl %ebp
	popl %esi
	popl %edi
	popl %edx
	popl %ecx
	popl %ebx
	popl %eax
	iret

invalid_TSS:
	pushl $do_invalid_TSS
	jmp error_code

segment_not_present:
	pushl $do_segment_not_present
	jmp error_code

stack_segment:
	pushl $do_stack_segment
	jmp error_code

general_protection:
	pushl $do_general_protection
	jmp error_code

