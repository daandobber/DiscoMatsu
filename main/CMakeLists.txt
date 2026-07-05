idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()

idf_component_register(
	SRCS
		"audio_output.c"
		"usb_msc_scsi.c"
		"cdrom_audio.c"
		"cdplayer_task.c"
		"wifi_setup.c"
		"cd_metadata.c"
		"main.c"
	PRIV_REQUIRES
		esp_lcd
		nvs_flash
		badge-bsp
		usb
		esp_driver_i2s
		esp_rom
		esp_timer
		tanmatsu-wifi
		bt
		esp_http_client
		esp_driver_jpeg
		esp_jpeg
		esp-tls
		mbedtls
		json
		${extra_requires}
	INCLUDE_DIRS
		"."
)

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
