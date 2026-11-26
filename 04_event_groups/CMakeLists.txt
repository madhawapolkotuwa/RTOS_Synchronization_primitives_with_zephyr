cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(event_groups)

target_sources(app PRIVATE src/main.c)
# Exactly one demo is compiled in, chosen in prj.conf (see Kconfig).
target_sources_ifdef(CONFIG_APP_DEMO_GATE     app PRIVATE src/startup_gate.c)
target_sources_ifdef(CONFIG_APP_DEMO_DISPATCH app PRIVATE src/dispatcher.c)
