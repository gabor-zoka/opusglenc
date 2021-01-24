gcc opusenc_example.c -lopusenc -I/usr/include/opus

flac --force-raw-format --endian little --sign signed -wfd TomsDiner.flac

./a.out TomsDiner.raw TomsDiner.opus

# The flac decoder:

gcc flac_decode_example.c -lFLAC
./a.out TomsDiner.flac TomsDiner.wav
