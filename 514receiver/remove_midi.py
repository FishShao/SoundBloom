import os
import glob

# 编译前自动删除 ESP8266Audio 中有问题的 MIDI 和 Opus 文件
# 这些模块在 ESP32-C3 上编译不过，但我们只需要 MP3

def remove_files(pattern):
    for f in glob.glob(pattern, recursive=True):
        print(f"  Removing: {f}")
        os.remove(f)

# PlatformIO 的库缓存路径
libdir = os.path.join(".", ".pio", "libdeps", "xiaoesp32c3", "ESP8266Audio", "src")

if os.path.exists(libdir):
    print(">> Removing problematic ESP8266Audio modules...")
    for name in ["AudioGeneratorMIDI", "AudioGeneratorOpus"]:
        for ext in [".cpp", ".h"]:
            filepath = os.path.join(libdir, name + ext)
            if os.path.exists(filepath):
                print(f"  Removing: {filepath}")
                os.remove(filepath)
    # 也删掉 libtinysoundfont 和 libopus 目录（如果存在）
    for subdir in ["libtinysoundfont", "libopus"]:
        dirpath = os.path.join(libdir, subdir)
        if os.path.isdir(dirpath):
            import shutil
            print(f"  Removing dir: {dirpath}")
            shutil.rmtree(dirpath)
    print(">> Done cleaning.")
else:
    print(">> ESP8266Audio not yet downloaded, will clean on next build.")
