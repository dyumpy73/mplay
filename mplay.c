#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <FLAC/stream_decoder.h>
#include <alsa/asoundlib.h>

#define POS_BAR_SEGMENTS 20
#define PCM_LATENCY_US   250000

enum {
  ACT_PREV = -1,
  ACT_NONE = 0,
  ACT_NEXT = 1,
  ACT_QUIT = 2,
  ACT_SEEK = 3,
  ACT_ERROR = 4
};

static snd_pcm_t *pcm_handle = NULL;
static unsigned pcm_channels = 0;
static unsigned pcm_rate = 0;
static snd_pcm_format_t pcm_format = SND_PCM_FORMAT_UNKNOWN;
static int pcm_can_pause = 0;
static unsigned pcm_bytes_per_sample = 0;
static unsigned pcm_shift = 0;
static size_t pcm_frame_bytes = 0;
static uint8_t *pcm_buf = NULL;
static size_t pcm_buf_size = 0;

static uint64_t total_samples_global = 0;
static uint64_t current_samples = 0;
static uint64_t seek_target_global = 0;
static uint32_t sample_rate_global = 44100;
static uint32_t fixed_blocksize_global = 0;
static volatile int user_action = ACT_NONE;
static volatile int is_paused = 0;
static volatile sig_atomic_t got_signal = 0;

static struct termios orig_termios;
static int raw_enabled = 0;

/* Restore the terminal to normal (cooked) mode. */
static void disable_raw_mode(void) {
  if (raw_enabled) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    raw_enabled = 0;
  }
}

/* Set the terminal to raw mode so keys are read byte-by-byte without Enter. */
static void enable_raw_mode(void) {
  if (!isatty(STDIN_FILENO)) return;
  if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) return;
  atexit(disable_raw_mode);

  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) raw_enabled = 1;
}

/* Signal handler: set a flag so the main loop can exit cleanly. */
static void signal_handler(int sig) {
  (void)sig;
  got_signal = 1;
}

/* Register handlers for SIGINT and SIGTERM without SA_RESTART. */
static void setup_signals(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

/* Samples of the current track actually heard (written minus ALSA delay). */
static uint64_t played_samples(void) {
  if (!pcm_handle) return current_samples;
  snd_pcm_sframes_t delay = 0;
  if (snd_pcm_delay(pcm_handle, &delay) < 0 || delay < 0) return current_samples;
  if ((uint64_t)delay >= current_samples) return 0;
  return current_samples - (uint64_t)delay;
}

/* Render the progress line. force_done shows the final (100%) state. */
static void print_progress_ex(int force_done) {
  if (total_samples_global == 0 || sample_rate_global == 0) return;

  uint64_t pos = force_done ? total_samples_global : played_samples();
  if (pos > total_samples_global) pos = total_samples_global;

  unsigned total_sec = (unsigned)(total_samples_global / sample_rate_global);
  unsigned current_sec = (unsigned)(pos / sample_rate_global);
  if (current_sec > total_sec) current_sec = total_sec;

  int pos_filled;
  if (force_done) {
    pos_filled = POS_BAR_SEGMENTS;
    current_sec = total_sec;
  } else {
    pos_filled = (int)((pos * (uint64_t)POS_BAR_SEGMENTS) / total_samples_global);
    if (pos_filled > POS_BAR_SEGMENTS) pos_filled = POS_BAR_SEGMENTS;
    if (pos + (uint64_t)(sample_rate_global / 50) >= total_samples_global) {
      pos_filled = POS_BAR_SEGMENTS;
      current_sec = total_sec;
    }
  }

  char pos_bar[POS_BAR_SEGMENTS + 3];
  int pidx = 0;
  pos_bar[pidx++] = '[';
  for (int i = 0; i < POS_BAR_SEGMENTS; i++) {
    pos_bar[pidx++] = (i < pos_filled) ? '#' : '-';
  }
  pos_bar[pidx++] = ']';
  pos_bar[pidx] = '\0';

  printf("\r\033[K[mplay] %s %s [%02u:%02u / %02u:%02u]",
         is_paused ? "[PAUSED]" : "[PLAYING]", pos_bar,
         current_sec / 60, current_sec % 60,
         total_sec / 60, total_sec % 60);
  fflush(stdout);
}

static void print_progress(void) {
  print_progress_ex(0);
}

/* Pause or resume ALSA; fall back to drop/prepare if the device cannot pause. */
static void pcm_set_pause(int pause) {
  if (!pcm_handle) return;
  if (pcm_can_pause && snd_pcm_pause(pcm_handle, pause) == 0) return;

  if (pause) {
    snd_pcm_drop(pcm_handle);
  } else {
    snd_pcm_prepare(pcm_handle);
  }
}

/* Discard queued audio immediately but keep the device open and ready. */
static void pcm_flush_now(void) {
  if (!pcm_handle) return;
  snd_pcm_drop(pcm_handle);
  snd_pcm_prepare(pcm_handle);
}

static int handle_keyboard_input(void);

/*
 * Let the queued audio finish while staying responsive to q / n / p / Ctrl+C.
 * Returns the action that interrupted the wait (ACT_NONE if it finished normally).
 */
static int pcm_drain_interactive(void) {
  struct timeval t0, tn;
  gettimeofday(&t0, NULL);

  int interrupted = 0;
  int result = ACT_NONE;

  for (;;) {
    if (got_signal) {
      interrupted = 1;
      result = ACT_QUIT;
      break;
    }

    int action = handle_keyboard_input();
    if (is_paused) {
      is_paused = 0;
      pcm_set_pause(0);
    }
    if (action == ACT_QUIT || action == ACT_NEXT || action == ACT_PREV) {
      interrupted = 1;
      result = action;
      break;
    }

    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(pcm_handle, &delay) < 0 || delay <= 0) break;

    gettimeofday(&tn, NULL);
    double elapsed = (tn.tv_sec - t0.tv_sec) + (tn.tv_usec - t0.tv_usec) / 1e6;
    if (elapsed > 1.0) break;

    usleep(20000);
  }

  if (interrupted) snd_pcm_drop(pcm_handle);
  else snd_pcm_drain(pcm_handle);
  return result;
}

