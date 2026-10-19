// kernel/dev/kbd.h
#ifndef KERNEL_DEV_KBD_H
#define KERNEL_DEV_KBD_H
#include "kernel/hal/hal.h"
namespace kbd {
void irq_handler(); int getchar_nb(); int getchar_nb_wait();
}
#endif
