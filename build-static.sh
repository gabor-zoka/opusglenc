#!/usr/bin/env bash

set -e -u -o pipefail



sudo pacman --noconfirm --needed -S cmake ninja git nasm musl upx



td="$(mktemp -d -t "$(basename "$0").XXXXXX")"
script_dir="$(realpath -- "$(dirname "$0")")"

export CC=musl-gcc
export CFLAGS='  -march=x86-64 -mtune=generic -O2 -pipe -fno-plt'
export CXXFLAGS='-march=x86-64 -mtune=generic -O2 -pipe -fno-plt'
export LDFLAGS=--static
export LIBRARY_PATH=$td/lib



# We need "flac: Work around gcc bug to prevent false unset MD5 signature
# warning" bug fixed otherwise sometimes md5sum integrity check fails, which
# I use.
#
# Chose to use "Mar 15, 2021" state.
flac_commit=27c615706cedd252a206dd77e3910dfa395dcc49

if [[ ! -e flac-$flac_commit.tar.gz ]]; then
  curl -ROLJ https://github.com/xiph/flac/archive/$flac_commit.tar.gz
fi

tar xf flac-$flac_commit.tar.gz
cd     flac-$flac_commit
./autogen.sh
./configure -prefix=$td --disable-shared --disable-doxygen-docs
make check && make install
cd -



if [[ ! -e opus-1.3.1.tar.gz ]]; then
  curl -ROLJ https://downloads.xiph.org/releases/opus/opus-1.3.1.tar.gz
fi

tar xf opus-1.3.1.tar.gz
cd     opus-1.3.1
./configure -prefix=$td --disable-shared --disable-doc
make check && make install
cd -



# There were a few bugfixes I can sign off on. So use Jan 11, 2021.
opus_commit=427d61131a1af5eed48d5428e723ab4602b56cc1

if [[ ! -e libopusenc-$opus_commit.tar.gz ]]; then
  curl -ROLJ https://github.com/xiph/libopusenc/archive/$opus_commit.tar.gz
fi

tar xf libopusenc-$opus_commit.tar.gz
cd     libopusenc-$opus_commit

# I hate the input sample rate concept as a idiotic decoder might do another
# sample rate conversion back to the input rate, which would be pointless and
# degrade the sound further. Hence we apply this patch.
patch -p1 -i "$script_dir/input_sample_rate.patch"

./autogen.sh
./configure -prefix=$td --disable-shared --disable-doc
make check && make install
cd -



$CC "$script_dir/main.c" -Wall -lFLAC -lopusenc -lopus -lm -I$td/include -I$td/include/opus -static -o opusglenc

# Checking that it is a static executable indeed.
# ldd exits with error, so I need a jiggery pokery:
{ ldd /usr/bin/ffmpeg-st || true; } |& grep -q 'not a dynamic executable'

# An alternative way to check that this is a static executable. If it were
# dynamic it would print:
#
# Elf file type is DYN (Shared object file)
readelf --program-headers opusglenc | grep -q '^Elf file type is EXEC'

strip -s opusglenc
upx      opusglenc

rm -rf $td flac-$flac_commit opus-1.3.1 libopusenc-$opus_commit
