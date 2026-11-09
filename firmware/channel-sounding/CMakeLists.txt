# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(channel_sounding)

target_sources(app PRIVATE src/main.c src/data_out.c)

# One application, two roles. The role .conf file selects which side is built.
target_sources_ifdef(CONFIG_APP_ROLE_INITIATOR app PRIVATE src/ranging_cs_initiator.c)
target_sources_ifdef(CONFIG_APP_ROLE_REFLECTOR app PRIVATE src/ranging_cs_reflector.c)
