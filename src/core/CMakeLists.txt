bridger_add_module(core
    PUBLIC_LIBS  tl::expected
    PRIVATE_LIBS xxHash::xxhash TBB::tbb spdlog::spdlog)

target_compile_definitions(bridger_core PUBLIC
    BRIDGER_VERSION_STRING="${PROJECT_VERSION}")
