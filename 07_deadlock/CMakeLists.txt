cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(deadlock)

# One demo; prj.conf selects which deadlock fix it uses (see Kconfig).
target_sources(app PRIVATE src/main.c src/fs_spi.c)
