#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h> /* PRIu64 */

#include <FLAC/stream_decoder.h>
#include <opusenc.h>



struct Client {
  /* input params */
  const char*      out_path;

  /* internal params */
  unsigned         sample_rate;
  unsigned         channels;
  unsigned         max_blocksize;
  OggOpusComments* comments;
  OggOpusEnc*      enc;
  float*           enc_buffer;
};



FLAC__StreamDecoderWriteStatus
write_cb(const FLAC__StreamDecoder* dec,
         const FLAC__Frame*         frame,
         const FLAC__int32* const   buffer[],
         void*                      client) {
  dec;
  struct Client* cli = (struct Client*)client;

	/* Set up the encoder before we write the first frame */
	if(frame->header.number.sample_number == 0) {
	  if (cli->channels > 2) {
		  fprintf(stderr, "ERROR: Only mono and stereo are supported.\n");
		  return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
	  }

    int error;
    cli->enc = ope_encoder_create_file(cli->out_path, cli->comments,
        cli->sample_rate, cli->channels, 0, &error);

    if (!cli->enc) {
      fprintf(stderr, "ERROR: Encoding to file %s: %s\n", cli->out_path, ope_strerror(error));
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    cli->enc_buffer = malloc(cli->channels * cli->max_blocksize * sizeof(float));
    if (!cli->enc_buffer) {
      fprintf(stderr, "ERROR: Memory allocation");
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  }

  unsigned channels = cli->channels;
  for(unsigned c = 0; c != channels; ++c) {
    float* o = cli->enc_buffer + c;
    const FLAC__int32* const iend = buffer[c] + frame->header.blocksize;
    for(const FLAC__int32* i = buffer[c]; i != iend; ++i) {
      *o = (float)*i / INT32_MAX;
      o += channels;
    }
  }

  ope_encoder_write_float(cli->enc, cli->enc_buffer, frame->header.blocksize);

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}



void
meta_cb(const FLAC__StreamDecoder*  dec,
        const FLAC__StreamMetadata* meta,
        void*                       client) {
	(void)dec;
  struct Client* cli = (struct Client*)client;

	if(meta->type == FLAC__METADATA_TYPE_STREAMINFO) {
    cli->sample_rate   = meta->data.stream_info.sample_rate;
    cli->channels      = meta->data.stream_info.channels;
    cli->max_blocksize = meta->data.stream_info.max_blocksize;
    cli->comments      = ope_comments_create();
	}
}



void
error_cb(const FLAC__StreamDecoder*     dec,
         FLAC__StreamDecoderErrorStatus status,
         void*                          client) {
	(void)dec, (void)client;

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

  struct Client client;
  FLAC__StreamDecoderInitStatus	init_status =
  FLAC__stream_decoder_init_file(dec, argv[1], write_cb, meta_cb, error_cb, &client);
	if(init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		fprintf(stderr, "ERROR: initializing decoder: %s\n", FLAC__StreamDecoderInitStatusString[init_status]);
		return 1;
	}

	FLAC__bool ok = FLAC__stream_decoder_process_until_end_of_stream(dec);

	FLAC__stream_decoder_delete(dec);
  ope_encoder_drain(client.enc);
  ope_encoder_destroy(client.enc);
  ope_comments_destroy(client.comments);
  free(client.enc_buffer);

	return !ok;
}
