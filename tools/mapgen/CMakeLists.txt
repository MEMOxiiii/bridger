# bridger_mapgen: compiles mapping sources into src/translate/data/*.brmap.
# Developer tool; it is not shipped and CI does not run it (it needs Mojang's server
# jar, fetched by fetch_sources.py). The generated blob is committed.
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(bridger_mapgen main.cpp)
target_link_libraries(bridger_mapgen PRIVATE bridger::core bridger::io nlohmann_json::nlohmann_json)
bridger_configure_executable(bridger_mapgen)

# bridger_targetgen: the Bedrock target versions (src/translate/data/bedrock-targets.nbt), run by
# build_bedrock_targets.py. Developer tool like bridger_mapgen; the output is committed.
add_executable(bridger_targetgen targetgen.cpp)
target_link_libraries(bridger_targetgen PRIVATE bridger::convert bridger::translate bridger::io bridger::platform nlohmann_json::nlohmann_json)
bridger_configure_executable(bridger_targetgen)
