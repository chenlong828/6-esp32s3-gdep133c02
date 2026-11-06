#pragma once

/* Run the USB-Serial-JTAG frame protocol forever.  Never returns. */
void bridge_run(void) __attribute__((noreturn));
