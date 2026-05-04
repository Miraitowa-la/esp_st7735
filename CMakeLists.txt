set(public_requires esp_common)

if(IDF_VERSION_MAJOR GREATER 5 OR (IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR GREATER_EQUAL 3))
    list(APPEND public_requires esp_driver_gpio esp_driver_spi)
else()
    list(APPEND public_requires driver)
endif()

idf_component_register(
    SRCS "src/st7735.c"
    INCLUDE_DIRS "include"
    REQUIRES ${public_requires}
    PRIV_REQUIRES freertos heap
)
