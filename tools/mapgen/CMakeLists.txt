# bridger_mapgen: compiles mapping sources into src/translate/data/*.brmap.
# Developer tool; it is not shipped and CI does not run it (it needs Mojang's server
# jar, fetched by fetch_sources.py). The generated blob is committed.
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(bridger_mapgen main.cpp)
target_link_libraries(bridger_mapgen PRIVATE bridger::core bridger::io nlohmann_json::nlohmann_json)
bridger_configure_executable(bridger_mapgen)
