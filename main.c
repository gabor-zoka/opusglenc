#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h> /* PRIu64 */
#include <math.h>
#include <regex.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <locale.h>
#include <unistd.h>
#include <libgen.h>

#include <FLAC/metadata.h>
#include <FLAC/stream_decoder.h>
#include <opusenc.h>

#define IMIN(a,b) ((a) < (b) ? (a) : (b))   /**< Minimum int value.   */
#define IMAX(a,b) ((a) > (b) ? (a) : (b))   /**< Maximum int value.   */



typedef struct {
  unsigned         max_blocksize;
  unsigned         sample_rate;
  unsigned         channels;
  unsigned         bits_per_sample;
  FLAC__uint64     total_samples;
  OggOpusComments* comments;
  OggOpusEnc*      enc;
  opus_int32       bitrate;
  int              individual;
  char**           inp_paths;
  char**           out_paths;
  size_t           num_paths;
  float*           enc_buffer;

  unsigned         initialized;
  size_t           idx;
  double           scale;
  double           prev_scale;
} Data;



int exit_warning = 0;



void
warning(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  vfprintf(stderr, format, ap);
  va_end(ap);

  if (exit_warning)
    exit(EXIT_FAILURE);
}



void
fatal(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  vfprintf(stderr, format, ap);
  va_end(ap);
  exit(EXIT_FAILURE);
}



void*
my_malloc(size_t size) {
  void* ret = malloc(size);

  if (size != 0 && ret == NULL)
    fatal("ERROR: Out of memory\n");

  return ret;
}



char*
my_sprintf(const char* format, ...) {
  va_list  ap1, ap2;
  va_start(ap1, format);
  va_copy (ap2, ap1);

  const int len = vsnprintf(NULL, 0, format, ap1) + 1;
  va_end(ap1);

  char* buf = my_malloc(len);

  vsnprintf(buf, len, format, ap2);
  va_end(ap2);

  return buf;
}



void
config_enc(OggOpusEnc* const enc, const Data* const d) {
  assert(ope_encoder_ctl(enc, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_20_MS))      == OPE_OK &&
         ope_encoder_ctl(enc, OPE_SET_MUXING_DELAY(48000))                               == OPE_OK &&
         ope_encoder_ctl(enc, OPE_SET_COMMENT_PADDING(8192))                             == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_VBR(1))                                           == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(0))                                == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC))                        == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10))                                   == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(0))                              == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_LSB_DEPTH(IMAX(8, IMIN(24, d->bits_per_sample)))) == OPE_OK);

  // We cannot fail on bitrate if it is positive:
  //
  // case OPUS_SET_BITRATE_REQUEST:
  // {
  //    opus_int32 value = va_arg(ap, opus_int32);
  //    if (value != OPUS_AUTO && value != OPUS_BITRATE_MAX)
  //    {
  //       if (value <= 0)
  //          goto bad_arg;
  //       value = IMIN(300000*st->layout.nb_channels, IMAX(500*st->layout.nb_channels, value));
  //    }
  //    st->bitrate_bps = value;
  // }
  int err = ope_encoder_ctl(enc, OPUS_SET_BITRATE(d->bitrate));
  if (err != OPE_OK)
    fatal("ERROR: Invalid bitrate: %s\n", ope_strerror(err));
}



