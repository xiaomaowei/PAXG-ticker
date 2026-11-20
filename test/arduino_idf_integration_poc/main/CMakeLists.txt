idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES arduino TFT_eSPI usb_device_composite tinyusb
)
