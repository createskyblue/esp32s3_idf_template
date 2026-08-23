set(HOST_SRCS "")
if(CONFIG_BLE_ENABLED)
    list(APPEND HOST_SRCS "ble_host_test.c")
endif()

idf_component_register(SRCS "${HOST_SRCS}"
    INCLUDE_DIRS "."
    REQUIRES ble_host
    PRIV_REQUIRES bt log freertos)
