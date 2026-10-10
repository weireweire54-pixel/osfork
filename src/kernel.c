#include <stdint.h>

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY ((volatile uint16_t *)0xB8000)

static uint8_t terminal_color = 0x0F;
static uint16_t terminal_row = 0;
static uint16_t terminal_col = 0;
static int shift_state = 0;
static int caps_state = 0;

static void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_write(const char *str);

static uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

#define ATA_DATA 0x1F0
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LOW 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HIGH 0x1F5
#define ATA_DRIVE 0x1F6
#define ATA_STATUS 0x1F7
#define ATA_COMMAND 0x1F7
#define ATA_ALT_STATUS 0x3F6

static int ata_wait_status(uint8_t mask, uint8_t expected) {
    for (uint32_t attempt = 0; attempt < 1000000; attempt++) {
        uint8_t status = inb(ATA_STATUS);
        if (status == 0 || status == 0xFF || (status & 0x01)) {
            return 0;
        }
        if ((status & 0x80) == 0 && (status & mask) == expected) {
            return 1;
        }
    }
    return 0;
}

static void ata_delay(void) {
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
}

static int ata_transfer_sector(uint32_t lba, uint16_t *buffer, int write) {
    if (lba >= 0x10000000) {
        return 0;
    }

    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_delay();
    if (!ata_wait_status(0x80, 0)) {
        return 0;
    }
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, write ? 0x30 : 0x20);

    if (!ata_wait_status(0x08, 0x08)) {
        return 0;
    }

    for (int i = 0; i < 256; i++) {
        if (write) {
            outw(ATA_DATA, buffer[i]);
        } else {
            buffer[i] = inw(ATA_DATA);
        }
    }

    if (write) {
        outb(ATA_COMMAND, 0xE7);
        if (!ata_wait_status(0x80, 0)) {
            return 0;
        }
        return 1;
    }
    return 1;
}

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static int serial_transmit_empty(void) {
    return (inb(0x3F8 + 5) & 0x20) != 0;
}

static void serial_write_char(char c) {
    while (!serial_transmit_empty()) {
        __asm__ volatile("nop");
    }
    outb(0x3F8, (uint8_t)c);
}

static void serial_write(const char *str) {
    for (const char *p = str; *p; ++p) {
        serial_write_char(*p);
    }
}

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t flags;
    uint16_t offset_high;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idtr_t;

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdtr_t;

typedef struct {
    uint32_t link;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es;
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_entry_t;

static tss_entry_t tss;
static gdt_entry_t gdt_entries[7];
static gdtr_t gdtr;
static idt_entry_t idt[256];
static idtr_t idtr;
volatile uint32_t system_ticks = 0;
static uint8_t keyboard_buffer[256];
static int keyboard_buffer_head = 0;
static int keyboard_buffer_tail = 0;
static int keyboard_buffer_count = 0;

#define MAX_PROCESSES 8
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE 0x18
#define GDT_USER_DATA 0x20

typedef enum {
    PROC_READY = 0,
    PROC_RUNNING = 1,
    PROC_SLEEPING = 2,
    PROC_ZOMBIE = 3
} ProcessState;

typedef struct {
    int pid;
    char name[16];
    ProcessState state;
    uint32_t runtime_ticks;
    uint32_t sleep_ticks;
    int parent_pid;
    int ready_queue_index;
    uint32_t esp;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
} Process;

static Process process_table[MAX_PROCESSES];
static int process_count = 0;
static int current_process = 0;
static int ready_queue[MAX_PROCESSES];
static int ready_queue_count = 0;

extern void irq0_stub(void);
extern void irq1_stub(void);
extern void irq2_stub(void);
extern void irq3_stub(void);
extern void syscall_stub(void);
extern void enter_user_mode(uint32_t entry, uint32_t stack);
extern void task_switch_to(uint32_t eip, uint32_t esp);

static void fs_copy_string(char *dst, const char *src, int max_len);
static void syscall_exec(const char *name);
static void task_context_switch(int next_pid);
static void syscall_with_args(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);
static void syscall_dispatch(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx);
static void terminal_write(const char *str);

static void task_user_entry(void) {
    terminal_write("user task: running\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

static void task_prepare_context(Process *task, uint32_t entry, uint32_t stack_top) {
    uint32_t *stack = (uint32_t *)stack_top;
    stack -= 5;
    stack[0] = 0x23;
    stack[1] = stack_top;
    stack[2] = 0x202;
    stack[3] = 0x1B;
    stack[4] = entry;
    task->esp = (uint32_t)stack;
    task->eip = entry;
    task->eflags = 0x202;
}

static void gdt_set_gate(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    gdt_entries[index].limit_low = limit & 0xFFFF;
    gdt_entries[index].base_low = base & 0xFFFF;
    gdt_entries[index].base_middle = (base >> 16) & 0xFF;
    gdt_entries[index].access = access;
    gdt_entries[index].granularity = (limit >> 16) & 0x0F;
    gdt_entries[index].granularity |= granularity & 0xF0;
    gdt_entries[index].base_high = (base >> 24) & 0xFF;
}

static void gdt_init(void) {
    (void)gdt_entries;
    (void)gdtr;
    (void)tss;
}

static void idt_set_gate(uint8_t index, uint32_t base, uint16_t selector, uint8_t flags) {
    idt[index].offset_low = base & 0xFFFF;
    idt[index].selector = selector;
    idt[index].zero = 0;
    idt[index].flags = flags;
    idt[index].offset_high = (base >> 16) & 0xFFFF;
}

static void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

static void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
}

static void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    idt_set_gate(0x20, (uint32_t)irq0_stub, 0x08, 0x8E);
    idt_set_gate(0x21, (uint32_t)irq1_stub, 0x08, 0x8E);
    idt_set_gate(0x22, (uint32_t)irq2_stub, 0x08, 0x8E);
    idt_set_gate(0x23, (uint32_t)irq3_stub, 0x08, 0x8E);
    idt_set_gate(0x80, (uint32_t)syscall_stub, 0x08, 0x8E);

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint32_t)idt;
    __asm__ volatile("lidt (%0)" : : "r"(&idtr));
}

static void scheduler_rebuild_ready_queue(void) {
    ready_queue_count = 0;
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state != PROC_ZOMBIE && process_table[i].state != PROC_SLEEPING) {
            ready_queue[ready_queue_count++] = i;
        }
    }
}

static int scheduler_choose_next(void) {
    if (ready_queue_count == 0) {
        return current_process;
    }

    int next = current_process;
    for (int i = 0; i < ready_queue_count; i++) {
        if (ready_queue[i] == current_process) {
            next = ready_queue[(i + 1) % ready_queue_count];
            break;
        }
    }

    if (next < 0 || next >= process_count) {
        next = ready_queue[0];
    }

    return next;
}

