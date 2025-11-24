import gzip
import os

INPUT_FILE = "index.html"
CPP_OUTPUT = "index_html_gz.cpp"
H_OUTPUT = "index_html_gz.h"
ARRAY_NAME = "index_html_gz"


# --- html files from the same folder as this script ---
script_dir = os.path.dirname(os.path.abspath(__file__))
os.chdir(script_dir)


# Read HTML file
with open(INPUT_FILE, "rb") as f:
    data = f.read()

# Gzip compress
compressed = gzip.compress(data)

# Convert bytes → C hex array
hex_array = ", ".join(f"0x{b:02x}" for b in compressed)

# Gzip length
array_len = len(compressed)

# --------------------------------------
# Generate header (.h)
# --------------------------------------
h_content = f"""#pragma once
#include <stdint.h>
#include <stddef.h>

extern const uint8_t {ARRAY_NAME}[];
extern const size_t {ARRAY_NAME}_len;

"""

with open(H_OUTPUT, "w") as f:
    f.write(h_content)

print(f"✔ Generated {H_OUTPUT}")


# --------------------------------------
# Generate source (.cpp)
# --------------------------------------
cpp_content = f"""#include <Arduino.h>
#include <pgmspace.h>
#include "{H_OUTPUT}"

const uint8_t {ARRAY_NAME}[] PROGMEM = {{
    {hex_array}
}};

const size_t {ARRAY_NAME}_len = {array_len};
"""

with open(CPP_OUTPUT, "w") as f:
    f.write(cpp_content)

print(f"✔ Generated {CPP_OUTPUT}")
print(f"✔ GZIP size: {array_len} bytes")
