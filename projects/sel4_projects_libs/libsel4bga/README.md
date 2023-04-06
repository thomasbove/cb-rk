<!--
     Copyright 2017, Data61, CSIRO (ABN 41 687 119 230)

     SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Overview

What you are looking at is a very basic driver for the Bochs Graphics Adaptor,
hereafter referred to as BGA. This device is available under QEMU when you pass
the command line option "-vga std". The driver has some limitations, such as no
support for banked mode which would increase performance.

There's currently only support for IA32 (is the BGA even available as a device
under ARM?).

## TODO
 - Some example code of how to use this driver.