static void scheduler_init(void) {
    process_count = 0;
    ready_queue_count = 0;

    process_table[0].pid = 0;
    fs_copy_string(process_table[0].name, "kernel", sizeof(process_table[0].name));
    process_table[0].state = PROC_RUNNING;
    process_table[0].runtime_ticks = 0;
    process_table[0].sleep_ticks = 0;
    process_table[0].parent_pid = 0;
    process_table[0].ready_queue_index = 0;
    process_table[0].esp = 0x200000;
    process_table[0].eip = 0x100000;
    process_table[0].eflags = 0x202;
    process_table[0].eax = 0;
    process_table[0].ebx = 0;
    process_table[0].ecx = 0;
    process_table[0].edx = 0;
    process_count = 1;
    current_process = 0;
    ready_queue[0] = 0;
    ready_queue_count = 1;

    process_table[1].pid = 1;
    fs_copy_string(process_table[1].name, "init", sizeof(process_table[1].name));
    process_table[1].state = PROC_READY;
    process_table[1].runtime_ticks = 0;
    process_table[1].sleep_ticks = 0;
    process_table[1].parent_pid = 0;
    process_table[1].ready_queue_index = 1;
    process_table[1].esp = 0x201000;
    process_table[1].eip = (uint32_t)task_user_entry;
    process_table[1].eflags = 0x202;
    process_table[1].eax = 0;
    process_table[1].ebx = 0;
    process_table[1].ecx = 0;
    process_table[1].edx = 0;
    task_prepare_context(&process_table[1], process_table[1].eip, process_table[1].esp);
    ready_queue[1] = 1;
    process_count = 2;
    ready_queue_count = 2;

    process_table[2].pid = 2;
    fs_copy_string(process_table[2].name, "shell", sizeof(process_table[2].name));
    process_table[2].state = PROC_READY;
    process_table[2].runtime_ticks = 0;
    process_table[2].sleep_ticks = 0;
    process_table[2].parent_pid = 1;
    process_table[2].ready_queue_index = 2;
    process_table[2].esp = 0x202000;
    process_table[2].eip = (uint32_t)task_user_entry;
    process_table[2].eflags = 0x202;
    process_table[2].eax = 0;
    process_table[2].ebx = 0;
    process_table[2].ecx = 0;
    process_table[2].edx = 0;
    task_prepare_context(&process_table[2], process_table[2].eip, process_table[2].esp);
    ready_queue[2] = 2;
    process_count = 3;
    ready_queue_count = 3;
}

static void task_context_switch(int next_pid) {
    int next_index = -1;
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == next_pid) {
            next_index = i;
            break;
        }
    }

    if (next_index < 0 || next_index == current_process) {
        return;
    }

    uint32_t old_eax, old_ebx, old_ecx, old_edx, old_eflags, old_esp;
    __asm__ volatile(
        "pushfl\n\t"
        "popl %4\n\t"
        "mov %%eax, %0\n\t"
        "mov %%ebx, %1\n\t"
        "mov %%ecx, %2\n\t"
        "mov %%edx, %3\n\t"
        "mov %%esp, %5\n\t"
        : "=r"(old_eax), "=r"(old_ebx), "=r"(old_ecx), "=r"(old_edx), "=r"(old_eflags), "=r"(old_esp)
        :
        : "memory");

    process_table[current_process].eax = old_eax;
    process_table[current_process].ebx = old_ebx;
    process_table[current_process].ecx = old_ecx;
    process_table[current_process].edx = old_edx;
    process_table[current_process].esp = old_esp;
    process_table[current_process].eflags = old_eflags;
    process_table[current_process].state = PROC_READY;

    current_process = next_index;
    process_table[current_process].state = PROC_RUNNING;
    process_table[current_process].runtime_ticks++;

    uint32_t kernel_esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(kernel_esp));
    tss.ss0 = GDT_KERNEL_DATA;
    tss.esp0 = kernel_esp;

    if (process_table[current_process].eip != 0) {
        enter_user_mode(process_table[current_process].eip, process_table[current_process].esp);
    }
}

static int shell_lock_active = 1;

static void scheduler_tick(void) {
    if (shell_lock_active) {
        return;
    }

    if (process_count <= 1) {
        return;
    }

    if (process_table[current_process].pid == 0) {
        return;
    }

    for (int i = 0; i < process_count; i++) {
        if (process_table[i].state == PROC_SLEEPING && process_table[i].sleep_ticks > 0) {
            process_table[i].sleep_ticks--;
            if (process_table[i].sleep_ticks == 0) {
                process_table[i].state = PROC_READY;
            }
        }
    }

    scheduler_rebuild_ready_queue();

    if (ready_queue_count == 0) {
        return;
    }

    int next_idx = scheduler_choose_next();
    int next_pid = process_table[next_idx].pid;
    if (next_pid != process_table[current_process].pid) {
        task_context_switch(next_pid);
    }
}

static void scheduler_add_task(const char *name) {
    if (process_count >= MAX_PROCESSES) {
        return;
    }

    int pid = 100 + process_count;
    process_table[process_count].pid = pid;
    fs_copy_string(process_table[process_count].name, name, sizeof(process_table[process_count].name));
    process_table[process_count].state = PROC_READY;
    process_table[process_count].runtime_ticks = 0;
    process_table[process_count].sleep_ticks = 0;
    process_table[process_count].parent_pid = process_table[current_process].pid;
    process_table[process_count].ready_queue_index = process_count;
    process_table[process_count].esp = 0x200000 + (uint32_t)process_count * 0x1000;
    process_table[process_count].eip = (uint32_t)task_user_entry;
    task_prepare_context(&process_table[process_count], process_table[process_count].eip, process_table[process_count].esp);
    process_count++;
    scheduler_rebuild_ready_queue();
}

static void scheduler_sleep_task(int pid, uint32_t ticks) {
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == pid) {
            process_table[i].state = PROC_SLEEPING;
            process_table[i].sleep_ticks = ticks;
            scheduler_rebuild_ready_queue();
            return;
        }
    }
}

static void scheduler_kill_task(int pid) {
    for (int i = 0; i < process_count; i++) {
        if (process_table[i].pid == pid) {
            process_table[i].state = PROC_ZOMBIE;
            scheduler_rebuild_ready_queue();
            if (current_process == i) {
                current_process = 0;
            }
            return;
        }
    }
}

static void scheduler_fork_current(void) {
    if (process_count >= MAX_PROCESSES) {
        return;
    }

    int idx = process_count;
    process_table[idx] = process_table[current_process];
    process_table[idx].pid = 200 + idx;
    process_table[idx].state = PROC_READY;
    process_table[idx].runtime_ticks = 0;
    process_table[idx].sleep_ticks = 0;
    process_table[idx].parent_pid = process_table[current_process].pid;
    fs_copy_string(process_table[idx].name, process_table[current_process].name, sizeof(process_table[idx].name));
    if (process_table[idx].name[0] != '\0') {
        int len = 0;
        while (process_table[idx].name[len] != '\0' && len < 15) {
            len++;
        }
        process_table[idx].name[len] = '\0';
    }
    process_count++;
    scheduler_rebuild_ready_queue();
}

static void terminal_write(const char *str);

void syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    syscall_dispatch(eax, ebx, ecx, edx);
    terminal_write("syscall: user-space request handled\n");
}

static void syscall_exec(const char *name) {
    if (name == 0 || name[0] == '\0') {
        terminal_write("usage: exec <name>\n");
        return;
    }

    scheduler_add_task(name);
    terminal_write("exec: launched ");
    terminal_write(name);
    terminal_write("\n");
}

static void syscall_with_args(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    process_table[current_process].eax = eax;
    process_table[current_process].ebx = ebx;
    process_table[current_process].ecx = ecx;
    process_table[current_process].edx = edx;
}

static void syscall_dispatch(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    if (eax == 1) {
        syscall_exec((const char *)ebx);
        return;
    }

    if (eax == 2) {
        task_context_switch((int)ebx);
        return;
    }

    syscall_with_args(eax, ebx, ecx, edx);
}

