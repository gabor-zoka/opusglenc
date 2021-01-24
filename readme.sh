gcc opusenc_example.c -lopusenc -I/usr/include/opus

flac --force-raw-format --endian little --sign signed -wfd TomsDiner.flac

./a.out TomsDiner.raw TomsDiner.opus
