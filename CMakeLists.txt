cmake_minimum_required(VERSION 3.17)
project(FFmpegAndSDLTutorial C)

set(CMAKE_C_STANDARD 99)

include_directories(
        /usr/local/include
        /usr/local/opt/zlib/include
        /usr/local/opt/xz/include
)
link_directories(
        /usr/local/lib
        /usr/local/opt/zlib/lib
        /usr/local/opt/xz/lib
)

add_executable(FFmpegAndSDLTutorial main.c)

target_link_libraries(
        FFmpegAndSDLTutorial
        avformat
        avcodec
        avutil
        swresample
        swscale
        z
        lzma
        bz2
        iconv
        fdk-aac
        mp3lame
        x264
        x265
        freetype
        ass
        "-framework Foundation"
        "-framework Security"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework AudioToolbox"
        "-framework VideoToolbox"
)