void irq0_handler(void) {
    system_ticks++;
    scheduler_tick();
    pic_send_eoi(0);
}

static void keyboard_buffer_push(uint8_t scancode);
static int keyboard_buffer_pop(void);

void irq1_handler(void) {
    uint8_t scancode = inb(0x60);
    keyboard_buffer_push(scancode);
    pic_send_eoi(1);
}

void irq2_handler(void) {
    pic_send_eoi(2);
}

static void keyboard_buffer_push(uint8_t scancode) {
    if (keyboard_buffer_count < 256) {
        keyboard_buffer[keyboard_buffer_head] = scancode;
        keyboard_buffer_head = (keyboard_buffer_head + 1) % 256;
        keyboard_buffer_count++;
    }
}

static int keyboard_buffer_pop(void) {
    if (keyboard_buffer_count == 0) {
        return -1;
    }

    int value = keyboard_buffer[keyboard_buffer_tail];
    keyboard_buffer_tail = (keyboard_buffer_tail + 1) % 256;
    keyboard_buffer_count--;
    return value;
}

void irq3_handler(void) {
    pic_send_eoi(3);
}

static void terminal_putentryat(char c, uint8_t color, uint16_t x, uint16_t y) {
    unsigned char ch = (unsigned char)c;
    if (ch == 0) {
        ch = ' ';
    }
    if (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t' && ch != '\b') {
        ch = '?';
    }
    if (ch > 0x7E) {
        ch = '?';
    }

    volatile uint16_t *cell = VGA_MEMORY + (y * VGA_WIDTH + x);
    *cell = (uint16_t)color << 8 | (uint16_t)ch;
}

static void terminal_scroll(void) {
    for (uint16_t y = 1; y < VGA_HEIGHT; y++) {
        for (uint16_t x = 0; x < VGA_WIDTH; x++) {
            VGA_MEMORY[(y - 1) * VGA_WIDTH + x] = VGA_MEMORY[y * VGA_WIDTH + x];
        }
    }

    for (uint16_t x = 0; x < VGA_WIDTH; x++) {
        terminal_putentryat(' ', terminal_color, x, VGA_HEIGHT - 1);
    }
    terminal_row = VGA_HEIGHT - 1;
}

static void terminal_putchar(char c) {
    unsigned char ch = (unsigned char)c;

    if (c == '\n') {
        terminal_row++;
        terminal_col = 0;
        return;
    }

    if (c == '\r') {
        terminal_col = 0;
        return;
    }

    if (c == '\b') {
        if (terminal_col > 0) {
            terminal_col--;
            terminal_putentryat(' ', terminal_color, terminal_col, terminal_row);
        }
        return;
    }

    if (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t' && ch != '\b') {
        ch = '?';
    }
    if (ch > 0x7E) {
        ch = '?';
    }

    terminal_putentryat((char)ch, terminal_color, terminal_col, terminal_row);
    terminal_col++;

    if (terminal_col >= VGA_WIDTH) {
        terminal_col = 0;
        terminal_row++;
    }

    if (terminal_row >= VGA_HEIGHT) {
        terminal_scroll();
    }
}

static void terminal_write(const char *str) {
    for (const char *p = str; *p; p++) {
        terminal_putchar(*p);
    }
}

static void terminal_clear(void) {
    for (uint16_t y = 0; y < VGA_HEIGHT; y++) {
        for (uint16_t x = 0; x < VGA_WIDTH; x++) {
            terminal_putentryat(' ', terminal_color, x, y);
        }
    }
    terminal_row = 0;
    terminal_col = 0;
}

static int keyboard_wait_for_data(void) {
    while ((inb(0x64) & 0x01) == 0) {
        __asm__ volatile("nop");
    }
    return 1;
}

static int keyboard_get_scan_code(void) {
    if (!keyboard_wait_for_data()) {
        return -1;
    }

    return inb(0x60);
}

static int char_from_scan_code(uint8_t scancode, int *was_modifier) {
    *was_modifier = 0;

    switch (scancode) {
        case 0x2A:
        case 0x36:
            shift_state = 1;
            *was_modifier = 1;
            return 0;
        case 0xAA:
        case 0xB6:
            shift_state = 0;
            *was_modifier = 1;
            return 0;
        case 0x3A:
            caps_state = !caps_state;
            *was_modifier = 1;
            return 0;
        case 0x01:
            return 27;
        case 0x0E:
            return '\b';
        case 0x1C:
            return '\n';
        case 0x39:
            return ' ';
        case 0x02:
            return shift_state || caps_state ? '!' : '1';
        case 0x03:
            return shift_state || caps_state ? '@' : '2';
        case 0x04:
            return shift_state || caps_state ? '#' : '3';
        case 0x05:
            return shift_state || caps_state ? '$' : '4';
        case 0x06:
            return shift_state || caps_state ? '%' : '5';
        case 0x07:
            return shift_state || caps_state ? '^' : '6';
        case 0x08:
            return shift_state || caps_state ? '&' : '7';
        case 0x09:
            return shift_state || caps_state ? '*' : '8';
        case 0x0A:
            return shift_state || caps_state ? '(' : '9';
        case 0x0B:
            return shift_state || caps_state ? ')' : '0';
        case 0x0C:
            return shift_state || caps_state ? '_' : '-';
        case 0x0D:
            return shift_state || caps_state ? '+' : '=';
        case 0x10:
            return (shift_state ^ caps_state) ? 'Q' : 'q';
        case 0x11:
            return (shift_state ^ caps_state) ? 'W' : 'w';
        case 0x12:
            return (shift_state ^ caps_state) ? 'E' : 'e';
        case 0x13:
            return (shift_state ^ caps_state) ? 'R' : 'r';
        case 0x14:
            return (shift_state ^ caps_state) ? 'T' : 't';
        case 0x15:
            return (shift_state ^ caps_state) ? 'Y' : 'y';
        case 0x16:
            return (shift_state ^ caps_state) ? 'U' : 'u';
        case 0x17:
            return (shift_state ^ caps_state) ? 'I' : 'i';
        case 0x18:
            return (shift_state ^ caps_state) ? 'O' : 'o';
        case 0x19:
            return (shift_state ^ caps_state) ? 'P' : 'p';
        case 0x1E:
            return (shift_state ^ caps_state) ? 'A' : 'a';
        case 0x1F:
            return (shift_state ^ caps_state) ? 'S' : 's';
        case 0x20:
            return (shift_state ^ caps_state) ? 'D' : 'd';
        case 0x21:
            return (shift_state ^ caps_state) ? 'F' : 'f';
        case 0x22:
            return (shift_state ^ caps_state) ? 'G' : 'g';
        case 0x23:
            return (shift_state ^ caps_state) ? 'H' : 'h';
        case 0x24:
            return (shift_state ^ caps_state) ? 'J' : 'j';
        case 0x25:
            return (shift_state ^ caps_state) ? 'K' : 'k';
        case 0x26:
            return (shift_state ^ caps_state) ? 'L' : 'l';
        case 0x2C:
            return (shift_state ^ caps_state) ? 'Z' : 'z';
        case 0x2D:
            return (shift_state ^ caps_state) ? 'X' : 'x';
        case 0x2E:
            return (shift_state ^ caps_state) ? 'C' : 'c';
        case 0x2F:
            return (shift_state ^ caps_state) ? 'V' : 'v';
        case 0x30:
            return (shift_state ^ caps_state) ? 'B' : 'b';
        case 0x31:
            return (shift_state ^ caps_state) ? 'N' : 'n';
        case 0x32:
            return (shift_state ^ caps_state) ? 'M' : 'm';
        case 0x33:
            return shift_state || caps_state ? '<' : ',';
        case 0x34:
            return shift_state || caps_state ? '>' : '.';
        case 0x35:
            return shift_state || caps_state ? '?' : '/';
        default:
            return 0;
    }
}

