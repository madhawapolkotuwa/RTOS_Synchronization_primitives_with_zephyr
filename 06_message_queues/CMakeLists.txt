cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(message_queues)

target_sources(app PRIVATE src/main.c)
# Exactly one demo is compiled in, chosen in prj.conf (see Kconfig).
target_sources_ifdef(CONFIG_APP_DEMO_SENSOR  app PRIVATE src/sensor_pipeline.c)
target_sources_ifdef(CONFIG_APP_DEMO_PACKETS app PRIVATE src/packet_pool.c)
