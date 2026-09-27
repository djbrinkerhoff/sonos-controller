idf_component_register(SRCS "sonos.cpp" "vendor/tinyxml2.cpp" INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "vendor")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
