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

#include <FLAC/stream_decoder.h>
#include <opusenc.h>

#define IMIN(a,b) ((a) < (b) ? (a) : (b))   /**< Minimum int value.   */
#define IMAX(a,b) ((a) > (b) ? (a) : (b))   /**< Maximum int value.   */



static void
fatal(const char *format, ...) {
  va_list ap;
  va_start(ap, format);
  vfprintf(stderr, format, ap);
  va_end(ap);
  exit(EXIT_FAILURE);
}



void*
mymalloc(size_t size) {
  // malloc can return NULL if size == 0;
  assert(size > 0);

  void* ret = malloc(size);

  if (ret == NULL)
    fatal("ERROR: Out of memory\n");

  return ret;
}



typedef struct {
  /* input params */
  opus_int32       bitrate;
  const char*      in_path;
  const char*      out_path;

  /* internal params */
  unsigned         sample_rate;
  unsigned         channels;
  float            scale;
  unsigned         max_blocksize;
  unsigned         bits_per_sample;
  OggOpusComments* comments;
  OggOpusEnc*      enc;
  float*           enc_buffer;
  int              initialized;
} Client;



void
config_enc(OggOpusEnc* const enc, const Client* const cli) {
  assert(ope_encoder_ctl(enc, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_20_MS))        == OPE_OK &&
         ope_encoder_ctl(enc, OPE_SET_MUXING_DELAY(48000))                                 == OPE_OK &&
         ope_encoder_ctl(enc, OPE_SET_COMMENT_PADDING(8192))                               == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_VBR(1))                                             == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_VBR_CONSTRAINT(0))                                  == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC))                          == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10))                                     == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(0))                                == OPE_OK &&
         ope_encoder_ctl(enc, OPUS_SET_LSB_DEPTH(IMAX(8, IMIN(24, cli->bits_per_sample)))) == OPE_OK &&

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
         ope_encoder_ctl(enc, OPUS_SET_BITRATE(cli->bitrate))                              == OPE_OK);
}



void initialize_enc(Client* const cli) {
  assert(cli->initialized == 0);

  if (cli->channels > 2)
    fatal("ERROR: Only mono and stereo are supported\n");

  int err;
  cli->enc = ope_encoder_create_file(cli->out_path, cli->comments,
      cli->sample_rate, cli->channels, 0, &err);
  if (cli->enc == NULL || err != OPE_OK)
    fatal("ERROR: Encoding to file %s: %s\n", cli->out_path, ope_strerror(err));

  config_enc(cli->enc, cli);

  cli->enc_buffer  = mymalloc(cli->channels * cli->max_blocksize * sizeof(float));

  cli->initialized = 1;
}



FLAC__StreamDecoderWriteStatus
write_cb(const FLAC__StreamDecoder* dec,
         const FLAC__Frame*         frame,
         const FLAC__int32* const   buffer[],
         void*                      client) {
  dec;
  Client* cli = client;

  // Set up the encoder before we write the first frame
  if (frame->header.number.sample_number == 0)
    initialize_enc(cli);

  float    scale    = cli->scale;
  unsigned channels = cli->channels;
  unsigned c        = 0;

  while (c != channels) {
    float*                   o    = cli->enc_buffer + c;
    const FLAC__int32*       i    = buffer[c];
    const FLAC__int32* const iend = buffer[c] + frame->header.blocksize;

    while (i != iend) {
      *o = scale * *i;

      o += channels;
      i += 1;
    }

    ++c;
  }

  int err = ope_encoder_write_float(cli->enc, cli->enc_buffer, frame->header.blocksize);
  if (err != OPE_OK)
    fatal("ERROR: Encoding aborted: %s\n", ope_strerror(err));

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}



double
read_gain(const char* const   str,
          const regmatch_t    pmatch,
          const Client* const cli) {
  assert(pmatch.rm_so != -1);

  const regoff_t len      = pmatch.rm_eo - pmatch.rm_so; assert(len >= 0);
  char* const    gain_str = mymalloc(len + 1);
  memcpy(gain_str, str + pmatch.rm_so, len);
  gain_str[len] = '\0';

  double gain = strtod(gain_str, NULL);
  if (errno)
    err(EXIT_FAILURE, "ERROR: Parsing %s of %s\n", str, cli->in_path);

  free(gain_str);

  return gain;
}



// From https://github.com/Moonbase59/loudgain/blob/master/src/tag.cc
int gain_to_q78num(const double gain) {
  // convert float to Q7.8 number: Q = round(f * 2^8)
  return (int) round(gain * 256.0);    // 2^8 = 256
}