static int string_equals(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static void string_to_lower(char *s) {
    while (*s) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s + 32);
        }
        s++;
    }
}

static char *trim_left(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static char *first_arg(char *cmd) {
    while (*cmd && *cmd != ' ' && *cmd != '\t') {
        cmd++;
    }
    while (*cmd == ' ' || *cmd == '\t') {
        *cmd = '\0';
        cmd++;
    }
    return cmd;
}

#define MAX_FS_ENTRIES 64
#define MAX_PATH_LEN 64
#define MAX_NAME_LEN 24
#define MAX_CONTENT_LEN 128

typedef struct {
    char name[MAX_NAME_LEN];
    char parent[MAX_PATH_LEN];
    char content[MAX_CONTENT_LEN];
    int is_dir;
} FsEntry;

#define FS_DISK_START_LBA 1
#define FS_DISK_SECTORS 32
#define FS_DISK_MAGIC 0x4F534653
#define FS_DISK_VERSION 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t checksum;
    FsEntry entries[MAX_FS_ENTRIES];
} FsDiskImage;

static FsEntry fs_entries[MAX_FS_ENTRIES];
static int fs_entry_count = 0;
static char current_dir[MAX_PATH_LEN] = "/";
static uint32_t fs_disk_buffer[FS_DISK_SECTORS * 128];
static int fs_persistence_ready = 0;
static int fs_storage_status = 0;

static int fs_persist_load(void);
static int fs_persist_save(void);

