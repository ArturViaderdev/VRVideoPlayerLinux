#!/bin/sh
# Builds vr-mvcdec (3D Blu-ray MVC decoder). Needs gcc/g++ (C17/C++17), nothing else.
set -e
cd "$(dirname "$0")"
mkdir -p build
CC=${CC:-gcc}; CXX=${CXX:-g++}
EF="-std=gnu17 -O3 -fno-strict-aliasing -flax-vector-conversions -w"
# edge264: generic code + x86-64-v2 / v3 (AVX2) variants chosen at run time
$CC $EF -DHAS_X86_64_V2 -DHAS_X86_64_V3 -Iedge264/src -c edge264/src/edge264.c -o build/e264_base.o
$CC $EF -march=x86-64-v2 '-DADD_VARIANT(f)=f##_v2' -include edge264/src/edge264_internal.h -Iedge264/src \
    -c edge264/src/edge264_headers.c -o build/e264_v2.o
$CC $EF -march=x86-64-v3 '-DADD_VARIANT(f)=f##_v3' -include edge264/src/edge264_internal.h -Iedge264/src \
    -c edge264/src/edge264_headers.c -o build/e264_v3.o
$CXX -std=c++17 -O2 -w -DEDGE264_AVAILABLE -Idemux/include -Iedge264 \
    mvcdec.cpp demux/src/file_byte_source.cpp demux/src/h264_nal_parser.cpp demux/src/m2ts_reader.cpp \
    demux/src/ssif_parser.cpp demux/src/mvc_ssif_demuxer.cpp demux/src/mvc_decoder.cpp \
    build/e264_base.o build/e264_v2.o build/e264_v3.o -o build/vr-mvcdec -pthread
echo "OK -> build/vr-mvcdec"
