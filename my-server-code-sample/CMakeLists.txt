cmake_minimum_required(VERSION 3.20)
project(MyGameServer)

# clangd / Zed などのエディタ向けに compile_commands.json を出力する
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(zone)
