.section .text

.global irq0_stub
.type irq0_stub, @function
irq0_stub:
    pushal
    call irq0_handler
    popal
    add $4, %esp
    iret

.global irq1_stub
.type irq1_stub, @function
irq1_stub:
    pushal
    call irq1_handler
    popal
    add $4, %esp
    iret

.global irq2_stub
.type irq2_stub, @function
irq2_stub:
    pushal
    call irq2_handler
    popal
    add $4, %esp
    iret

.global irq3_stub
.type irq3_stub, @function
irq3_stub:
    pushal
    call irq3_handler
    popal
    add $4, %esp
    iret

.global syscall_stub
.type syscall_stub, @function
syscall_stub:
    pushal
    push %ds
    push %es
    push %fs
    push %gs
    push %edx
    push %ecx
    push %ebx
    push %eax
    call syscall_handler
    add $16, %esp
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popal
    iret

.global enter_user_mode
.type enter_user_mode, @function
enter_user_mode:
    mov 4(%esp), %eax
    mov 8(%esp), %ebx
    mov $0x23, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ebx, %esp
    pushl $0x23
    pushl %ebx
    pushfl
    popl %ecx
    orl $0x200, %ecx
    pushl %ecx
    pushl $0x1B
    pushl %eax
    iret

.global task_switch_to
.type task_switch_to, @function
task_switch_to:
    mov 4(%esp), %eax
    mov 8(%esp), %ebx
    mov %ebx, %esp
    jmp *%eax