/* Close the device. If wait_for_audio is set, let queued audio finish first. */
static int pcm_close(int wait_for_audio) {
  int result = ACT_NONE;
  if (!pcm_handle) return result;
  if (wait_for_audio) result = pcm_drain_interactive();
  else snd_pcm_drop(pcm_handle);
  snd_pcm_close(pcm_handle);
  pcm_handle = NULL;
  return result;
}

/*
 * Make sure the device is open with the given parameters.
 * Same parameters as before -> reuse the open device (gapless).
 * Different parameters -> finish the old audio, then reopen.
 */
static int pcm_ensure(unsigned channels, unsigned rate, snd_pcm_format_t format) {
  if (pcm_handle && channels == pcm_channels && rate == pcm_rate && format == pcm_format) {
    return 0;
  }

  if (pcm_close(!got_signal) == ACT_QUIT) user_action = ACT_QUIT;

  snd_pcm_t *h = NULL;
  if (snd_pcm_open(&h, "pulse", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
    h = NULL;
    if (snd_pcm_open(&h, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
      fprintf(stderr, "\n[mplay] Failed to open audio device.\n");
      return -1;
    }
  }

  int err = snd_pcm_set_params(h, format, SND_PCM_ACCESS_RW_INTERLEAVED,
                               channels, rate, 1, PCM_LATENCY_US);
  if (err < 0) {
    fprintf(stderr, "\n[mplay] snd_pcm_set_params failed: %s\n", snd_strerror(err));
    snd_pcm_close(h);
    return -1;
  }

  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  pcm_can_pause = (snd_pcm_hw_params_current(h, hw) == 0) && snd_pcm_hw_params_can_pause(hw);

  pcm_handle = h;
  pcm_channels = channels;
  pcm_rate = rate;
  pcm_format = format;
  return 0;
}

/* Write all frames to ALSA, recovering from underruns. */
static int pcm_write_all(const uint8_t *p, snd_pcm_uframes_t frames) {
  while (frames > 0) {
    if (got_signal) return -1;

    snd_pcm_sframes_t n = snd_pcm_writei(pcm_handle, p, frames);
    if (n < 0) {
      if (snd_pcm_recover(pcm_handle, (int)n, 1) < 0) return -1;
      continue;
    }
    p += (size_t)n * pcm_frame_bytes;
    frames -= (snd_pcm_uframes_t)n;
  }
  return 0;
}

/* Read a single byte from stdin with a timeout (ms); returns 1 on success. */
static int read_byte(unsigned char *c, int timeout_ms) {
  fd_set fds;
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  FD_ZERO(&fds);
  FD_SET(STDIN_FILENO, &fds);
  if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) return 0;
  return read(STDIN_FILENO, c, 1) == 1;
}

/* Prepare a +/-5 second seek from the current position. */
static int request_seek(int direction) {
  uint64_t step = (uint64_t)sample_rate_global * 5;
  uint64_t base = played_samples();
  uint64_t target;

  if (direction > 0) {
    target = base + step;
    if (total_samples_global > 0 && target >= total_samples_global) return ACT_NONE;
  } else {
    target = (base > step) ? (base - step) : 0;
  }

  seek_target_global = target;
  current_samples = target;
  return ACT_SEEK;
}

/* Read and process one keypress (including arrow keys); returns an action. */
static int handle_keyboard_input(void) {
  unsigned char c;
  if (!read_byte(&c, 0)) return ACT_NONE;

  if (c == ' ') {
    is_paused = !is_paused;
    pcm_set_pause(is_paused);
    print_progress();
    return ACT_NONE;
  }

  if (c == '\033') {
    unsigned char s0, s1;
    if (read_byte(&s0, 20) && read_byte(&s1, 20) && s0 == '[') {
      if (s1 == 'C') c = 'd';
      else if (s1 == 'D') c = 'a';
    }
  }

  switch (c) {
    case 'd': return request_seek(+1);
    case 'a': return request_seek(-1);
    case 'n': case 'N': return ACT_NEXT;
    case 'p': case 'P': return ACT_PREV;
    case 'q': case 'Q': return ACT_QUIT;
    default: return ACT_NONE;
  }
}

static int is_media_file(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (!dot) return 0;
  return (strcasecmp(dot, ".flac") == 0);
}

static int compare_strings(const void *a, const void *b) {
  return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static void add_to_playlist(char ***playlist, int *count, const char *filepath) {
  char *copy = strdup(filepath);
  if (!copy) return;

  char **new_playlist = realloc(*playlist, sizeof(char *) * (*count + 1));
  if (!new_playlist) {
    free(copy);
    return;
  }
  *playlist = new_playlist;
  (*playlist)[*count] = copy;
  (*count)++;
}

/* Scan a directory for .flac files; sort only the files found here. */
static void scan_directory(const char *dirpath, char ***playlist, int *count) {
  DIR *d = opendir(dirpath);
  if (!d) return;

  int start = *count;
  struct dirent *dir;
  char fullpath[PATH_MAX];

  while ((dir = readdir(d)) != NULL) {
    if (dir->d_name[0] == '.') continue;
    if (is_media_file(dir->d_name)) {
      int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, dir->d_name);
      if (n < 0 || (size_t)n >= sizeof(fullpath)) continue;
      add_to_playlist(playlist, count, fullpath);
    }
  }
  closedir(d);

  if (*count - start > 1) {
    qsort(*playlist + start, *count - start, sizeof(char *), compare_strings);
  }
}

static void build_playlist(int argc, char *argv[], char ***playlist, int *count) {
  *playlist = NULL;
  *count = 0;

  for (int i = 1; i < argc; i++) {
    struct stat st;
    if (stat(argv[i], &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        scan_directory(argv[i], playlist, count);
      } else if (S_ISREG(st.st_mode) && is_media_file(argv[i])) {
        add_to_playlist(playlist, count, argv[i]);
      }
    }
  }
}

static void free_playlist(char **playlist, int count) {
  if (!playlist) return;
  for (int i = 0; i < count; i++) free(playlist[i]);
  free(playlist);
}

/* Read streaminfo and (re)use or open the ALSA device. */
static void metadata_callback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data) {
  (void)decoder;
  (void)client_data;

  if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) return;

  total_samples_global = metadata->data.stream_info.total_samples;
  sample_rate_global = metadata->data.stream_info.sample_rate;
  fixed_blocksize_global = metadata->data.stream_info.min_blocksize;

  unsigned channels = metadata->data.stream_info.channels;
  unsigned bps = metadata->data.stream_info.bits_per_sample;

  snd_pcm_format_t format;
  if (bps <= 16) {
    format = SND_PCM_FORMAT_S16_LE;
    pcm_bytes_per_sample = 2;
    pcm_shift = 16 - bps;
  } else {
    format = SND_PCM_FORMAT_S32_LE;
    pcm_bytes_per_sample = 4;
    pcm_shift = 32 - bps;
  }
  pcm_frame_bytes = (size_t)pcm_bytes_per_sample * channels;

  pcm_ensure(channels, sample_rate_global, format);
}

