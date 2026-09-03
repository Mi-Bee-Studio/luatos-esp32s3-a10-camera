cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3_a10_camera)

# 2026-09-02 契约 v1.1：与姐妹仓一致，构建时自动打包 web UI（此前需手动 spiffsgen）
file(GLOB MIBEE_UI_FILES main/web_ui/*)
# 2026-09-03：目录级依赖不随文件内容编辑触发（build/spiffs.bin 比新 app.js 旧），
# 必须显式 DEPENDS 文件列表，否则固件烧的是旧 UI
spiffs_create_partition_image(spiffs main/web_ui FLASH_IN_PROJECT DEPENDS ${MIBEE_UI_FILES})
