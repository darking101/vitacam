cmake_minimum_required(VERSION 3.10)
project(VitaCam)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -ftree-vectorize -ffast-math -Wl,-q")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--section-start,.init_array=0x81100000")


add_executable(vitacam src/main.c src/qrcodegen.c src/webserver.c)

target_link_libraries(vitacam 
    vita2d 
    SceCamera_stub 
    SceJpegEnc_stub
    SceJpegEncArm_stub
    SceRtc_stub
    SceDisplay_stub 
    SceGxm_stub 
    SceCtrl_stub 
    SceTouch_stub
    ScePower_stub
    SceSysmodule_stub 
    SceAppMgr_stub
    SceCommonDialog_stub
    ScePgf_stub
    SceAudioIn_stub
    SceAudio_stub
    SceNet_stub
    SceNetCtl_stub
    png
    jpeg
    z
    m 
    c
)

include("${VITASDK}/share/vita.cmake" REQUIRED)

vita_create_self(eboot.bin vitacam UNSAFE TRUE)

vita_create_vpk(vitacam.vpk "VCAM00001" eboot.bin
    VERSION 01.00
    NAME "VitaCam"
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    FILE sce_sys/icons/btn_cross.png    sce_sys/icons/btn_cross.png
    FILE sce_sys/icons/btn_circle.png   sce_sys/icons/btn_circle.png
    FILE sce_sys/icons/btn_triangle.png sce_sys/icons/btn_triangle.png
    FILE sce_sys/icons/btn_trash.png    sce_sys/icons/btn_trash.png
    FILE sce_sys/icons/btn_back.png     sce_sys/icons/btn_back.png
    FILE sce_sys/icons/btn_check.png    sce_sys/icons/btn_check.png
    FILE sce_sys/icons/btn_select.png   sce_sys/icons/btn_select.png
    FILE sce_sys/icons/btn_camera.png   sce_sys/icons/btn_camera.png
    FILE sce_sys/icons/btn_left.png     sce_sys/icons/btn_left.png
    FILE sce_sys/icons/btn_right.png    sce_sys/icons/btn_right.png
    FILE sce_sys/icons/cam_flip.png     sce_sys/icons/cam_flip.png
    FILE sce_sys/icons/cam_flash_off.png sce_sys/icons/cam_flash_off.png
    FILE sce_sys/icons/cam_flash_screen.png sce_sys/icons/cam_flash_screen.png
    FILE sce_sys/icons/cam_flash_ring.png sce_sys/icons/cam_flash_ring.png
    FILE sce_sys/icons/cam_grid.png     sce_sys/icons/cam_grid.png
    FILE sce_sys/icons/cam_wm.png       sce_sys/icons/cam_wm.png
    FILE sce_sys/icons/cam_sound_on.png sce_sys/icons/cam_sound_on.png
    FILE sce_sys/icons/cam_sound_off.png sce_sys/icons/cam_sound_off.png
    FILE sce_sys/icons/cam_timer.png    sce_sys/icons/cam_timer.png
    FILE sce_sys/icons/cam_burst.png    sce_sys/icons/cam_burst.png
)