void
add_r128_gain_tag(OggOpusComments* const comments,
                  const char* const      key,
                  const double           gain) {
  // convert float to Q7.8 number: Q = round(f * 2^8)
  // See gain_to_q78num in 
  // https://github.com/Moonbase59/loudgain/blob/master/src/tag.cc
  const int i   = round(gain * 256.0);
  const int len = snprintf(NULL, 0, "%d", i) + 1;
  char*     str = mymalloc(len);
  snprintf(str, len, "%d", i);

  assert(ope_comments_add(comments, key, str) == OPE_OK);

  free(str);
}



void
meta_cb(const FLAC__StreamDecoder*  dec,
        const FLAC__StreamMetadata* meta,
        void*                       client) {
	dec;
  Client* cli = client;

	if (meta->type == FLAC__METADATA_TYPE_STREAMINFO) {
    cli->sample_rate     = meta->data.stream_info.sample_rate;
    cli->channels        = meta->data.stream_info.channels;
    cli->scale           = exp(-(meta->data.stream_info.bits_per_sample - 1.0) * M_LN2);
    cli->max_blocksize   = meta->data.stream_info.max_blocksize;
    cli->bits_per_sample = meta->data.stream_info.bits_per_sample;
    cli->comments        = ope_comments_create();
	}
  else if (meta->type == FLAC__METADATA_TYPE_VORBIS_COMMENT) {
    FLAC__StreamMetadata_VorbisComment_Entry* entry     = meta->data.vorbis_comment.comments;
    FLAC__StreamMetadata_VorbisComment_Entry* entry_end = meta->data.vorbis_comment.comments +
      meta->data.vorbis_comment.num_comments;

    regex_t replaygain_re, album_gain_re, track_gain_re;
    assert(regcomp(&replaygain_re, "^REPLAYGAIN_",                    REG_ICASE) == 0);
    assert(regcomp(&album_gain_re, "^REPLAYGAIN_ALBUM_GAIN=\\(.*\\)", REG_ICASE) == 0);
    assert(regcomp(&track_gain_re, "^REPLAYGAIN_TRACK_GAIN=\\(.*\\)", REG_ICASE) == 0);

    regmatch_t pmatch[2];

    double album_gain = NAN;
    double track_gain = NAN;

    while (entry != entry_end) {
      const char* const comment = entry->entry;

      if (regexec(&replaygain_re, comment, 2, pmatch, 0)) {
        // Not REPLAYGAIN_*
        ope_comments_add_string(cli->comments, comment);
      }
      else if (!regexec(&album_gain_re, comment, 2, pmatch, 0)) {
        album_gain = read_gain(comment, pmatch[1], cli);
      }
      else if (!regexec(&track_gain_re, comment, 2, pmatch, 0)) {
        track_gain = read_gain(comment, pmatch[1], cli);
      }

      ++entry;
    }

    if (!isnan(album_gain) && album_gain < 20.0) {
      // album_gain uses -18LUFS, but Opus (and me) wants to use -23LUFS as 
      // target loudness.
      cli->scale *= exp((album_gain - 5.0) / 20.0 * M_LN10);

      if (!isnan(track_gain))
        add_r128_gain_tag(cli->comments, "R128_TRACK_GAIN", track_gain - album_gain);
    }
    else if (!isnan(track_gain))
      add_r128_gain_tag(cli->comments, "R128_TRACK_GAIN", track_gain);
  }
}



void
error_cb(const FLAC__StreamDecoder*     dec,
         FLAC__StreamDecoderErrorStatus status,
         void*                          client) {
	dec, client;

	fprintf(stderr, "ERROR: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
}



int main(int argc, char *argv[])
{
	if(argc != 3) {
		fprintf(stderr, "USAGE: %s infile.flac outfile.opus\n", argv[0]);
		return 1;
	}

	FLAC__StreamDecoder* dec = FLAC__stream_decoder_new();
	if(dec == NULL) {
		fprintf(stderr, "ERROR: Allocating decoder\n");
		return 1;
	}

  FLAC__stream_decoder_set_md5_checking(dec, true);
  FLAC__stream_decoder_set_metadata_respond(dec, FLAC__METADATA_TYPE_VORBIS_COMMENT);

  Client client;
  client.bitrate     = 192000;
  client.in_path     = argv[1];
  client.out_path    = argv[2];
  client.initialized = 0;

  FLAC__StreamDecoderInitStatus	init_status =
  FLAC__stream_decoder_init_file(dec, argv[1], write_cb, meta_cb, error_cb, &client);
	if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		fprintf(stderr, "ERROR: initializing decoder: %s\n", FLAC__StreamDecoderInitStatusString[init_status]);
		return 1;
	}

	FLAC__bool ok = FLAC__stream_decoder_process_until_end_of_stream(dec);
  // If the FLAC file is empty, the write_cb() is never called.
  if (!client.initialized)
    initialize_enc(&client);

	FLAC__stream_decoder_delete(dec);
  ope_encoder_drain(client.enc);
  ope_encoder_destroy(client.enc);
  ope_comments_destroy(client.comments);
  free(client.enc_buffer);

	return !ok;
}
