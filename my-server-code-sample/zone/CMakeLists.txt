cmake_minimum_required(VERSION 3.20)
project(Zone CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- パス変数 ----
set(PROJECT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
# dep は本プロジェクトの一つ上のディレクトリにある（zone/ と同じ階層）
set(DEP_LIBS     ${PROJECT_ROOT}/../dep)

list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(FLATC_EXECUTABLE flatc REQUIRED)

# ---- MySQL クライアント（従来プロトコル 3306）----
# X DevAPI(33060) から従来プロトコルへ移行済みで、libmysqlclient を使う。
set(MYSQL_CLIENT_ROOT "" CACHE PATH "Optional root path for MySQL client library")
set(_MYSQL_CLIENT_HINTS)
if(MYSQL_CLIENT_ROOT)
    list(APPEND _MYSQL_CLIENT_HINTS ${MYSQL_CLIENT_ROOT})
endif()
list(APPEND _MYSQL_CLIENT_HINTS
    /opt/homebrew/opt/mysql-client
    /opt/homebrew/opt/mysql@8.0
    /opt/homebrew/opt/mysql
    /usr/local/opt/mysql-client
    /usr/local
)

find_path(MYSQL_CLIENT_INCLUDE_DIR
    NAMES mysql.h
    HINTS ${_MYSQL_CLIENT_HINTS}
    PATH_SUFFIXES include/mysql include
)

find_library(MYSQL_CLIENT_LIBRARY
    NAMES mysqlclient
    HINTS ${_MYSQL_CLIENT_HINTS}
    PATH_SUFFIXES lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MySQLClient
    REQUIRED_VARS MYSQL_CLIENT_INCLUDE_DIR MYSQL_CLIENT_LIBRARY
)

# ---- パフォーマンス向けのコンパイルオプション ----
add_compile_options(
    -O3
    -flto
    -Wall
    -Wextra
)

# ---- EnTT のスパースセットのページサイズ ----
# このプロジェクトではメインシティごとに独立した小さな registry（約 20 エンティティ）を持つが、
# EnTT のデフォルトではコンポーネントプールごとに 4096/1024 要素単位でページを確保するため、
# ほとんどのページが空になる。実測ではプレイヤーあたり 244 KB で、その 99% が空ページの無駄だった。
#
# 64/64 に変更した結果、実測でプレイヤーあたり 21 KB まで減った（1 万人同時接続で 2.33 GB -> 0.20 GB）。
#
# add_compile_definitions で全体に適用すること。この 2 つのマクロは EnTT のメモリレイアウトを変えるので、
# 1 つでも定義の異なるコンパイル単位があると ODR 違反になり、実行時に原因の分かりにくいクラッシュを起こす。
#
# HINT: 将来「複数のシティで 1 つの大きな registry を共有する」構成（registry あたり数万エンティティ）に
# 変える場合は、この 2 つの値で改めて負荷テストを行うこと。大きな集合では小さいページがページポインタの
# 間接参照を増やす。
add_compile_definitions(
    ENTT_SPARSE_PAGE=64
    ENTT_PACKED_PAGE=64
)

# ── FlatBuffers スキーマからの自動生成 ──────────────────────────────────
set(FLATC_OUTPUT ${PROJECT_ROOT}/include/generated/game_generated.h)
set(SCHEMA_FILE  ${PROJECT_ROOT}/../schema/game.fbs)

add_custom_command(
    OUTPUT  ${FLATC_OUTPUT}
    COMMAND ${FLATC_EXECUTABLE} --cpp --gen-mutable -o ${PROJECT_ROOT}/include/generated
            ${SCHEMA_FILE}
    DEPENDS ${SCHEMA_FILE}
    COMMENT "Zone: Generating FlatBuffers headers..."
)

add_custom_target(zone_generate_fbs DEPENDS ${FLATC_OUTPUT})

# ── ナビゲーション JSON からヘッダーを自動生成 ───────────────────────────────────
set(NAV_JSON      ${PROJECT_ROOT}/nav/main_city_nav_32x32.json)
set(NAV_HEADER    ${PROJECT_ROOT}/include/generated/main_city_nav_32x32.h)
set(NAV_GENERATOR ${PROJECT_ROOT}/tools/nav_json_to_header.py)

add_custom_command(
    OUTPUT ${NAV_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${NAV_GENERATOR} ${NAV_JSON} ${NAV_HEADER}
    DEPENDS ${NAV_JSON} ${NAV_GENERATOR}
    COMMENT "Zone: Generating main city navigation header..."
)

add_custom_target(zone_generate_nav DEPENDS ${NAV_HEADER})

# ---- 外部依存ライブラリの検索 ----
find_package(OpenSSL     REQUIRED)
find_package(flatbuffers REQUIRED)
find_package(EnTT        REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED hiredis)

# ── ECS のソースファイル（現在メンテナンスしているシステムを自動で収集する）──────────────────────
file(GLOB_RECURSE ECS_SOURCES CONFIGURE_DEPENDS ${PROJECT_ROOT}/src/ecs/*.cpp)

message(STATUS "Zone ECS sources (active): ${ECS_SOURCES}")

# ================================================================
# Target: zone
# ================================================================
add_executable(zone
    ${PROJECT_ROOT}/src/main.cpp
    ${PROJECT_ROOT}/src/auth/join_ticket.cpp
    ${PROJECT_ROOT}/src/auth/pass_token.cpp
    ${PROJECT_ROOT}/src/game/battle_event_pack.cpp
    ${PROJECT_ROOT}/src/game/game_loop.cpp
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/src/game/nav_astar.cpp
    ${PROJECT_ROOT}/src/game/player_team_runtime.cpp
    ${PROJECT_ROOT}/src/game/equip_handler.cpp
    ${PROJECT_ROOT}/src/game/shop_handler.cpp
    ${ECS_SOURCES}
    ${PROJECT_ROOT}/src/db/mysql_client.cpp
    ${PROJECT_ROOT}/src/db/mysql_worker.cpp
    ${PROJECT_ROOT}/src/db/redis_worker.cpp
    ${PROJECT_ROOT}/src/db/player_ship_db_writer.cpp
    ${PROJECT_ROOT}/src/db/role_economy_db_writer.cpp
    ${PROJECT_ROOT}/src/network/zone_http.cpp
    ${PROJECT_ROOT}/src/config/env_loader.cpp
    ${PROJECT_ROOT}/src/runtime/shutdown.cpp
)

add_dependencies(zone zone_generate_fbs zone_generate_nav)

# ビルド完了後、プロジェクトルートの bin/ へ自動でコピーする
set(BIN_DIR ${PROJECT_ROOT}/../bin)
add_custom_command(TARGET zone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BIN_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:zone> ${BIN_DIR}/zone
    COMMENT "Copied zone → bin/zone"
)

target_include_directories(zone PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${OPENSSL_INCLUDE_DIR}
    ${DEP_LIBS}/uWebSockets/src
    ${DEP_LIBS}/uWebSockets/uSockets/src
    ${HIREDIS_INCLUDE_DIRS}
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_directories(zone PRIVATE ${HIREDIS_LIBRARY_DIRS})

target_link_libraries(zone PRIVATE
    ${DEP_LIBS}/uWebSockets/uSockets/uSockets.a
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${HIREDIS_LIBRARIES}
    ${MYSQL_CLIENT_LIBRARY}
    ${OPENSSL_LIBRARIES}
    z
)

add_executable(zone_player_team_runtime_test
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/src/game/player_team_runtime.cpp
    ${PROJECT_ROOT}/tests/player_team_runtime_test.cpp
)

add_dependencies(zone_player_team_runtime_test zone_generate_fbs zone_generate_nav)

target_include_directories(zone_player_team_runtime_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_player_team_runtime_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_main_city_nav_test
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/src/game/nav_astar.cpp
    ${PROJECT_ROOT}/tests/main_city_nav_test.cpp
)

add_dependencies(zone_main_city_nav_test zone_generate_nav)

target_include_directories(zone_main_city_nav_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_main_city_nav_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_nav_runtime_test
    ${PROJECT_ROOT}/src/game/battle_event_pack.cpp
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/src/game/nav_astar.cpp
    ${PROJECT_ROOT}/src/ecs/area_registry_system.cpp
    ${PROJECT_ROOT}/src/ecs/city_monster_combat_system.cpp
    ${PROJECT_ROOT}/src/ecs/nav_request_system.cpp
    ${PROJECT_ROOT}/src/ecs/nav_follow_system.cpp
    ${PROJECT_ROOT}/src/ecs/player_ship_ai_system.cpp
    ${PROJECT_ROOT}/tests/nav_runtime_test.cpp
)

add_dependencies(zone_nav_runtime_test zone_generate_nav zone_generate_fbs)

target_include_directories(zone_nav_runtime_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_nav_runtime_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_combat_result_event_test
    ${PROJECT_ROOT}/tests/combat_result_event_test.cpp
)

add_dependencies(zone_combat_result_event_test zone_generate_fbs)

target_include_directories(zone_combat_result_event_test PRIVATE
    ${PROJECT_ROOT}/include
)

target_link_libraries(zone_combat_result_event_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
)

add_executable(zone_battle_event_pack_test
    ${PROJECT_ROOT}/src/game/battle_event_pack.cpp
    ${PROJECT_ROOT}/tests/battle_event_pack_test.cpp
)

add_dependencies(zone_battle_event_pack_test zone_generate_fbs)

target_include_directories(zone_battle_event_pack_test PRIVATE
    ${PROJECT_ROOT}/include
)

target_link_libraries(zone_battle_event_pack_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
)

add_executable(zone_aoi_snapshot_contract_test
    ${PROJECT_ROOT}/tests/aoi_snapshot_contract_test.cpp
)

add_dependencies(zone_aoi_snapshot_contract_test zone_generate_fbs)

target_include_directories(zone_aoi_snapshot_contract_test PRIVATE
    ${PROJECT_ROOT}/include
)

target_link_libraries(zone_aoi_snapshot_contract_test PRIVATE
    flatbuffers::flatbuffers
)

add_executable(zone_aoi_viewer_state_test
    ${PROJECT_ROOT}/tests/aoi_viewer_state_test.cpp
)

target_include_directories(zone_aoi_viewer_state_test PRIVATE
    ${PROJECT_ROOT}/include
)

target_link_libraries(zone_aoi_viewer_state_test PRIVATE
    EnTT::EnTT
)

add_executable(zone_main_startup_test
    ${PROJECT_ROOT}/src/auth/join_ticket.cpp
    ${PROJECT_ROOT}/src/runtime/shutdown.cpp
    ${PROJECT_ROOT}/tests/main_startup_test.cpp
)

target_include_directories(zone_main_startup_test PRIVATE
    ${PROJECT_ROOT}/include
    ${OPENSSL_INCLUDE_DIR}
)

target_compile_definitions(zone_main_startup_test PRIVATE
    MYGAMESERVER_PROJECT_ROOT="${PROJECT_ROOT}/.."
)

target_link_libraries(zone_main_startup_test PRIVATE
    ${OPENSSL_LIBRARIES}
)

add_executable(zone_join_ticket_test
    ${PROJECT_ROOT}/src/auth/join_ticket.cpp
    ${PROJECT_ROOT}/tests/join_ticket_test.cpp
)

target_include_directories(zone_join_ticket_test PRIVATE
    ${PROJECT_ROOT}/include
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(zone_join_ticket_test PRIVATE
    ${OPENSSL_LIBRARIES}
)

add_executable(zone_pass_token_test
    ${PROJECT_ROOT}/src/auth/pass_token.cpp
    ${PROJECT_ROOT}/tests/pass_token_test.cpp
)

target_include_directories(zone_pass_token_test PRIVATE
    ${PROJECT_ROOT}/include
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(zone_pass_token_test PRIVATE
    ${OPENSSL_LIBRARIES}
)

add_executable(zone_redis_auth_scripts_test
    ${PROJECT_ROOT}/tests/redis_auth_scripts_test.cpp
)

target_include_directories(zone_redis_auth_scripts_test PRIVATE
    ${PROJECT_ROOT}/include
)

add_executable(zone_safe_utils_test
    ${PROJECT_ROOT}/tests/safe_utils_test.cpp
)

target_include_directories(zone_safe_utils_test PRIVATE
    ${PROJECT_ROOT}/include
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_safe_utils_test PRIVATE
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_redis_worker_offline_forward_test
    ${PROJECT_ROOT}/src/db/redis_worker.cpp
    ${PROJECT_ROOT}/src/runtime/shutdown.cpp
    ${PROJECT_ROOT}/tests/redis_worker_offline_forward_test.cpp
)

target_include_directories(zone_redis_worker_offline_forward_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${HIREDIS_INCLUDE_DIRS}
)

target_link_directories(zone_redis_worker_offline_forward_test PRIVATE
    ${HIREDIS_LIBRARY_DIRS}
)

target_link_libraries(zone_redis_worker_offline_forward_test PRIVATE
    ${HIREDIS_LIBRARIES}
)

add_executable(zone_db_schema_contract_test
    ${PROJECT_ROOT}/tests/db_schema_contract_test.cpp
)

target_include_directories(zone_db_schema_contract_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
)

target_compile_definitions(zone_db_schema_contract_test PRIVATE
    MYGAMESERVER_PROJECT_ROOT="${PROJECT_ROOT}/.."
)

target_link_libraries(zone_db_schema_contract_test PRIVATE
    EnTT::EnTT
)

add_executable(zone_game_loop_session_contract_test
    ${PROJECT_ROOT}/tests/game_loop_session_contract_test.cpp
)

add_dependencies(zone_game_loop_session_contract_test zone_generate_fbs)

target_include_directories(zone_game_loop_session_contract_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
)

target_link_libraries(zone_game_loop_session_contract_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
)

add_executable(zone_shutdown_runtime_test
    ${PROJECT_ROOT}/src/runtime/shutdown.cpp
    ${PROJECT_ROOT}/tests/shutdown_runtime_test.cpp
)

target_include_directories(zone_shutdown_runtime_test PRIVATE
    ${PROJECT_ROOT}/include
)

add_executable(zone_shop_handler_test
    ${PROJECT_ROOT}/src/game/equip_handler.cpp
    ${PROJECT_ROOT}/src/game/shop_handler.cpp
    ${PROJECT_ROOT}/tests/shop_handler_test.cpp
)

target_include_directories(zone_shop_handler_test PRIVATE
    ${PROJECT_ROOT}/include
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_shop_handler_test PRIVATE
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_equip_handler_test
    ${PROJECT_ROOT}/src/game/equip_handler.cpp
    ${PROJECT_ROOT}/tests/equip_handler_test.cpp
)

target_include_directories(zone_equip_handler_test PRIVATE
    ${PROJECT_ROOT}/include
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_equip_handler_test PRIVATE
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_equip_stats_test
    ${PROJECT_ROOT}/tests/equip_stats_test.cpp
)

target_include_directories(zone_equip_stats_test PRIVATE
    ${PROJECT_ROOT}/include
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_equip_stats_test PRIVATE
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_team_ops_test
    ${PROJECT_ROOT}/src/db/role_economy_db_writer.cpp
    ${PROJECT_ROOT}/tests/team_ops_test.cpp
)

add_dependencies(zone_team_ops_test zone_generate_fbs)

target_include_directories(zone_team_ops_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_team_ops_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_item_drop_test
    ${PROJECT_ROOT}/src/ecs/item_drop_system.cpp
    ${PROJECT_ROOT}/tests/item_drop_test.cpp
)

add_dependencies(zone_item_drop_test zone_generate_fbs)

target_include_directories(zone_item_drop_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_item_drop_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_boss_challenge_test
    ${PROJECT_ROOT}/src/ecs/boss_challenge_system.cpp
    ${PROJECT_ROOT}/src/ecs/city_monster_factory.cpp
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/tests/boss_challenge_test.cpp
)

add_dependencies(zone_boss_challenge_test zone_generate_fbs zone_generate_nav)

target_include_directories(zone_boss_challenge_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_boss_challenge_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_city_isolation_test
    ${PROJECT_ROOT}/src/ecs/boss_challenge_system.cpp
    ${PROJECT_ROOT}/src/ecs/city_monster_factory.cpp
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/tests/city_isolation_test.cpp
)

add_dependencies(zone_city_isolation_test zone_generate_fbs zone_generate_nav)

target_include_directories(zone_city_isolation_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_city_isolation_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_team_switch_movement_test
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/src/game/nav_astar.cpp
    ${PROJECT_ROOT}/src/ecs/area_registry_system.cpp
    ${PROJECT_ROOT}/src/ecs/nav_request_system.cpp
    ${PROJECT_ROOT}/src/ecs/nav_follow_system.cpp
    ${PROJECT_ROOT}/src/ecs/player_ship_ai_system.cpp
    ${PROJECT_ROOT}/src/ecs/city_monster_combat_system.cpp
    ${PROJECT_ROOT}/src/game/battle_event_pack.cpp
    ${PROJECT_ROOT}/tests/team_switch_movement_test.cpp
)

add_dependencies(zone_team_switch_movement_test zone_generate_fbs zone_generate_nav)

target_include_directories(zone_team_switch_movement_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_team_switch_movement_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_retreat_target_test
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/src/game/nav_astar.cpp
    ${PROJECT_ROOT}/src/ecs/area_registry_system.cpp
    ${PROJECT_ROOT}/src/ecs/player_ship_ai_system.cpp
    ${PROJECT_ROOT}/src/ecs/city_monster_combat_system.cpp
    ${PROJECT_ROOT}/src/game/battle_event_pack.cpp
    ${PROJECT_ROOT}/tests/retreat_target_test.cpp
)

add_dependencies(zone_retreat_target_test zone_generate_fbs zone_generate_nav)

target_include_directories(zone_retreat_target_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_retreat_target_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_team_exp_share_test
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/src/game/nav_astar.cpp
    ${PROJECT_ROOT}/src/ecs/area_registry_system.cpp
    ${PROJECT_ROOT}/src/ecs/player_ship_ai_system.cpp
    ${PROJECT_ROOT}/src/ecs/city_monster_combat_system.cpp
    ${PROJECT_ROOT}/src/game/battle_event_pack.cpp
    ${PROJECT_ROOT}/tests/team_exp_share_test.cpp
)

add_dependencies(zone_team_exp_share_test zone_generate_fbs zone_generate_nav)

target_include_directories(zone_team_exp_share_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_team_exp_share_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_unit_static_data_test
    ${PROJECT_ROOT}/tests/unit_static_data_test.cpp
)

add_dependencies(zone_unit_static_data_test zone_generate_fbs)

target_include_directories(zone_unit_static_data_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_unit_static_data_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_nearest_target_test
    ${PROJECT_ROOT}/src/ecs/area_registry_system.cpp
    ${PROJECT_ROOT}/tests/nearest_target_test.cpp
)

add_dependencies(zone_nearest_target_test zone_generate_fbs)

target_include_directories(zone_nearest_target_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_nearest_target_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_transit_immunity_test
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/src/game/nav_astar.cpp
    ${PROJECT_ROOT}/src/ecs/area_registry_system.cpp
    ${PROJECT_ROOT}/src/ecs/city_monster_combat_system.cpp
    ${PROJECT_ROOT}/tests/transit_immunity_test.cpp
)

add_dependencies(zone_transit_immunity_test zone_generate_fbs zone_generate_nav)

target_include_directories(zone_transit_immunity_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_transit_immunity_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_kill_exp_test
    ${PROJECT_ROOT}/tests/kill_exp_test.cpp
)

add_dependencies(zone_kill_exp_test zone_generate_fbs)

target_include_directories(zone_kill_exp_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_kill_exp_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_healing_station_test
    ${PROJECT_ROOT}/src/ecs/healing_station_system.cpp
    ${PROJECT_ROOT}/tests/healing_station_test.cpp
)

add_dependencies(zone_healing_station_test zone_generate_fbs)

target_include_directories(zone_healing_station_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_healing_station_test PRIVATE
    flatbuffers::flatbuffers
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_city_nav_grid_test
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/src/game/nav_astar.cpp
    ${PROJECT_ROOT}/tests/city_nav_grid_test.cpp
)

add_dependencies(zone_city_nav_grid_test zone_generate_nav)

target_include_directories(zone_city_nav_grid_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_city_nav_grid_test PRIVATE
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_building_placement_test
    ${PROJECT_ROOT}/src/game/nav_grid.cpp
    ${PROJECT_ROOT}/tests/building_placement_test.cpp
)

add_dependencies(zone_building_placement_test zone_generate_nav)

target_include_directories(zone_building_placement_test PRIVATE
    ${PROJECT_ROOT}/include
    ${DEP_LIBS}/concurrentqueue
    ${MYSQL_CLIENT_INCLUDE_DIR}
)

target_link_libraries(zone_building_placement_test PRIVATE
    EnTT::EnTT
    ${MYSQL_CLIENT_LIBRARY}
)

add_executable(zone_live_probe_codec
    ${PROJECT_ROOT}/tools/live_probe_codec.cpp
)

add_dependencies(zone_live_probe_codec zone_generate_fbs)

target_include_directories(zone_live_probe_codec PRIVATE
    ${PROJECT_ROOT}/include
)

target_link_libraries(zone_live_probe_codec PRIVATE
    flatbuffers::flatbuffers
)

enable_testing()

add_test(NAME zone_player_team_runtime_test COMMAND zone_player_team_runtime_test)
add_test(NAME zone_main_city_nav_test COMMAND zone_main_city_nav_test)
add_test(NAME zone_nav_runtime_test COMMAND zone_nav_runtime_test)
add_test(NAME zone_combat_result_event_test COMMAND zone_combat_result_event_test)
add_test(NAME zone_battle_event_pack_test COMMAND zone_battle_event_pack_test)
add_test(NAME zone_aoi_snapshot_contract_test COMMAND zone_aoi_snapshot_contract_test)
add_test(NAME zone_aoi_viewer_state_test COMMAND zone_aoi_viewer_state_test)
add_test(NAME zone_main_startup_test COMMAND zone_main_startup_test)
add_test(NAME zone_join_ticket_test COMMAND zone_join_ticket_test)
add_test(NAME zone_pass_token_test COMMAND zone_pass_token_test)
add_test(NAME zone_redis_auth_scripts_test COMMAND zone_redis_auth_scripts_test)
add_test(NAME zone_safe_utils_test COMMAND zone_safe_utils_test)
add_test(NAME zone_redis_worker_offline_forward_test COMMAND zone_redis_worker_offline_forward_test)
add_test(NAME zone_db_schema_contract_test COMMAND zone_db_schema_contract_test)
add_test(NAME zone_game_loop_session_contract_test COMMAND zone_game_loop_session_contract_test)
add_test(NAME zone_shutdown_runtime_test COMMAND zone_shutdown_runtime_test)
add_test(NAME zone_shop_handler_test COMMAND zone_shop_handler_test)
add_test(NAME zone_equip_handler_test COMMAND zone_equip_handler_test)
add_test(NAME zone_equip_stats_test COMMAND zone_equip_stats_test)
add_test(NAME zone_team_ops_test COMMAND zone_team_ops_test)
add_test(NAME zone_item_drop_test COMMAND zone_item_drop_test)
add_test(NAME zone_boss_challenge_test COMMAND zone_boss_challenge_test)
add_test(NAME zone_city_isolation_test COMMAND zone_city_isolation_test)
add_test(NAME zone_team_switch_movement_test COMMAND zone_team_switch_movement_test)
add_test(NAME zone_healing_station_test COMMAND zone_healing_station_test)
add_test(NAME zone_kill_exp_test COMMAND zone_kill_exp_test)
add_test(NAME zone_transit_immunity_test COMMAND zone_transit_immunity_test)
add_test(NAME zone_nearest_target_test COMMAND zone_nearest_target_test)
add_test(NAME zone_unit_static_data_test COMMAND zone_unit_static_data_test)
add_test(NAME zone_team_exp_share_test COMMAND zone_team_exp_share_test)
add_test(NAME zone_retreat_target_test COMMAND zone_retreat_target_test)
add_test(NAME zone_city_nav_grid_test COMMAND zone_city_nav_grid_test)
add_test(NAME zone_building_placement_test COMMAND zone_building_placement_test)
add_test(NAME zone_live_probe_codec_self_test COMMAND zone_live_probe_codec self-test)
add_test(NAME zone_gateway_live_probe_unit_test
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_ROOT}/../tools/test_zone_gateway_live_probe.py)
