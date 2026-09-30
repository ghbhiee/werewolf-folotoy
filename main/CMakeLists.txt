idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "ww_core.c"
         "ww_host.c"
         "ww_api.c"
         "ww_ui.c"
         "ww_app.c"
         "ww_net.c"
         "ww_http.c"
         "ww_sound.c"
         "ww_voice.c"
         "ww_ai.c"
         "ww_cloud.c"
         "ww_brain.c"
         "font_pixel_12.c"
         "font_pixel_24.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_server esp_http_client esp-tls json mbedtls esp_netif esp_timer
             esp_wifi nvs_flash tcp_transport
)

# 玩家网页:构建时 gzip,原样嵌进固件,HTTP 以 Content-Encoding: gzip 吐出。
# -n 去掉时间戳,同样的输入得到同样的固件。
set(WEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../web")
foreach(page player setup keys)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${page}.html.gz"
        COMMAND gzip -9 -n -c "${WEB_DIR}/${page}.html" > "${CMAKE_CURRENT_BINARY_DIR}/${page}.html.gz"
        DEPENDS "${WEB_DIR}/${page}.html"
        VERBATIM)
    add_custom_target(ww_web_${page} DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${page}.html.gz")
    add_dependencies(${COMPONENT_LIB} ww_web_${page})
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/${page}.html.gz" BINARY
                           DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${page}.html.gz")
endforeach()