void initialize_enc(Data* const d) {
  assert(d->initialized == 0);
  assert(d->scale != 0.0);

  int err;

  // Start a new encoding (non-gapless) when 
  // - -i option is set
  // - 1st track
  // - When the scaling changes (bitdepth or album gain changed).
  if (d->individual || d->idx == 0 ||
      fabs(d->scale - d->prev_scale) / fabs(d->scale) > 0.01) {

    if (d->idx != 0) {
      ope_encoder_drain(d->enc);
      ope_encoder_destroy(d->enc);
    }

    if (d->channels > 2)
      fatal("ERROR: Only mono and stereo are supported\n");

    d->enc = ope_encoder_create_file(d->out_paths[d->idx], d->comments,
        d->sample_rate, d->channels, 0, &err);
    if (d->enc == NULL || err != OPE_OK)
      fatal("ERROR: %s: %s while initializing encoder\n", d->out_paths[d->idx], ope_strerror(err));

    config_enc(d->enc, d);
  }
  else {
    err = ope_encoder_continue_new_file(d->enc, d->out_paths[d->idx], d->comments);
    if (err != OPE_OK)
      fatal("ERROR: %s: %s while encoding\n", d->out_paths[d->idx], ope_strerror(err));
  }

  d->initialized = 1;
}



double
read_gain(const char* const str,
          const regmatch_t  pmatch,
          const Data* const d) {
  assert(pmatch.rm_so != -1);

  const regoff_t len      = pmatch.rm_eo - pmatch.rm_so; assert(len >= 0);
  char* const    gain_str = my_malloc(len + 1);
  memcpy(gain_str, str + pmatch.rm_so, len);
  gain_str[len] = '\0';

  char* endp;
  double gain = strtod(gain_str, &endp);
  if (gain_str == endp)
    fatal("ERROR: %s: Parsing %s\n", d->inp_paths[d->idx], str);

  if (isnan(gain))
    fatal("ERROR: %s: %s\n", d->inp_paths[d->idx], str);

  free(gain_str);

  return gain;
}



// From https://github.com/Moonbase59/loudgain/blob/master/src/tag.cc
int gain_to_q78num(const double gain) {
  // convert float to Q7.8 number: Q = round(f * 2^8)
  return (int) round(gain * 256.0);    // 2^8 = 256
}



FLAC__StreamDecoderWriteStatus
write_cb(const FLAC__StreamDecoder* dec,
         const FLAC__Frame*         frame,
         const FLAC__int32* const   buffer[],
         void*                      data) {
  Data* d = data;

  // Set up the encoder before we write the first frame
  if (frame->header.number.sample_number == 0)
    initialize_enc(d);

  double   scale    = d->scale;
  unsigned channels = d->channels;
  unsigned c        = 0;

  while (c != channels) {
    float*                   o    = d->enc_buffer + c;
    const FLAC__int32*       i    = buffer[c];
    const FLAC__int32* const iend = buffer[c] + frame->header.blocksize;

    while (i != iend) {
      *o = scale * *i;

      o += channels;
      i += 1;
    }

    ++c;
  }

  int err = ope_encoder_write_float(d->enc, d->enc_buffer, frame->header.blocksize);
  if (err != OPE_OK)
    fatal("ERROR: %s: %s\n", d->out_paths[d->idx], ope_strerror(err));

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}



