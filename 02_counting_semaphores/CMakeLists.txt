cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(counting_semaphores)

target_sources(app PRIVATE src/main.c)
# Exactly one demo is compiled in, chosen in prj.conf (see Kconfig).
target_sources_ifdef(CONFIG_APP_DEMO_EVENTS app PRIVATE src/event_counter.c)
target_sources_ifdef(CONFIG_APP_DEMO_POOL   app PRIVATE src/buffer_pool.c)
