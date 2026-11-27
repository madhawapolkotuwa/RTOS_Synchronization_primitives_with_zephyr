cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(priority_inversion)

# One demo; prj.conf selects which lock it uses (see Kconfig).
# For logic-analyser pins, build with -DEXTRA_DTC_OVERLAY_FILE=scope.overlay
target_sources(app PRIVATE src/main.c src/hml.c)
