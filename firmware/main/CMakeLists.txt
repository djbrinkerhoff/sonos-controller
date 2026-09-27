idf_component_register(SRCS "main.cpp" "network.cpp" INCLUDE_DIRS "." REQUIRES sonos nvs_flash esp_wifi esp_netif esp_event esp_http_client lwip)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
