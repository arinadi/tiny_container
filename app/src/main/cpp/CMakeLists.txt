cmake_minimum_required(VERSION 3.18.1)
project("tiny_jni")

find_library(aaudio-lib aaudio)

# ── TinyAudio (speaker / playback) ────────────────────────────────
add_library(tiny_audio_jni SHARED tiny_audio_jni.c)
target_link_libraries(tiny_audio_jni ${aaudio-lib} log)
target_compile_options(tiny_audio_jni PRIVATE -Wall -Wextra -O2 -std=c11)

# ── TinyMicrophone (capture) ──────────────────────────────────────
add_library(tiny_microphone_jni SHARED tiny_microphone_jni.c)
target_link_libraries(tiny_microphone_jni ${aaudio-lib} log)
target_compile_options(tiny_microphone_jni PRIVATE -Wall -Wextra -O2 -std=c11)

# ── TinyIpp (IPP print server) ────────────────────────────────────
add_library(tiny_ipp_jni SHARED tiny_ipp_jni.c)
target_link_libraries(tiny_ipp_jni log)
target_compile_options(tiny_ipp_jni PRIVATE -Wall -Wextra -O2 -std=c11)





