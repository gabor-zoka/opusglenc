Build Instruction:

    gcc main.c -Wall -lFLAC -lopusenc -lm -I/usr/include/opus -o opusglenc

Documentation:

    USAGE: opusglenc [-h] [-w] [-b bitrate] output-dir input-dir
    
    Encodes all *.fla or *.flac FLAC files from input-dir into OPUS format.
    The output goes into output-dir with same filename with *.opus extension.
    The volume is scaled to -23 LUFS with REPLAYGAIN_ALBUM_GAIN if exists.
    It uses GAPLESS encoding between tracks if scaling does not change.
    
      -h   This help.
      -w   Fail even on warnings.
      -b   Bitrate in bits/sec. Must be integer (default 160000).
      -i   Each track independently encoded (i.e. not gapless).
           It is scaled to -23 LUFS with REPLAYGAIN_TRACK_GAIN if exists.
