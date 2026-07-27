import csv
import os
import re
import zipfile

Import("env")

partition_gen_path = "components/partition_table/gen_esp32part.py"
spiffsgen_path = "components/spiffs/spiffsgen.py"

files_to_zip = ["firmware.bin", "bootloader.bin", "partitions.bin", "ota_data_initial.bin", "partitions.csv"]
zip_filename = "firmware.zip"


def create_zip(files, output_filename):
    with zipfile.ZipFile(output_filename, 'w') as zipf:
        for file in files:
            zipf.write(file, arcname=os.path.basename(file))
    print(f"{output_filename} created successfully.")


def create_partitions_csv():
    print("Creating partitions.csv...")
    package_dir = env.PioPlatform().get_package_dir("framework-espidf")
    path = os.path.join(package_dir, partition_gen_path)
    part_bin_path = os.path.join(env.subst("$BUILD_DIR"), "partitions.bin")
    part_csv_path = os.path.join(env.subst("$BUILD_DIR"), "partitions.csv")
    env.Execute(f"python {path} {part_bin_path} {part_csv_path}")
    print(f"{part_csv_path} created successfully.")


def _sdkconfig_values():
    """SPIFFS parameters straight from the generated sdkconfig, so the image
    cannot drift from what the firmware was compiled to read."""
    path = os.path.join(env.subst("$PROJECT_DIR"),
                        "sdkconfig." + env.subst("$PIOENV"))
    values = {}
    with open(path) as fh:
        for line in fh:
            m = re.match(r"^(CONFIG_SPIFFS_\w+)=(.+)$", line.strip())
            if m:
                values[m.group(1)] = m.group(2)
    return values


def _spiffs_partition_size():
    """Size of the spiffs partition, read from partitions.csv rather than
    hardcoded, since this fork already moved it once."""
    path = os.path.join(env.subst("$PROJECT_DIR"), "partitions.csv")
    with open(path) as fh:
        for row in csv.reader(fh):
            if not row or row[0].strip().startswith("#"):
                continue
            if row[0].strip() == "spiffs":
                return int(row[4].strip(), 0)
    raise Exception("no spiffs partition in partitions.csv")


def rebuild_spiffs(source, target, env):
    """Replace PlatformIO's SPIFFS image with one ESP-IDF can actually read.

    PlatformIO packs the data directory with mkspiffs_espressif8266_arduino,
    which is built with the ESP8266 Arduino SPIFFS parameters. This project
    runs the ESP-IDF SPIFFS driver with CONFIG_SPIFFS_OBJ_NAME_LEN=56 and
    CONFIG_SPIFFS_META_LENGTH=4, so the object index layouts do not match:
    the device mounts the image, finds no files, and every icon silently
    falls back to the placeholder glyph.

    ESP-IDF ships spiffsgen.py for exactly this. Run it over the same data
    directory with the parameters read out of sdkconfig and overwrite the
    image in place, so `-t buildfs` and `-t uploadfs` keep working normally.
    """
    image = os.path.join(env.subst("$BUILD_DIR"), "spiffs.bin")
    data_dir = env.subst("$PROJECT_DATA_DIR")
    if not os.path.isdir(data_dir):
        print(f"no data dir at {data_dir}, leaving {image} alone")
        return

    cfg = _sdkconfig_values()
    package_dir = env.PioPlatform().get_package_dir("framework-espidf")
    script = os.path.join(package_dir, spiffsgen_path)

    cmd = [
        "python", script,
        "--page-size", cfg.get("CONFIG_SPIFFS_PAGE_SIZE", "256"),
        "--obj-name-len", cfg.get("CONFIG_SPIFFS_OBJ_NAME_LEN", "32"),
        "--meta-len", cfg.get("CONFIG_SPIFFS_META_LENGTH", "4"),
    ]
    if cfg.get("CONFIG_SPIFFS_USE_MAGIC") == "y":
        cmd.append("--use-magic")
    if cfg.get("CONFIG_SPIFFS_USE_MAGIC_LENGTH") == "y":
        cmd.append("--use-magic-len")
    cmd += [str(_spiffs_partition_size()), data_dir, image]

    print("#### REBUILDING SPIFFS IMAGE FOR ESP-IDF ####")
    print(" ".join(cmd))
    if env.Execute(" ".join(f'"{c}"' if " " in c else c for c in cmd)):
        raise Exception("spiffsgen.py failed")
    print(f"{image} regenerated with ESP-IDF SPIFFS parameters.")


def post_build(source, target, env):
    print("#### POST BUILD ####")
    create_partitions_csv()
    files = [os.path.join(env.subst("$BUILD_DIR"), file) for file in files_to_zip]
    create_zip(files, os.path.join(env.subst("$BUILD_DIR"), zip_filename))

print("#### POST SCRIPT ####")
env.AddPostAction("buildprog", post_build)
env.AddPostAction("$BUILD_DIR/spiffs.bin", rebuild_spiffs)
