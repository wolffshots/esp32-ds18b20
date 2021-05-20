set(COMPONENT_ADD_INCLUDEDIRS include)
set(COMPONENT_SRCS "ds18b20_wrapper.c" "ds18b20.c")
set(COMPONENT_PRIV_REQUIRES "esp32-owb" "esp32-utility")
register_component()


