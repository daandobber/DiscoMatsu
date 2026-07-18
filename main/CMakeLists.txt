idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu)
endif()
if("${idf_target}" STREQUAL "esp32p4" OR "${idf_target}" STREQUAL "esp32s2" OR "${idf_target}" STREQUAL "esp32s3")
list(APPEND usb_srcs "usb_msc_scsi.c")
list(APPEND usb_requires usb)
endif()

idf_component_register(
	SRCS
		"audio_output.c"
		${usb_srcs}
		"cdrom_audio.c"
		"cdplayer_task.c"
		"cd_ripper.c"
		"dobber_splash.c"
		"sd_storage.c"
		"wifi_setup.c"
		"wifi_file_server.c"
		"cd_metadata.c"
		"lastfm_config_file.c"
		"lastfm_scrobbler.c"
		"main.c"
	PRIV_REQUIRES
		esp_lcd
		nvs_flash
		fatfs
		sdmmc
		esp_driver_sdmmc
		badge-bsp
		${usb_requires}
		esp_driver_i2s
		esp_rom
		esp_timer
		tanmatsu-wifi
		bt
		esp_http_client
		esp_http_server
		esp_netif
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
