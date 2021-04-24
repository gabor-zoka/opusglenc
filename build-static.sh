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
./configure -prefix=$td --disable-shared --disable-doxygen-docs --disable-examples #--disable-thorough-tests
make check && make install
cd -



# Build Opus from Git to pick up the numerous minor changes.
#
# We need to clone rather than just download a given commit in order to have 
# package version correctly set, which calls for a functioning git. This is 
# important as each Opus file will feature this at "Encoded with"
if [[ ! -e opus ]]; then
  git clone https://github.com/xiph/opus
  cd opus
else
  cd opus
  git pull
fi

./autogen.sh
./configure -prefix=$td --disable-shared --disable-doc --disable-extra-programs
make check && make install
cd -



if [[ ! -e libopusenc ]]; then
  # This has my patch on input sample rate.
  git clone https://github.com/gabor-zoka/libopusenc
  cd libopusenc
else
  cd libopusenc
  git pull
fi

./autogen.sh
./configure -prefix=$td --disable-shared --disable-doc --disable-examples
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

rm -rf $td flac-$flac_commit