void
meta_cb(const FLAC__StreamDecoder*  dec,
        const FLAC__StreamMetadata* meta,
        void*                       data) {
  Data* d = data;

	if (meta->type == FLAC__METADATA_TYPE_STREAMINFO) {
    d->scale    = exp(-(meta->data.stream_info.bits_per_sample - 1.0) * M_LN2);
	}
  else if (meta->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
    FLAC__StreamMetadata_VorbisComment_Entry* entry     = meta->data.vorbis_comment.comments;
    FLAC__StreamMetadata_VorbisComment_Entry* entry_end = meta->data.vorbis_comment.comments +
      meta->data.vorbis_comment.num_comments;

    regex_t replaygain_re, album_gain_re, track_gain_re;
    assert(regcomp(&replaygain_re, "^REPLAYGAIN_",                REG_EXTENDED|REG_ICASE) == 0);
    assert(regcomp(&album_gain_re, "^REPLAYGAIN_ALBUM_GAIN=(.*)", REG_EXTENDED|REG_ICASE) == 0);
    assert(regcomp(&track_gain_re, "^REPLAYGAIN_TRACK_GAIN=(.*)", REG_EXTENDED|REG_ICASE) == 0);

    regmatch_t pmatch[2];

    double album_gain = NAN;
    double track_gain = NAN;

    while (entry != entry_end) {
      const char* const comment = (const char*)entry->entry;

      if (regexec(&replaygain_re, comment, 0, NULL, 0)) {
        // Not REPLAYGAIN_*
        ope_comments_add_string(d->comments, comment);
      }
      else {
        if (!regexec(&album_gain_re, comment, 2, pmatch, 0))
          album_gain = read_gain(comment, pmatch[1], d);
      
        if (!regexec(&track_gain_re, comment, 2, pmatch, 0))
          track_gain = read_gain(comment, pmatch[1], d);
      }

      ++entry;
    }

    const double limit = 30.0;

    if (d->individual) {
      if (!isnan(track_gain)) {
        if (track_gain < limit) {
          // track_gain uses -18LUFS, but Opus (and me) wants to use -23 LUFS as 
          // target loudness.
          d->scale *= exp((track_gain - 5.0) / 20.0 * M_LN10);
        }
        else
          warning("WARNING: %s: REPLAYGAIN_TRACK_GAIN >= %.1f hence not applied\n",
              d->out_paths[d->idx], limit);
      }
    }
    else {
      if (!isnan(album_gain)) {
        if (album_gain < limit) {
          // album_gain uses -18LUFS, but Opus (and me) wants to use -23 LUFS as 
          // target loudness.
          d->scale *= exp((album_gain - 5.0) / 20.0 * M_LN10);
        }
        else
          warning("WARNING: %s: REPLAYGAIN_ALBUM_GAIN >= %.1f hence not applied\n",
              d->out_paths[d->idx], limit);
      }
    }
  }
}



void
error_cb(const FLAC__StreamDecoder*     dec,
         FLAC__StreamDecoderErrorStatus status,
         void*                          data) {
  Data* d = (Data*)data;

	fprintf(stderr, "ERROR: %s: %s\n", d->inp_paths[d->idx],
      FLAC__StreamDecoderErrorStatusString[status]);
}