static void fs_copy_string(char *dst, const char *src, int max_len) {
    int i = 0;
    while (src[i] != '\0' && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static uint32_t fs_persist_checksum(const FsDiskImage *image) {
    uint32_t hash = 5381;
    const uint8_t *count_bytes = (const uint8_t *)&image->entry_count;
    const uint8_t *entries_bytes = (const uint8_t *)image->entries;

    for (uint32_t i = 0; i < sizeof(image->entry_count); i++) {
        hash = ((hash << 5) + hash) ^ count_bytes[i];
    }
    for (uint32_t i = 0; i < sizeof(image->entries); i++) {
        hash = ((hash << 5) + hash) ^ entries_bytes[i];
    }
    return hash;
}

static int fs_persist_load(void) {
    if (sizeof(FsDiskImage) > sizeof(fs_disk_buffer)) {
        return 0;
    }

    for (uint32_t sector = 0; sector < FS_DISK_SECTORS; sector++) {
        if (!ata_transfer_sector(FS_DISK_START_LBA + sector,
                                 (uint16_t *)&fs_disk_buffer[sector * 128], 0)) {
            return 0;
        }
    }

    const FsDiskImage *image = (const FsDiskImage *)fs_disk_buffer;
    if (image->magic != FS_DISK_MAGIC || image->version != FS_DISK_VERSION ||
        image->entry_count == 0 || image->entry_count > MAX_FS_ENTRIES ||
        image->checksum != fs_persist_checksum(image)) {
        return 0;
    }

    for (uint32_t i = 0; i < image->entry_count; i++) {
        const FsEntry *entry = &image->entries[i];
        if (entry->name[0] == '\0' || entry->name[MAX_NAME_LEN - 1] != '\0' ||
            entry->parent[0] != '/' || entry->parent[MAX_PATH_LEN - 1] != '\0' ||
            entry->content[MAX_CONTENT_LEN - 1] != '\0' ||
            (entry->is_dir != 0 && entry->is_dir != 1)) {
            return 0;
        }
    }

    for (int i = 0; i < MAX_FS_ENTRIES; i++) {
        uint8_t *destination = (uint8_t *)&fs_entries[i];
        const uint8_t *source = (const uint8_t *)&image->entries[i];
        for (uint32_t byte = 0; byte < sizeof(FsEntry); byte++) {
            destination[byte] = source[byte];
        }
    }
    fs_entry_count = (int)image->entry_count;
    fs_storage_status = 1;
    return 1;
}

static int fs_persist_save(void) {
    if (!fs_persistence_ready || sizeof(FsDiskImage) > sizeof(fs_disk_buffer)) {
        return 0;
    }

    uint8_t *buffer_bytes = (uint8_t *)fs_disk_buffer;
    for (uint32_t i = 0; i < sizeof(fs_disk_buffer); i++) {
        buffer_bytes[i] = 0;
    }

    FsDiskImage *image = (FsDiskImage *)fs_disk_buffer;
    image->magic = FS_DISK_MAGIC;
    image->version = FS_DISK_VERSION;
    image->entry_count = (uint32_t)fs_entry_count;
    for (int i = 0; i < MAX_FS_ENTRIES; i++) {
        uint8_t *destination = (uint8_t *)&image->entries[i];
        const uint8_t *source = (const uint8_t *)&fs_entries[i];
        for (uint32_t byte = 0; byte < sizeof(FsEntry); byte++) {
            destination[byte] = source[byte];
        }
    }
    image->checksum = fs_persist_checksum(image);

    for (uint32_t sector = 1; sector < FS_DISK_SECTORS; sector++) {
        if (!ata_transfer_sector(FS_DISK_START_LBA + sector,
                                 (uint16_t *)&fs_disk_buffer[sector * 128], 1)) {
            return 0;
        }
    }
    if (!ata_transfer_sector(FS_DISK_START_LBA, (uint16_t *)fs_disk_buffer, 1)) {
        return 0;
    }

    fs_storage_status = 2;
    return 1;
}

static int fs_path_equals(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static void fs_trim_trailing_slash(char *path) {
    int len = 0;
    while (path[len] != '\0') {
        len++;
    }

    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
}

static void fs_resolve_path(const char *input, char *out, int out_max) {
    char base[MAX_PATH_LEN];
    char work[MAX_PATH_LEN];
    char parts[16][MAX_NAME_LEN];
    int part_count = 0;
    int i = 0;
    int j = 0;

    if (input == 0 || input[0] == '\0') {
        fs_copy_string(base, current_dir, sizeof(base));
    } else if (input[0] == '/') {
        fs_copy_string(base, input, sizeof(base));
    } else {
        fs_copy_string(base, current_dir, sizeof(base));
        if (!fs_path_equals(base, "/")) {
            if (base[0] != '\0' && base[0] != '/') {
                base[0] = '/';
                base[1] = '\0';
            }
            if (base[0] == '/' && base[1] != '\0') {
                i = 0;
                while (base[i] != '\0') {
                    i++;
                }
                if (base[i - 1] != '/') {
                    base[i] = '/';
                    base[i + 1] = '\0';
                }
            }
        }
        if (!fs_path_equals(base, "/")) {
            i = 0;
            while (base[i] != '\0') {
                i++;
            }
            if (base[i - 1] != '/') {
                base[i] = '/';
                base[i + 1] = '\0';
            }
        }
        i = 0;
        while (base[i] != '\0' && i < (int)sizeof(base) - 1) {
            i++;
        }
        if (i > 0 && base[i - 1] != '/') {
            base[i] = '/';
            base[i + 1] = '\0';
        }
        if (base[0] == '\0') {
            fs_copy_string(base, "/", sizeof(base));
        }
    }

    if (fs_path_equals(base, "/")) {
        fs_copy_string(work, "/", sizeof(work));
    } else {
        fs_copy_string(work, base, sizeof(work));
    }

    if (input != 0 && input[0] != '\0' && input[0] != '/') {
        i = 0;
        while (work[i] != '\0') {
            i++;
        }
        if (i > 0 && work[i - 1] != '/') {
            work[i] = '/';
            work[i + 1] = '\0';
        }
        j = 0;
        while (input[j] != '\0' && i + j + 1 < (int)sizeof(work)) {
            work[i + j] = input[j];
            j++;
        }
        work[i + j] = '\0';
    }

    if (work[0] == '\0') {
        fs_copy_string(out, "/", out_max);
        return;
    }

    fs_trim_trailing_slash(work);
    if (work[0] == '\0') {
        fs_copy_string(out, "/", out_max);
        return;
    }

    i = 0;
    while (work[i] != '\0') {
        if (work[i] == '/') {
            i++;
            continue;
        }
        j = 0;
        while (work[i] != '\0' && work[i] != '/') {
            if (j < MAX_NAME_LEN - 1) {
                parts[part_count][j++] = work[i];
            }
            i++;
        }
        parts[part_count][j] = '\0';
        if (parts[part_count][0] != '\0' && !fs_path_equals(parts[part_count], ".")) {
            if (fs_path_equals(parts[part_count], "..")) {
                if (part_count > 0) {
                    part_count--;
                }
            } else {
                part_count++;
            }
        }
    }

    out[0] = '/';
    out[1] = '\0';
    for (i = 0; i < part_count; i++) {
        int len = 0;
        while (out[len] != '\0') {
            len++;
        }
        if (len > 1 && out[len - 1] != '/') {
            out[len] = '/';
            out[len + 1] = '\0';
        }
        j = 0;
        while (parts[i][j] != '\0' && len + j + 1 < out_max) {
            out[len + j] = parts[i][j];
            j++;
        }
        out[len + j] = '\0';
    }

    if (out[1] == '\0' && out[0] == '/') {
        fs_copy_string(out, "/", out_max);
    }
}
static void fs_parent_path(const char *path, char *out) {
    if (path == 0 || fs_path_equals(path, "/")) {
        fs_copy_string(out, "/", MAX_PATH_LEN);
        return;
    }

    char copy[MAX_PATH_LEN];
    fs_copy_string(copy, path, sizeof(copy));
    fs_trim_trailing_slash(copy);

    int len = 0;
    while (copy[len] != '\0') {
        len++;
    }

    int slash = len;
    while (slash > 0 && copy[slash - 1] != '/') {
        slash--;
    }

    if (slash <= 1) {
        fs_copy_string(out, "/", MAX_PATH_LEN);
        return;
    }

    int out_len = 0;
    while (out_len < slash - 1 && copy[out_len] != '\0') {
        out[out_len] = copy[out_len];
        out_len++;
    }
    out[out_len] = '\0';
    if (out[0] == '\0') {
        fs_copy_string(out, "/", MAX_PATH_LEN);
    }
}

static void fs_last_name(const char *path, char *out) {
    if (path == 0 || fs_path_equals(path, "/")) {
        fs_copy_string(out, "", MAX_NAME_LEN);
        return;
    }

    char copy[MAX_PATH_LEN];
    fs_copy_string(copy, path, sizeof(copy));
    fs_trim_trailing_slash(copy);

    int len = 0;
    while (copy[len] != '\0') {
        len++;
    }

    int start = len;
    while (start > 0 && copy[start - 1] != '/') {
        start--;
    }

    int i = 0;
    while (copy[start] != '\0' && i < MAX_NAME_LEN - 1) {
        out[i] = copy[start];
        start++;
        i++;
    }
    out[i] = '\0';
}

static void fs_add_dir(const char *name, const char *parent) {
    if (fs_entry_count >= MAX_FS_ENTRIES) {
        return;
    }

    for (int i = 0; i < fs_entry_count; i++) {
        if (string_equals(fs_entries[i].name, name) && string_equals(fs_entries[i].parent, parent)) {
            return;
        }
    }

    fs_copy_string(fs_entries[fs_entry_count].name, name, sizeof(fs_entries[fs_entry_count].name));
    fs_copy_string(fs_entries[fs_entry_count].parent, parent, sizeof(fs_entries[fs_entry_count].parent));
    fs_entries[fs_entry_count].content[0] = '\0';
    fs_entries[fs_entry_count].is_dir = 1;
    fs_entry_count++;
    (void)fs_persist_save();
}

static void fs_add_file(const char *name, const char *parent, const char *content) {
    if (fs_entry_count >= MAX_FS_ENTRIES) {
        return;
    }

    for (int i = 0; i < fs_entry_count; i++) {
        if (string_equals(fs_entries[i].name, name) && string_equals(fs_entries[i].parent, parent)) {
            return;
        }
    }

    fs_copy_string(fs_entries[fs_entry_count].name, name, sizeof(fs_entries[fs_entry_count].name));
    fs_copy_string(fs_entries[fs_entry_count].parent, parent, sizeof(fs_entries[fs_entry_count].parent));
    fs_copy_string(fs_entries[fs_entry_count].content, content, sizeof(fs_entries[fs_entry_count].content));
    fs_entries[fs_entry_count].is_dir = 0;
    fs_entry_count++;
    (void)fs_persist_save();
}

static void fs_init(void) {
    fs_entry_count = 0;
    fs_add_dir("bin", "/");
    fs_add_dir("dev", "/");
    fs_add_dir("etc", "/");
    fs_add_dir("home", "/");
    fs_add_dir("usr", "/");
    fs_add_dir("var", "/");
    fs_add_file("hello.txt", "/", "Hello from osfork!\n");
    fs_add_file("readme.txt", "/", "Welcome to osfork shell.\nThis is a minimal kernel shell.\n");
    fs_add_file("motd", "/", "osfork v0.2 - minimal bare-metal OS\n");
    fs_add_dir("docs", "/home");
    fs_add_file("notes.txt", "/home/docs", "Kernel notes: shell is live.\n");
    fs_copy_string(current_dir, "/", sizeof(current_dir));

    if (fs_persist_load()) {
        fs_persistence_ready = 1;
        return;
    }

    fs_persistence_ready = 1;
    if (!fs_persist_save()) {
        fs_storage_status = 0;
    }
}

static int fs_lookup_index(const char *path) {
    char resolved[MAX_PATH_LEN];
    char parent[MAX_PATH_LEN];
    char name[MAX_NAME_LEN];

    fs_resolve_path(path, resolved, sizeof(resolved));
    fs_parent_path(resolved, parent);
    fs_last_name(resolved, name);

    if (name[0] == '\0') {
        return -1;
    }

    for (int i = 0; i < fs_entry_count; i++) {
        if (string_equals(fs_entries[i].name, name) && string_equals(fs_entries[i].parent, parent)) {
            return i;
        }
    }

    return -1;
}

static void fs_list_dir(const char *path) {
    char resolved[MAX_PATH_LEN];
    fs_resolve_path(path, resolved, sizeof(resolved));

    int found = 0;
    for (int i = 0; i < fs_entry_count; i++) {
        if (string_equals(fs_entries[i].parent, resolved)) {
            if (found) {
                terminal_write("  ");
            }
            terminal_write(fs_entries[i].name);
            found = 1;
        }
    }

    if (!found) {
        terminal_write("empty");
    }
    terminal_write("\n");
}

static const char *fs_read_file(const char *path) {
    int index = fs_lookup_index(path);
    if (index < 0 || fs_entries[index].is_dir) {
        return 0;
    }
    return fs_entries[index].content;
}

static void fs_remove_entry(const char *path) {
    int index = fs_lookup_index(path);
    if (index < 0) {
        return;
    }

    for (int i = index; i < fs_entry_count - 1; i++) {
        fs_entries[i].name[0] = '\0';
        fs_entries[i].parent[0] = '\0';
        fs_entries[i].content[0] = '\0';
        fs_entries[i].is_dir = fs_entries[i + 1].is_dir;

        int j = 0;
        while (fs_entries[i + 1].name[j] != '\0' && j < MAX_NAME_LEN - 1) {
            fs_entries[i].name[j] = fs_entries[i + 1].name[j];
            j++;
        }
        fs_entries[i].name[j] = '\0';

        j = 0;
        while (fs_entries[i + 1].parent[j] != '\0' && j < MAX_PATH_LEN - 1) {
            fs_entries[i].parent[j] = fs_entries[i + 1].parent[j];
            j++;
        }
        fs_entries[i].parent[j] = '\0';

        j = 0;
        while (fs_entries[i + 1].content[j] != '\0' && j < MAX_CONTENT_LEN - 1) {
            fs_entries[i].content[j] = fs_entries[i + 1].content[j];
            j++;
        }
        fs_entries[i].content[j] = '\0';
    }
    fs_entry_count--;
    (void)fs_persist_save();
}

static void print_uint32(uint32_t value) {
    char buf[16];
    int len = 0;

    if (value == 0) {
        terminal_write("0");
        return;
    }

    while (value > 0 && len < 15) {
        buf[len++] = (char)('0' + (value % 10));
        value /= 10;
    }

    for (int i = len - 1; i >= 0; i--) {
        terminal_putchar(buf[i]);
    }
}

static void print_uptime(void) {
    uint32_t total_seconds = system_ticks / 18;
    uint32_t hours = total_seconds / 3600;
    uint32_t minutes = (total_seconds % 3600) / 60;
    uint32_t seconds = total_seconds % 60;

    char time_buf[16];
    time_buf[0] = '0' + (hours / 10);
    time_buf[1] = '0' + (hours % 10);
    time_buf[2] = ':';
    time_buf[3] = '0' + (minutes / 10);
    time_buf[4] = '0' + (minutes % 10);
    time_buf[5] = ':';
    time_buf[6] = '0' + (seconds / 10);
    time_buf[7] = '0' + (seconds % 10);
    time_buf[8] = '\0';

    terminal_write("uptime: ");
    terminal_write(time_buf);
    terminal_write("\n");
}

static void browser_draw_page(int page_index) {
    static const char *titles[] = {
        "Home",
        "Docs",
        "Tasks",
        "Files"
    };

    static const char *pages[] = {
        "Welcome to osfork\n\nThis is a lightweight browser demo for\nthe bare-metal kernel.\n\nUse 1-4 to switch pages and q to quit.",
        "Kernel docs\n\nThe browser runs in text mode and can\nshow built-in pages without networking.\n\nThis is a simple local web-viewer shell.",
        "Tasks\n\nThe scheduler tracks processes, jobs,\nforeground shell state, and sleep states.\n\nUse 'ps' or 'jobs' in the shell to inspect.",
        "Files\n\n/\n/bin\n/etc\n/home\n\nUse mkdir, touch, ls, cat to explore the FS."
    };

    if (page_index < 0) {
        page_index = 0;
    }
    if (page_index > 3) {
        page_index = 3;
    }

    terminal_clear();
    terminal_write("=== osfork browser ===\n");
    terminal_write("Page ");
    print_uint32((uint32_t)(page_index + 1));
    terminal_write("/4: ");
    terminal_write(titles[page_index]);
    terminal_write("\n");
    terminal_write("--------------------\n");
    terminal_write(pages[page_index]);
    terminal_write("\n\n");
    terminal_write("[1] Home  [2] Docs  [3] Tasks  [4] Files\n");
    terminal_write("[q] Quit to shell\n");
}

static void browser_mode(void) {
    int page_index = 0;

    browser_draw_page(page_index);

    while (1) {
        int scancode = keyboard_get_scan_code();
        if (scancode < 0) {
            continue;
        }

        if ((scancode & 0x80) != 0) {
            continue;
        }

        if (scancode == 0xE0 || scancode == 0xE1) {
            continue;
        }

        int modifier = 0;
        int ch = char_from_scan_code((uint8_t)scancode, &modifier);
        if (modifier || ch == 0) {
            continue;
        }

        if (ch == 'q' || ch == 'Q' || ch == 27) {
            terminal_clear();
            terminal_write("osfork> ");
            return;
        }

        if (ch >= '1' && ch <= '4') {
            page_index = ch - '1';
            browser_draw_page(page_index);
        }
    }
}

static int tictactoe_find_line(char board[9], char mark) {
    static const uint8_t lines[8][3] = {
        {0, 1, 2}, {3, 4, 5}, {6, 7, 8},
        {0, 3, 6}, {1, 4, 7}, {2, 5, 8},
        {0, 4, 8}, {2, 4, 6}
    };

    for (int line = 0; line < 8; line++) {
        int matches = 0;
        int empty = -1;
        for (int cell = 0; cell < 3; cell++) {
            int index = lines[line][cell];
            if (board[index] == mark) {
                matches++;
            } else if (board[index] >= '1' && board[index] <= '9') {
                empty = index;
            }
        }
        if (matches == 2 && empty >= 0) {
            return empty;
        }
    }
    return -1;
}

static int tictactoe_choose_computer_move(char board[9]) {
    static const uint8_t preferred_moves[9] = {4, 0, 2, 6, 8, 1, 3, 5, 7};
    int move = tictactoe_find_line(board, 'O');
    if (move >= 0) {
        return move;
    }

    move = tictactoe_find_line(board, 'X');
    if (move >= 0) {
        return move;
    }

    for (int i = 0; i < 9; i++) {
        move = preferred_moves[i];
        if (board[move] >= '1' && board[move] <= '9') {
            return move;
        }
    }
    return -1;
}

static char tictactoe_winner(char board[9]) {
    static const uint8_t lines[8][3] = {
        {0, 1, 2}, {3, 4, 5}, {6, 7, 8},
        {0, 3, 6}, {1, 4, 7}, {2, 5, 8},
        {0, 4, 8}, {2, 4, 6}
    };

    for (int line = 0; line < 8; line++) {
        char mark = board[lines[line][0]];
        if ((mark == 'X' || mark == 'O') &&
            mark == board[lines[line][1]] && mark == board[lines[line][2]]) {
            return mark;
        }
    }
    return 0;
}

static int tictactoe_is_full(char board[9]) {
    for (int i = 0; i < 9; i++) {
        if (board[i] >= '1' && board[i] <= '9') {
            return 0;
        }
    }
    return 1;
}

static int tictactoe_scan_char(uint8_t scancode, int *modifier) {
    int ch = char_from_scan_code(scancode, modifier);
    if (ch != 0 || *modifier) {
        return ch;
    }

    switch (scancode) {
        case 0x47: return '7';
        case 0x48: return '8';
        case 0x49: return '9';
        case 0x4B: return '4';
        case 0x4C: return '5';
        case 0x4D: return '6';
        case 0x4F: return '1';
        case 0x50: return '2';
        case 0x51: return '3';
        default: return 0;
    }
}

static void tictactoe_draw(char board[9]) {
    terminal_clear();
    terminal_write("=== Tic-tac-toe ===\n");
    terminal_write("You are X, computer is O. Use number row or keypad 1-9.\n\n");
    for (int row = 0; row < 3; row++) {
        int start = row * 3;
        terminal_putchar(' ');
        terminal_putchar(board[start]);
        terminal_write(" | ");
        terminal_putchar(board[start + 1]);
        terminal_write(" | ");
        terminal_putchar(board[start + 2]);
        terminal_putchar('\n');
        if (row < 2) {
            terminal_write("---+---+---\n");
        }
    }
    terminal_write("\n[q] Quit to shell\n");
}

static void tictactoe_mode(void) {
    char board[9];
    for (int i = 0; i < 9; i++) {
        board[i] = (char)('1' + i);
    }

    tictactoe_draw(board);
    terminal_write("Your move (1-9): ");

    while (1) {
        int scancode = keyboard_get_scan_code();
        if (scancode < 0 || (scancode & 0x80) != 0 || scancode == 0xE0 || scancode == 0xE1) {
            continue;
        }

        int modifier = 0;
        int ch = tictactoe_scan_char((uint8_t)scancode, &modifier);
        if (modifier || ch == 0 || ch == '\n') {
            continue;
        }
        if (ch == 'q' || ch == 'Q' || ch == 27) {
            terminal_putchar('\n');
            return;
        }
        if (ch < '1' || ch > '9') {
            continue;
        }

        int player_move = ch - '1';
        if (board[player_move] == 'X' || board[player_move] == 'O') {
            terminal_write("Cell occupied. Choose another: ");
            continue;
        }
        board[player_move] = 'X';

        char winner = tictactoe_winner(board);
        if (winner == 'X') {
            tictactoe_draw(board);
            terminal_write("You win!\n");
            return;
        }
        if (tictactoe_is_full(board)) {
            tictactoe_draw(board);
            terminal_write("Draw!\n");
            return;
        }

        int computer_move = tictactoe_choose_computer_move(board);
        if (computer_move >= 0) {
            board[computer_move] = 'O';
        }
        winner = tictactoe_winner(board);
        if (winner == 'O') {
            tictactoe_draw(board);
            terminal_write("Computer wins.\n");
            return;
        }
        if (tictactoe_is_full(board)) {
            tictactoe_draw(board);
            terminal_write("Draw!\n");
            return;
        }

        tictactoe_draw(board);
        terminal_write("Your move (1-9): ");
    }
}

static void handle_command(char *buffer) {
    char *cmd = trim_left(buffer);
    char *arg = first_arg(cmd);

    string_to_lower(cmd);

    if (string_equals(cmd, "help")) {
        terminal_write("Commands:\n");
        terminal_write("  help      show this menu\n");
        terminal_write("  browse    open local browser view\n");
        terminal_write("  game      play tic-tac-toe\n");
        terminal_write("  clear     clear the screen\n");
        terminal_write("  echo      print a message\n");
        terminal_write("  ls [dir]  list directory contents\n");
        terminal_write("  cd <dir>  change current directory\n");
        terminal_write("  pwd       print current directory\n");
        terminal_write("  mkdir <dir> create a directory\n");
        terminal_write("  touch <file> create a file\n");
        terminal_write("  rm <file> remove a file\n");
        terminal_write("  cat <file> print file contents\n");
        terminal_write("  ps        list running tasks\n");
        terminal_write("  jobs      list background tasks\n");
        terminal_write("  whoami    print current user\n");
        terminal_write("  uname     print system info\n");
        terminal_write("  run <name> start a task\n");
        terminal_write("  fork      clone the current task\n");
        terminal_write("  exec <name> load a user task\n");
        terminal_write("  switch <pid> switch active task\n");
        terminal_write("  sleep <ticks> suspend current task\n");
        terminal_write("  kill <pid> terminate a task\n");
        terminal_write("  exit      terminate current task\n");
        terminal_write("  time      show uptime\n");
        terminal_write("  mem       show memory info\n");
        terminal_write("  about     show OS info\n");
        terminal_write("  hello     greet user\n");
        terminal_write("  reboot    reboot machine\n");
        return;
    }

    if (string_equals(cmd, "browse") || string_equals(cmd, "browser") || string_equals(cmd, "web")) {
        browser_mode();
        return;
    }

    if (string_equals(cmd, "game") || string_equals(cmd, "tictactoe")) {
        tictactoe_mode();
        return;
    }

    if (string_equals(cmd, "clear")) {
        terminal_clear();
        return;
    }

    if (string_equals(cmd, "echo")) {
        terminal_write(arg);
        terminal_write("\n");
        return;
    }

    if (string_equals(cmd, "ls")) {
        char target[MAX_PATH_LEN];
        fs_resolve_path(arg[0] != '\0' ? arg : current_dir, target, sizeof(target));
        fs_list_dir(target);
        return;
    }

    if (string_equals(cmd, "cd")) {
        if (arg[0] == '\0') {
            fs_copy_string(current_dir, "/", sizeof(current_dir));
            return;
        }

        char target[MAX_PATH_LEN];
        fs_resolve_path(arg, target, sizeof(target));
        int index = fs_lookup_index(target);
        if (index >= 0 && fs_entries[index].is_dir) {
            fs_copy_string(current_dir, target, sizeof(current_dir));
            return;
        }

        if (string_equals(arg, "..")) {
            char parent[MAX_PATH_LEN];
            fs_parent_path(current_dir, parent);
            fs_copy_string(current_dir, parent, sizeof(current_dir));
            return;
        }

        terminal_write("cd: no such directory\n");
        return;
    }

    if (string_equals(cmd, "pwd")) {
        terminal_write(current_dir);
        terminal_write("\n");
        return;
    }

    if (string_equals(cmd, "mkdir")) {
        if (arg[0] == '\0') {
            terminal_write("usage: mkdir <dir>\n");
            return;
        }

        char target[MAX_PATH_LEN];
        fs_resolve_path(arg, target, sizeof(target));
        char name[MAX_NAME_LEN];
        fs_last_name(target, name);
        char parent[MAX_PATH_LEN];
        fs_parent_path(target, parent);
        fs_add_dir(name, parent);
        terminal_write("created dir: ");
        terminal_write(name);
        terminal_write("\n");
        return;
    }

    if (string_equals(cmd, "touch")) {
        if (arg[0] == '\0') {
            terminal_write("usage: touch <file>\n");
            return;
        }

        char target[MAX_PATH_LEN];
        fs_resolve_path(arg, target, sizeof(target));
        char name[MAX_NAME_LEN];
        fs_last_name(target, name);
        char parent[MAX_PATH_LEN];
        fs_parent_path(target, parent);
        fs_add_file(name, parent, "");
        terminal_write("created file: ");
        terminal_write(name);
        terminal_write("\n");
        return;
    }

    if (string_equals(cmd, "rm")) {
        if (arg[0] == '\0') {
            terminal_write("usage: rm <file>\n");
            return;
        }

        char target[MAX_PATH_LEN];
        fs_resolve_path(arg, target, sizeof(target));
        int index = fs_lookup_index(target);
        if (index < 0) {
            terminal_write("rm: not found\n");
            return;
        }

        fs_remove_entry(target);
        terminal_write("removed\n");
        return;
    }

    if (string_equals(cmd, "cat")) {
        if (arg[0] == '\0') {
            terminal_write("usage: cat <file>\n");
            return;
        }

        const char *data = fs_read_file(arg);
        if (data == 0) {
            terminal_write("cat: file not found\n");
            return;
        }

        terminal_write(data);
        return;
    }

    if (string_equals(cmd, "ps") || string_equals(cmd, "jobs")) {
        terminal_write("PID  STATE  NAME\n");
        for (int i = 0; i < process_count; i++) {
            if (process_table[i].state == PROC_ZOMBIE) {
                continue;
            }

            char state_buf[8];
            if (process_table[i].state == PROC_RUNNING) {
                fs_copy_string(state_buf, "run", sizeof(state_buf));
            } else if (process_table[i].state == PROC_READY) {
                fs_copy_string(state_buf, "ready", sizeof(state_buf));
            } else if (process_table[i].state == PROC_SLEEPING) {
                fs_copy_string(state_buf, "sleep", sizeof(state_buf));
            } else {
                fs_copy_string(state_buf, "zombie", sizeof(state_buf));
            }

            print_uint32((uint32_t)process_table[i].pid);
            terminal_write("  ");
            terminal_write(state_buf);
            terminal_write("  ");
            terminal_write(process_table[i].name);
            terminal_write("\n");
        }
        return;
    }

    if (string_equals(cmd, "whoami")) {
        terminal_write("root\n");
        return;
    }

    if (string_equals(cmd, "uname")) {
        terminal_write("osfork 0.2 x86 bare-metal\n");
        return;
    }

    if (string_equals(cmd, "run")) {
        if (arg[0] == '\0') {
            terminal_write("usage: run <name>\n");
            return;
        }

        char task_name[16];
        int k = 0;
        while (arg[k] != '\0' && k < 15) {
            task_name[k] = arg[k];
            k++;
        }
        task_name[k] = '\0';

        scheduler_add_task(task_name);
        terminal_write("started task: ");
        terminal_write(task_name);
        terminal_write("\n");
        return;
    }

    if (string_equals(cmd, "exec")) {
        syscall_exec(arg);
        return;
    }

    if (string_equals(cmd, "fork")) {
        scheduler_fork_current();
        terminal_write("forked current task\n");
        return;
    }

    if (string_equals(cmd, "switch")) {
        if (arg[0] == '\0') {
            terminal_write("usage: switch <pid>\n");
            return;
        }

        int pid = 0;
        for (int i = 0; arg[i] != '\0'; i++) {
            if (arg[i] >= '0' && arg[i] <= '9') {
                pid = pid * 10 + (arg[i] - '0');
            }
        }

        task_context_switch(pid);
        terminal_write("context switched\n");
        return;
    }

    if (string_equals(cmd, "sleep")) {
        if (arg[0] == '\0') {
            terminal_write("usage: sleep <ticks>\n");
            return;
        }

        uint32_t ticks = 0;
        for (int i = 0; arg[i] != '\0'; i++) {
            if (arg[i] >= '0' && arg[i] <= '9') {
                ticks = ticks * 10 + (uint32_t)(arg[i] - '0');
            }
        }

        scheduler_sleep_task(process_table[current_process].pid, ticks);
        terminal_write("sleeping task\n");
        return;
    }

    if (string_equals(cmd, "kill")) {
        if (arg[0] == '\0') {
            terminal_write("usage: kill <pid>\n");
            return;
        }

        int target = 0;
        for (int i = 0; arg[i] != '\0'; i++) {
            if (arg[i] >= '0' && arg[i] <= '9') {
                target = target * 10 + (arg[i] - '0');
            }
        }

        scheduler_kill_task(target);
        terminal_write("terminated task\n");
        return;
    }

    if (string_equals(cmd, "exit")) {
        scheduler_kill_task(process_table[current_process].pid);
        terminal_write("task exited\n");
        return;
    }

    if (string_equals(cmd, "time")) {
        print_uptime();
        return;
    }

    if (string_equals(cmd, "mem")) {
        terminal_write("memory: 640 KB usable RAM\n");
        return;
    }

    if (string_equals(cmd, "about")) {
        terminal_write("osfork minimal OS. Built for x86 bare metal.\n");
        return;
    }

    if (string_equals(cmd, "hello")) {
        terminal_write("Hello from osfork!\n");
        return;
    }

    if (string_equals(cmd, "reboot")) {
        terminal_write("Rebooting...\n");
        __asm__ volatile("cli");
        outb(0x64, 0xFE);
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    if (cmd[0] != '\0') {
        terminal_write("Unknown command: ");
        terminal_write(cmd);
        terminal_write("\n");
    }
}

static void shell_loop(void) {
    char buffer[128];
    int length = 0;

    shell_lock_active = 1;
    terminal_write("osfork> ");

    while (1) {
        int scancode = keyboard_get_scan_code();
        if (scancode < 0) {
            continue;
        }

        if ((scancode & 0x80) != 0) {
            continue;
        }

        if (scancode == 0xE0 || scancode == 0xE1) {
            continue;
        }

        int modifier = 0;
        int ch = char_from_scan_code((uint8_t)scancode, &modifier);

        if (modifier) {
            continue;
        }

        if (ch == 0) {
            continue;
        }

        if (ch == '\b') {
            if (length > 0) {
                length--;
                buffer[length] = '\0';
                terminal_putchar('\b');
            }
            continue;
        }

        if (ch == '\n') {
            terminal_putchar('\n');
            buffer[length] = '\0';
            handle_command(buffer);
            length = 0;
            terminal_write("osfork> ");
            continue;
        }

        if (length < 127) {
            buffer[length++] = (char)ch;
            terminal_putchar((char)ch);
        }
    }
}

void kernel_main(void) {
    serial_init();
    serial_write("[boot] serial ready\n");
    terminal_clear();
    terminal_write("[boot] VGA ready\n");
    terminal_write("[boot] osfork OS is booting...\n");
    serial_write("[boot] kernel_main entered\n");

    gdt_init();
    serial_write("[boot] GDT skipped\n");
    terminal_write("[boot] GDT skipped\n");

    fs_init();
    serial_write("[boot] fs ready\n");
    terminal_write("[boot] fs ready\n");
    if (fs_storage_status == 1) {
        terminal_write("[storage] filesystem restored from disk\n");
    } else if (fs_storage_status == 2) {
        terminal_write("[storage] virtual disk ready\n");
    } else {
        terminal_write("[storage] no virtual disk; changes are temporary\n");
    }

    serial_write("[boot] entering shell\n");
    terminal_write("Welcome to the minimal operating system.\n\n");
    terminal_write("[boot] entering shell\n");
    shell_loop();
    serial_write("[boot] shell exited\n");
    terminal_write("[boot] shell exited\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}
