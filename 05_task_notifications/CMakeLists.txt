cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(task_notifications)

target_sources(app PRIVATE src/main.c)
# Exactly one demo is compiled in, chosen in prj.conf (see Kconfig).
target_sources_ifdef(CONFIG_APP_DEMO_DRIVER app PRIVATE src/dma_driver.c)
target_sources_ifdef(CONFIG_APP_DEMO_BENCH  app PRIVATE src/signal_bench.c)