Data*
ls_flac(char* const out_dir, char* const inp_dir) {
  // Check the out_dir

  struct stat st;
  // Stat follows symbolic links.
  if (stat(out_dir, &st))
    err(EXIT_FAILURE, "ERROR: %s", out_dir);

  if (!S_ISDIR(st.st_mode))
    fatal("ERROR: %s: Not a directory\n", out_dir);

  // Directory does not have to be readable. That is only needed for listing 
  // the dir. We do not need to list out_dir.
  if (access(out_dir, W_OK)) {
    if (errno == EACCES)
      fatal("ERROR: %s: Not writable\n", out_dir);
    else
      err(EXIT_FAILURE, "ERROR: %s", out_dir);
  }
  if (access(out_dir, X_OK)) {
    if (errno == EACCES)
      fatal("ERROR: %s: Not executable\n", out_dir);
    else
      err(EXIT_FAILURE, "ERROR: %s", out_dir);
  }

  // Traverse the contents of inp_dir. It is ordered as per current locale.

  struct dirent **list = NULL;
  // Scandir follows symbolic links.
  int size = scandir(inp_dir, &list, NULL, alphasort);
  if (size == -1)
    err(EXIT_FAILURE, "ERROR: %s", inp_dir);

  // Trim tailing slashes on input dirs.

  regex_t slash_re;
  assert(regcomp(&slash_re, "/+$", REG_EXTENDED|REG_ICASE) == 0);
  regmatch_t pmatch[1];

  if(!regexec(&slash_re, inp_dir, 1, pmatch, 0))
    inp_dir[pmatch[0].rm_so] = '\0';
  if(!regexec(&slash_re, out_dir, 1, pmatch, 0))
    out_dir[pmatch[0].rm_so] = '\0';

  regex_t flac_re;
  assert(regcomp(&flac_re, "\\.flac?$", REG_EXTENDED|REG_ICASE) == 0);

  FLAC__StreamMetadata m;
  Data*                d = NULL;

  for (int i = 0; i != size; ++i) {
    if (!regexec(&flac_re, list[i]->d_name, 1, pmatch, 0)) {
      // Matches ".flac?$"

      char* inp_path = my_sprintf("%s/%s", inp_dir, list[i]->d_name);
      int   skip     = 0;

      // Stat follows symbolic links.
      if (stat(inp_path, &st))
        err(EXIT_FAILURE, "ERROR: %s", inp_path);

      if (!S_ISREG(st.st_mode)) {
        warning("WARNING: Skipping %s: Not regular file\n", inp_path);
        skip = 1;
      }
      // Access follows symbolic links.
      else if (access(inp_path, R_OK)) {
        if (errno == EACCES)
          warning("WARNING: Skipping %s: Not readable\n", inp_path);
        else
          err(EXIT_FAILURE, "ERROR: %s", inp_path);

        skip = 1;
      }
      else if (!FLAC__metadata_get_streaminfo(inp_path, &m)) {
        warning("WARNING: Skipping %s: Not a FLAC file\n", inp_path);
        skip = 1;
      }

      if(skip) {
        free(inp_path);
      }
      else {
        // This is a FLAC file.

        // Generate the output path.
        list[i]->d_name[pmatch[0].rm_so] = '\0';
        char* out_path = my_sprintf("%s/%s.opus", out_dir, list[i]->d_name);

        if (access(out_path, F_OK) == 0)
          fatal("ERROR: %s: Path exists\n", out_path);

        if (d == NULL) {
          // This is the 1st FLAC file. Initialize Data.
          d                  = my_malloc(sizeof(Data));
          d->max_blocksize   = m.data.stream_info.max_blocksize;
          d->sample_rate     = m.data.stream_info.sample_rate;
          d->channels        = m.data.stream_info.channels;
          d->bits_per_sample = m.data.stream_info.bits_per_sample;
          d->total_samples   = m.data.stream_info.total_samples;
          d->comments        = NULL;
          d->enc             = NULL;
          d->bitrate         = OPUS_AUTO;
          d->individual      = 0;
          d->prev_scale      = 0.0;
          d->inp_paths       = my_malloc(sizeof(char*) * size);
          d->out_paths       = my_malloc(sizeof(char*) * size);
          d->inp_paths[0]    = inp_path;
          d->out_paths[0]    = out_path;
          d->num_paths       = 1;
        }
        else {
          if (d->max_blocksize < m.data.stream_info.max_blocksize)
            d->max_blocksize = m.data.stream_info.max_blocksize;

          if (d->sample_rate != m.data.stream_info.sample_rate)
            fatal("ERROR: Sample rate differs between %s and %s\n",     inp_path, d->inp_paths[0]);

          if (d->channels    != m.data.stream_info.channels)
            fatal("ERROR: Num of channels differs between %s and %s\n", inp_path, d->inp_paths[0]);

          if (d->bits_per_sample < m.data.stream_info.bits_per_sample)
            d->bits_per_sample = m.data.stream_info.bits_per_sample;

          d->total_samples += m.data.stream_info.total_samples;

          d->inp_paths[d->num_paths] = inp_path;
          d->out_paths[d->num_paths] = out_path;
          ++d->num_paths;
        }
      }
    }
    free(list[i]);
  }
  free(list);

  if (d == NULL)
    fatal("ERROR: %s: No FLAC files found\n", inp_dir);

  d->enc_buffer = my_malloc(d->channels * d->max_blocksize * sizeof(float));

  return d;
}



