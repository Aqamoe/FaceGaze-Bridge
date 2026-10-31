# 四个无硬件示例；各自的目录也可以单独 cmake -S 构建。
foreach(sample native-sine native-face native-gaze native-openness)
    add_subdirectory(${sample})
endforeach()