/* Starting sample number of a frame (works for fixed and variable blocksize). */
static uint64_t frame_start_sample(const FLAC__Frame *frame) {
  if (frame->header.number_type == FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER) {
    return frame->header.number.sample_number;
  }
  return (uint64_t)frame->header.number.frame_number * fixed_blocksize_global;
}

/* Check input, convert samples, write to ALSA. */
static FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data) {
  (void)decoder;
  (void)client_data;

  if (!pcm_handle) {
    user_action = ACT_ERROR;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  if (got_signal) {
    user_action = ACT_QUIT;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  if (user_action != ACT_NONE) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

  int action = handle_keyboard_input();
  if (action != ACT_NONE) {
    user_action = action;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  while (is_paused) {
    usleep(50000);
    if (got_signal) {
      user_action = ACT_QUIT;
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    action = handle_keyboard_input();
    if (action != ACT_NONE) {
      user_action = action;
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  }

  size_t num_samples = frame->header.blocksize;
  unsigned channels = frame->header.channels;
  size_t need = num_samples * channels * pcm_bytes_per_sample;

  if (need > pcm_buf_size) {
    uint8_t *nb = realloc(pcm_buf, need);
    if (!nb) {
      user_action = ACT_ERROR;
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    pcm_buf = nb;
    pcm_buf_size = need;
  }

  if (pcm_bytes_per_sample == 2) {
    int16_t *out = (int16_t *)pcm_buf;
    for (size_t i = 0; i < num_samples; i++) {
      for (unsigned ch = 0; ch < channels; ch++) {
        out[i * channels + ch] = (int16_t)((uint32_t)buffer[ch][i] << pcm_shift);
      }
    }
  } else {
    int32_t *out = (int32_t *)pcm_buf;
    for (size_t i = 0; i < num_samples; i++) {
      for (unsigned ch = 0; ch < channels; ch++) {
        out[i * channels + ch] = (int32_t)((uint32_t)buffer[ch][i] << pcm_shift);
      }
    }
  }

  if (pcm_write_all(pcm_buf, (snd_pcm_uframes_t)num_samples) < 0) {
    user_action = got_signal ? ACT_QUIT : ACT_ERROR;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  current_samples = frame_start_sample(frame) + num_samples;
  print_progress();

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data) {
  (void)decoder;
  (void)client_data;
  fprintf(stderr, "\n[mplay] FLAC error: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: %s <file_or_directory> [file2 file3 ...]\n", argv[0]);
    return 1;
  }

  char **playlist = NULL;
  int playlist_count = 0;
  build_playlist(argc, argv, &playlist, &playlist_count);

  if (playlist_count == 0) {
    fprintf(stderr, "[mplay] .flac file not found.\n");
    return 1;
  }

  printf("[mplay] Loaded: %d song.\n", playlist_count);
  printf("Controls: [SPACE] Pause | [LEFT/a] -5s | [RIGHT/d] +5s | [n] Next | [p] Prev | [q] Quit\n\n");

  setup_signals();
  enable_raw_mode();

  int stop_now = 0;
  int pending_newline = 0;
  int playlist_index = 0;

  while (playlist_index >= 0 && playlist_index < playlist_count && !got_signal) {
    char *current_file = playlist[playlist_index];
    const char *base = strrchr(current_file, '/');
    base = base ? base + 1 : current_file;

    printf("[mplay] (%d/%d) Playing: %s\n", playlist_index + 1, playlist_count, base);
    fflush(stdout);

    current_samples = 0;
    total_samples_global = 0;
    user_action = ACT_NONE;
    is_paused = 0;

    FLAC__StreamDecoder *decoder = FLAC__stream_decoder_new();
    if (!decoder) break;

    FLAC__StreamDecoderInitStatus init_status = FLAC__stream_decoder_init_file(
        decoder, current_file, write_callback, metadata_callback, error_callback, NULL);

    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      fprintf(stderr, "[mplay] Failed to open %s: %s\n", base,
              FLAC__StreamDecoderInitStatusString[init_status]);
      FLAC__stream_decoder_delete(decoder);
      playlist_index++;
      continue;
    }

    for (;;) {
      FLAC__stream_decoder_process_until_end_of_stream(decoder);
      if (user_action != ACT_SEEK) break;

      user_action = ACT_NONE;
      pcm_flush_now();
      FLAC__stream_decoder_flush(decoder);
      if (!FLAC__stream_decoder_seek_absolute(decoder, seek_target_global)) {
        if (FLAC__stream_decoder_get_state(decoder) == FLAC__STREAM_DECODER_SEEK_ERROR) {
          FLAC__stream_decoder_flush(decoder);
        }
      }
      current_samples = seek_target_global;
    }

    stop_now = (user_action != ACT_NONE) || got_signal;

    if (stop_now) {
      if (user_action != ACT_QUIT && !got_signal) pcm_flush_now();
    } else if (total_samples_global > 0) {
      print_progress_ex(1);
    }

    FLAC__stream_decoder_finish(decoder);
    FLAC__stream_decoder_delete(decoder);

    if (!stop_now && playlist_index + 1 >= playlist_count) {
      pending_newline = 1;
    } else {
      printf("\n");
      fflush(stdout);
    }

    if (user_action == ACT_QUIT || got_signal) {
      printf("[mplay] Stopped by user.\n");
      break;
    } else if (user_action == ACT_PREV) {
      playlist_index--;
      if (playlist_index < 0) playlist_index = 0;
    } else {
      playlist_index++;
    }
  }

  pcm_close(!stop_now && !got_signal);
  if (pending_newline) printf("\n");

  disable_raw_mode();
  free(pcm_buf);
  free_playlist(playlist, playlist_count);
  printf("[mplay] Finished.\n");

  return 0;
}