void
usage(const char* const prg) {
  fprintf(stderr, "USAGE: %s [-h] [-w] [-b bitrate] output-dir input-dir\n\n", prg);
  fprintf(stderr, "Encodes all *.fla or *.flac FLAC files from input-dir into OPUS format.\n");
  fprintf(stderr, "The output goes into output-dir with same filename with *.opus extension.\n");
  fprintf(stderr, "The tracks are assumed to form an album. The conversion uses the GAPLESS\n");
  fprintf(stderr, "encoding provided by libopusenc.\n");
  fprintf(stderr, "The volume is scaled to -23 LUFS with REPLAYGAIN_ALBUM_GAIN if exists.\n\n");
  fprintf(stderr, "  -h   This help.\n");
  fprintf(stderr, "  -w   Fail even on warnings.\n");
  fprintf(stderr, "  -b   Bitrate in bsp. Must be integer (default 160000).\n");
  fprintf(stderr, "  -i   Each track independently encoded (i.e. not gapless.\n");
  fprintf(stderr, "       Scaled to -23 LUFS with REPLAYGAIN_TRACK_GAIN\n");
}



int main(int argc, char *argv[]) {
  // To make this program locale-aware.
  setlocale(LC_ALL, "");

  char* prg = basename(argv[0]);

  opus_int32 bitrate    = 160000;
  int        individual = 0;
  int        c;
  char*      endp;
  while ((c = getopt (argc, argv, "hwb:i")) != -1)
    switch (c) {
      case 'h':
        usage(prg);
        return EXIT_SUCCESS;
        break;

      case 'w':
        exit_warning = 1;
        break;

      case 'b':
        bitrate = (opus_int32)strtoimax(optarg, &endp, 10);
        if (optarg == endp || endp < optarg + strlen(optarg))
          fatal("ERROR: Parsing bitrate = %s\n", optarg);
        break;

      case 'i':
        individual = 1;
        break;

      case '?':
        // Parameter errors. getopt() already prints out an error.
        usage(prg);
        return EXIT_FAILURE;
        break;

      default:
        abort();
    }
  
  if (argc - optind != 2) {
	  if (argc - optind == 0)
      fprintf(stderr, "ERROR: Missing input and output directories\n");
    else if (argc - optind == 1)
      fprintf(stderr, "ERROR: Missing output directory\n");
    else if (argc - optind > 2)
      fprintf(stderr, "ERROR: Too many parameters\n");

    usage(prg);
    return EXIT_FAILURE;
  }

  Data* d = ls_flac(argv[optind], argv[optind + 1]);

  d->bitrate    = bitrate;
  d->individual = individual;

  for (int i = 0; i != d->num_paths; ++i) {
    d->initialized = 0;
    d->idx         = i;

    d->comments = ope_comments_create();
	  FLAC__StreamDecoder* dec = FLAC__stream_decoder_new();
    assert(dec != NULL);

    FLAC__stream_decoder_set_md5_checking(dec, true);
    FLAC__stream_decoder_set_metadata_respond(dec, FLAC__METADATA_TYPE_VORBIS_COMMENT);

    FLAC__StreamDecoderInitStatus	init_status =
      FLAC__stream_decoder_init_file(dec, d->inp_paths[i], write_cb, meta_cb, error_cb, d);
	  if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
		  fatal("ERROR: %s: %s\n", d->inp_paths[i],
          FLAC__StreamDecoderInitStatusString[init_status]);

	  if (!FLAC__stream_decoder_process_until_end_of_stream(dec))
      fatal("ERROR: %s: %s\n", d->inp_paths[i],
          FLAC__StreamDecoderStateString[FLAC__stream_decoder_get_state(dec)]);

    // If the FLAC file is empty, the write_cb() has not been called so 
    // initialize_enc() has not been executed.
    if (!d->initialized)
      initialize_enc(d);

    d->prev_scale = d->scale;

    FLAC__stream_decoder_delete(dec);
    ope_comments_destroy(d->comments);

    free(d->inp_paths[i]);
    free(d->out_paths[i]);
  }

  ope_encoder_drain(d->enc);
  ope_encoder_destroy(d->enc);

  free(d->inp_paths);
  free(d->out_paths);
  free(d->enc_buffer);
  free(d);

	return EXIT_SUCCESS;
}
