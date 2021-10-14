///                MentOS, The Mentoring Operating system project
/// @file fdc.c
/// @brief Floppy driver controller handling.
/// @copyright (c) 2014-2021 This file is distributed under the MIT License.
/// See LICENSE.md for details.

#include "drivers/fdc.h"
#include "io/video.h"
#include "io/port_io.h"

void fdc_disable_motor()
{
    /* Setting bits:
     * 2: (RESET)
     * 3: (IRQ)
     */
    outportb(DOR, 0x0c);
}

void fdc_enable_motor()
{
    /* Setting bits:
     * 3: (IRQ)
     * 4: (MOTA)
     */
    outportb(DOR, 0x18);
}
