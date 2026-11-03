PROJ_NAME=bbShow

STANDALONE=../..

BSP ?= efinix/EfxSapphireSoc

# ---------------------------------------------------------------------------
# Toolchain selection.
#
# The Efinity RISC-V IDE keeps its toolchain under
#   "D:\...\Efinity RISC-V\toolchain"
# which contains a space.  The recipes in standalone.mk invoke $(RISCV_CC)
# without quoting it, so the shell splits the path and gcc/ld end up unable to
# resolve their own sysroot - the link then fails with
#     cannot find -lc: No such file or directory
#     cannot find -lgcc: No such file or directory
# When a space-free copy of the same toolchain is available we use that instead.
# ---------------------------------------------------------------------------
ifneq ($(wildcard D:/riscv/bin/riscv-none-elf-gcc.exe),)
override RISCV_BIN     := D:/riscv/bin/riscv-none-elf-
override RISCV_CC      := D:/riscv/bin/riscv-none-elf-gcc.exe
override RISCV_OBJCOPY := D:/riscv/bin/riscv-none-elf-objcopy.exe
override RISCV_OBJDUMP := D:/riscv/bin/riscv-none-elf-objdump.exe
endif

CFLAGS+=-DSMP -I../fbDemo/src

SRCS = 	$(wildcard src/*.c) 		../fbDemo/src/fb_driver.c         ${STANDALONE}/common/start.S         ${STANDALONE}/common/trap.S


include ${STANDALONE}/common/bsp.mk
include ${STANDALONE}/common/riscv64-unknown-elf.mk
include ${STANDALONE}/common/standalone.mk

LDSCRIPT = ${BSP_PATH}/linker/default.ld
