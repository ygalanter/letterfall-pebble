#include <pebble.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"

#define SCREEN_W 200
#define SCREEN_H 228

#define BOARD_ROWS 5
#define BOARD_COLS 5
#define TILE_OUTER 38
#define TILE_INSET 1
#define TILE_DRAW (TILE_OUTER - TILE_INSET * 2)
#define TILE_RADIUS 4

#define GRID_W (BOARD_COLS * TILE_OUTER)
#define GRID_H (BOARD_ROWS * TILE_OUTER)
#define GRID_X ((SCREEN_W - GRID_W) / 2)
#define GRID_Y ((SCREEN_H - GRID_H) / 2)

#define MAX_SELECTION 24
#define MIN_WORD_LEN 3
#define MAX_WORD_LEN 24

#define POPUP_DURATION_MS 2000
#define ANIM_FRAME_MS 35
#define ANIM_STEPS 14         // 14 * 35ms ≈ 500ms per fade phase (~1s total drop)
#define SHAKE_DEBOUNCE_MS 800

#define PERSIST_VERSION 1
#define PERSIST_KEY_VERSION 1
#define PERSIST_KEY_SCORE 2
#define PERSIST_KEY_RANK 3
#define PERSIST_KEY_BOARD 4
#define PERSIST_KEY_HAS_GAME 5
#define PERSIST_KEY_SHAKE 6
#define PERSIST_KEY_SOUND_VOLUME 7
#define PERSIST_KEY_BACKLIGHT_COLOR 8

#define DEFAULT_SOUND_VOLUME 20  // matches Clay slider default in src/pkjs/config.json
#define DEFAULT_BACKLIGHT_COLOR BACKLIGHT_DISABLED

#define COLOR_MAIN_BACKGROUND GColorBulgarianRose
#define COLOR_MODAL_BACKGROUND GColorWindsorTan
#define COLOR_SCORE_POPUP GColorBlueMoon
#define COLOR_SELECTED_TILE GColorWindsorTan

typedef enum {
  STATUS_DEFAULT = 0,
  STATUS_BURNING = 1,
  STATUS_GOLD = 2,
  STATUS_DELETED = 3,
} TileStatus;

typedef enum {
  SCREEN_TITLE,
  SCREEN_PLAYING,
  SCREEN_POPUP,
  SCREEN_GAME_OVER,
} GameScreen;

typedef enum {
  TITLE_NEW,
  TITLE_CONTINUED,
} TitleVariant;

typedef enum {
  BACKLIGHT_DISABLED = 0,
  BACKLIGHT_WHITE,
  BACKLIGHT_YELLOW,
  BACKLIGHT_RED,
  BACKLIGHT_BLUE,
  BACKLIGHT_GREEN,
} BacklightColor;

typedef struct {
  uint8_t letter;
  uint8_t cost;
  uint8_t status;
  uint8_t alpha;  // 255 = fully visible, 0 = fully transparent (toward black board)
} Tile;

static Window *s_window;
static Layer *s_scene_layer;

static Tile s_board[BOARD_ROWS][BOARD_COLS];
static int s_selected[MAX_SELECTION][2];
static int s_selected_count;

static GameScreen s_screen = SCREEN_TITLE;
static TitleVariant s_title_variant = TITLE_NEW;
static int s_score;
static int s_rank;

static int s_small_count;  // unit = 0.5; +1 for 4-letter, +2 for 3-letter
static int s_small_limit;  // 6..10 (= 2 * (3..5))
static int s_big_count;    // unit = 0.5
static int s_big_limit;    // 6..14 (= 2 * (3..7))

static char s_popup_word[MAX_WORD_LEN + 1];
static int s_popup_cost;
static char s_rank_line[40];

static AppTimer *s_anim_timer;
static AppTimer *s_popup_timer;

static bool s_input_locked;
static bool s_shake_enabled;
static int s_sound_volume;
static BacklightColor s_backlight_color;
static time_t s_last_shake;

// ---------------------------------------------------------------------------
// Backlight
// ---------------------------------------------------------------------------

static BacklightColor clamp_backlight_color(int value) {
  if (value < BACKLIGHT_DISABLED || value > BACKLIGHT_GREEN) {
    return BACKLIGHT_DISABLED;
  }
  return (BacklightColor)value;
}

static void update_game_backlight(void) {
  if (s_backlight_color == BACKLIGHT_DISABLED) {
    light_enable(false);
    light_set_system_color();
    return;
  }

  uint32_t rgb = 0xFFFFFF;
  switch (s_backlight_color) {
    case BACKLIGHT_YELLOW:
      rgb = 0xFFFF00;
      break;
    case BACKLIGHT_RED:
      rgb = 0xFF0000;
      break;
    case BACKLIGHT_BLUE:
      rgb = 0x0000FF;
      break;
    case BACKLIGHT_GREEN:
      rgb = 0x00FF00;
      break;
    case BACKLIGHT_WHITE:
    default:
      break;
  }

  light_set_color_rgb888(rgb);
  light_enable(true);
}

// ---------------------------------------------------------------------------
// Sound feedback
// ---------------------------------------------------------------------------

static const SpeakerNote GOOD_WORD_NOTES[] = {
  { .midi_note = 84, .waveform = SpeakerWaveformSine, .duration_ms = 55 },
  { .midi_note = 88, .waveform = SpeakerWaveformSine, .duration_ms = 55 },
  { .midi_note = 91, .waveform = SpeakerWaveformSine, .duration_ms = 90 },
};

static const SpeakerNote BAD_WORD_NOTES[] = {
  { .midi_note = 55, .waveform = SpeakerWaveformSquare, .duration_ms = 90 },
  { .midi_note = 48, .waveform = SpeakerWaveformSquare, .duration_ms = 130 },
};

// Longer, slower descending lament so game over reads as more weighty
// than just submitting an invalid word.
static const SpeakerNote GAME_OVER_NOTES[] = {
  { .midi_note = 60, .waveform = SpeakerWaveformSquare, .duration_ms = 150 },
  { .midi_note = 55, .waveform = SpeakerWaveformSquare, .duration_ms = 150 },
  { .midi_note = 50, .waveform = SpeakerWaveformSquare, .duration_ms = 150 },
  { .midi_note = 43, .waveform = SpeakerWaveformSquare, .duration_ms = 350 },
};

static int clamp_sound_volume(int value) {
  if (value < 0) return 0;
  if (value > 100) return 100;
  return value;
}

static uint8_t sound_velocity(void) {
  int volume = clamp_sound_volume(s_sound_volume);
  if (volume <= 0) return 0;
  return (uint8_t)((volume * 127 + 50) / 100);
}

static void play_single_tone(uint8_t midi_note, uint16_t duration_ms,
                             SpeakerWaveform waveform) {
  uint8_t velocity = sound_velocity();
  if (velocity == 0) return;
  SpeakerNote note = {
    .midi_note = midi_note,
    .waveform = waveform,
    .duration_ms = duration_ms,
    .velocity = velocity,
  };
  speaker_stop();
  speaker_play_notes(&note, 1, 100);
}

static void play_note_sequence(const SpeakerNote *src, size_t count) {
  uint8_t velocity = sound_velocity();
  if (velocity == 0 || count == 0) return;
  // Stack-copy so we can stamp the per-note velocity without mutating the
  // const template.
  SpeakerNote notes[8];
  if (count > sizeof(notes) / sizeof(notes[0])) {
    count = sizeof(notes) / sizeof(notes[0]);
  }
  memcpy(notes, src, count * sizeof(SpeakerNote));
  for (size_t i = 0; i < count; i++) {
    notes[i].velocity = velocity;
  }
  speaker_stop();
  speaker_play_notes(notes, count, 100);
}

// Tile-press feedback: short, bright square click.
static void play_press_tone(void) {
  play_single_tone(88, 45, SpeakerWaveformSquare);
}

// Tile-unpress: a perfect fourth below the press tone, so retracting a
// selection sounds audibly distinct from extending it.
static void play_unpress_tone(void) {
  play_single_tone(83, 45, SpeakerWaveformSquare);
}

static void play_good_tone(void) {
  play_note_sequence(GOOD_WORD_NOTES,
                     sizeof(GOOD_WORD_NOTES) / sizeof(GOOD_WORD_NOTES[0]));
}

static void play_bad_tone(void) {
  play_note_sequence(BAD_WORD_NOTES,
                     sizeof(BAD_WORD_NOTES) / sizeof(BAD_WORD_NOTES[0]));
}

static void play_game_over_tone(void) {
  play_note_sequence(GAME_OVER_NOTES,
                     sizeof(GAME_OVER_NOTES) / sizeof(GAME_OVER_NOTES[0]));
}

// Letter frequency thresholds match the original Fitbit implementation.
// pairs of {threshold * 10000, letter, cost}
static const struct {
  uint16_t threshold;
  char letter;
  uint8_t cost;
} LETTER_TABLE[] = {
  { 1304, 'E', 1 }, { 2349, 'T', 1 }, { 3205, 'A', 1 }, { 4002, 'O', 1 },
  { 4709, 'N', 1 }, { 5386, 'R', 1 }, { 6013, 'I', 1 }, { 6620, 'S', 1 },
  { 7148, 'H', 1 }, { 7526, 'D', 1 }, { 7865, 'L', 1 }, { 8154, 'F', 1 },
  { 8433, 'C', 1 }, { 8682, 'M', 1 }, { 8931, 'U', 1 }, { 9130, 'G', 1 },
  { 9329, 'P', 1 }, { 9528, 'Y', 1 }, { 9677, 'W', 1 }, { 9816, 'B', 3 },
  { 9908, 'V', 2 }, { 9950, 'K', 2 }, { 9967, 'X', 2 }, { 9980, 'J', 2 },
  { 9992, 'Q', 3 }, {10000, 'Z', 3 },
};

static const struct {
  int score;
  const char *title;
} RANKS[] = {
  {     0, "Illiterate" },
  {    10, "Kindergarden" },
  {    15, "First grader" },
  {    25, "Grammar Student" },
  {    30, "High school student" },
  {    50, "College student" },
  {    75, "Junior librarian" },
  {   115, "Avid reader" },
  {   170, "Librarian" },
  {   250, "Literature teacher" },
  {   380, "Language scientist" },
  {   570, "Senior lecturer" },
  {   860, "Literary PhD" },
  {  1300, "Word treasurer" },
  {  1900, "Printing machine" },
  {  3000, "Master of ABC" },
  {  4300, "Senior wordmonger" },
  {  6500, "Academician" },
  {  9800, "Google competetor" },
  { 14000, "Light of knowledge" },
  { 22000, "Walking encyclopedia" },
  { 33000, "Alphabet hero" },
  { 50000, "Absolute genius" },
};

#define RANK_COUNT (int)(sizeof(RANKS) / sizeof(RANKS[0]))

// ---------------------------------------------------------------------------
// Board helpers
// ---------------------------------------------------------------------------

static bool is_submit_tile(int row, int col) {
  return row == BOARD_ROWS - 1 && col == BOARD_COLS - 1;
}

static void mark_dirty(void) {
  if (s_scene_layer) {
    layer_mark_dirty(s_scene_layer);
  }
}

static void random_letter(int row, int col) {
  uint16_t r = (uint16_t)(rand() % 10000) + 1;
  for (size_t i = 0; i < sizeof(LETTER_TABLE) / sizeof(LETTER_TABLE[0]); i++) {
    if (r <= LETTER_TABLE[i].threshold) {
      s_board[row][col].letter = (uint8_t)LETTER_TABLE[i].letter;
      s_board[row][col].cost = LETTER_TABLE[i].cost;
      return;
    }
  }
  s_board[row][col].letter = 'E';
  s_board[row][col].cost = 1;
}

static void clear_selection(void) {
  s_selected_count = 0;
}

static bool selection_contains(int row, int col, int *out_index) {
  for (int i = 0; i < s_selected_count; i++) {
    if (s_selected[i][0] == row && s_selected[i][1] == col) {
      if (out_index) *out_index = i;
      return true;
    }
  }
  return false;
}

static void reset_small_words(void) {
  s_small_count = 0;
  s_small_limit = 6 + (rand() % 5);  // 6..10 (= 2*(3..5))
}

static void reset_big_words(void) {
  s_big_count = 0;
  s_big_limit = 6 + (rand() % 9);   // 6..14 (= 2*(3..7))
}

static void start_field(void) {
  for (int row = 0; row < BOARD_ROWS; row++) {
    for (int col = 0; col < BOARD_COLS; col++) {
      s_board[row][col].status = STATUS_DEFAULT;
      s_board[row][col].alpha = 255;
      if (is_submit_tile(row, col)) {
        s_board[row][col].letter = 0;
        s_board[row][col].cost = 0;
      } else {
        random_letter(row, col);
      }
    }
  }
  clear_selection();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

static void save_field(void) {
  uint8_t buf[BOARD_ROWS * BOARD_COLS * 3];
  int idx = 0;
  for (int row = 0; row < BOARD_ROWS; row++) {
    for (int col = 0; col < BOARD_COLS; col++) {
      buf[idx++] = s_board[row][col].letter;
      buf[idx++] = s_board[row][col].cost;
      buf[idx++] = s_board[row][col].status;
    }
  }
  persist_write_data(PERSIST_KEY_BOARD, buf, sizeof(buf));
  persist_write_int(PERSIST_KEY_HAS_GAME, 1);
  persist_write_int(PERSIST_KEY_SCORE, s_score);
  persist_write_int(PERSIST_KEY_RANK, s_rank);
}

static void clear_saved_game(void) {
  persist_delete(PERSIST_KEY_HAS_GAME);
  persist_delete(PERSIST_KEY_BOARD);
}

static bool load_field(void) {
  if (!persist_exists(PERSIST_KEY_HAS_GAME)) return false;
  if (persist_read_int(PERSIST_KEY_HAS_GAME) != 1) return false;
  if (!persist_exists(PERSIST_KEY_BOARD)) return false;

  uint8_t buf[BOARD_ROWS * BOARD_COLS * 3];
  int read = persist_read_data(PERSIST_KEY_BOARD, buf, sizeof(buf));
  if (read != (int)sizeof(buf)) return false;

  int idx = 0;
  for (int row = 0; row < BOARD_ROWS; row++) {
    for (int col = 0; col < BOARD_COLS; col++) {
      s_board[row][col].letter = buf[idx++];
      s_board[row][col].cost = buf[idx++];
      s_board[row][col].status = buf[idx++];
      s_board[row][col].alpha = 255;
      if (is_submit_tile(row, col)) {
        s_board[row][col].letter = 0;
        s_board[row][col].cost = 0;
        s_board[row][col].status = STATUS_DEFAULT;
      }
    }
  }
  clear_selection();
  return true;
}

// ---------------------------------------------------------------------------
// Word validation against hashes.bin
// ---------------------------------------------------------------------------

static uint32_t word_hash_prefix(const char *word, int len) {
  Sha256Ctx ctx;
  sha256_init(&ctx);
  uint8_t pair[2];
  for (int i = 0; i < len; i++) {
    char ch = word[i];
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    pair[0] = (uint8_t)ch;
    pair[1] = 0;
    sha256_update(&ctx, pair, 2);
  }
  uint8_t digest[32];
  sha256_final(&ctx, digest);
  return ((uint32_t)digest[0]) |
         ((uint32_t)digest[1] << 8) |
         ((uint32_t)digest[2] << 16) |
         ((uint32_t)digest[3] << 24);
}

static bool word_is_valid(const char *word, int len) {
  if (len < MIN_WORD_LEN) return false;
  uint32_t key = word_hash_prefix(word, len);

  ResHandle h = resource_get_handle(RESOURCE_ID_HASHES);
  size_t bytes = resource_size(h);
  if (bytes < 4) return false;
  int count = (int)(bytes / 4);

  int min_idx = 0;
  int max_idx = count - 1;
  int prev_idx = -1;
  int idx = 0;
  while (prev_idx != idx) {
    prev_idx = idx;
    idx = (max_idx + min_idx) / 2;
    uint32_t value;
    if (resource_load_byte_range(h, idx * 4, (uint8_t *)&value, 4) != 4) {
      return false;
    }
    if (value == key) return true;
    if (value < key) min_idx = idx;
    else max_idx = idx;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Drop / scoring mechanics
// ---------------------------------------------------------------------------

static bool any_burning_at_dead_end(void);  // forward
static void create_burning_letters(int word_len);
static void create_gold_letters(int word_len);
static void start_drop_animation(int word_len);

static int rank_score_for(int rank) {
  if (rank < 0 || rank >= RANK_COUNT) return -1;
  return RANKS[rank].score;
}

static const char *rank_title_for(int rank) {
  if (rank < 0) rank = 0;
  if (rank >= RANK_COUNT) rank = RANK_COUNT - 1;
  return RANKS[rank].title;
}

static void update_rank_line(void) {
  if (s_rank + 1 < RANK_COUNT) {
    int next = rank_score_for(s_rank + 1);
    snprintf(s_rank_line, sizeof(s_rank_line), "Next rank in %d pts", next - s_score);
  } else {
    snprintf(s_rank_line, sizeof(s_rank_line), "RANK: %s", rank_title_for(s_rank));
  }
}

// Compact columns by removing DELETED tiles. Tiles that move retain their alpha
// (they're already opaque). Newly created tiles at the top start at alpha 0
// so the fade-in animation can reveal them.
static void compact_columns(void) {
  for (int col = 0; col < BOARD_COLS; col++) {
    int bottom = (col == BOARD_COLS - 1) ? BOARD_ROWS - 2 : BOARD_ROWS - 1;
    int write = bottom;
    for (int row = bottom; row >= 0; row--) {
      if (s_board[row][col].status != STATUS_DELETED) {
        if (row != write) {
          s_board[write][col] = s_board[row][col];
        }
        write--;
      }
    }
    while (write >= 0) {
      s_board[write][col].status = STATUS_DEFAULT;
      s_board[write][col].alpha = 0;
      random_letter(write, col);
      write--;
    }
  }
}

static void force_all_alpha_full(void) {
  for (int row = 0; row < BOARD_ROWS; row++) {
    for (int col = 0; col < BOARD_COLS; col++) {
      s_board[row][col].alpha = 255;
    }
  }
}

// Mark the user's selected tiles as DELETED but keep the s_selected list intact:
// the renderer relies on selection_contains() to draw them in the pressed color
// during fade-out. Selection is cleared just before compact_columns().
static void mark_pressed_deleted(void) {
  for (int i = 0; i < s_selected_count; i++) {
    int row = s_selected[i][0];
    int col = s_selected[i][1];
    if (!is_submit_tile(row, col)) {
      s_board[row][col].status = STATUS_DELETED;
    }
  }
}

// Mark tiles below burning ones as DELETED so the burning tiles drop down.
// Returns false if any burning tile has nowhere to drop (game over).
static bool mark_below_burning(void) {
  for (int row = 0; row < BOARD_ROWS; row++) {
    for (int col = 0; col < BOARD_COLS; col++) {
      if (s_board[row][col].status != STATUS_BURNING) continue;
      // Game over if burning sits at bottom (non-submit) or directly above submit.
      if ((col < BOARD_COLS - 1 && row == BOARD_ROWS - 1) ||
          (col == BOARD_COLS - 1 && row == BOARD_ROWS - 2)) {
        return false;
      }
      int below_row = row + 1;
      int below_col = col;
      if (is_submit_tile(below_row, below_col)) continue;  // safety
      s_board[below_row][below_col].status = STATUS_DELETED;
    }
  }
  return true;
}

static void create_burning_letters(int word_len) {
  if (word_len == 4) s_small_count += 1;
  else if (word_len == 3) s_small_count += 2;

  if (s_small_count >= s_small_limit) {
    int n = 1 + ((rand() % 5));  // 1..5
    for (int i = 0; i < n; i++) {
      int c = rand() % BOARD_COLS;
      s_board[0][c].status = STATUS_BURNING;
    }
    reset_small_words();
  }
}

static void create_gold_letters(int word_len) {
  if (word_len == 5) s_big_count += 1;
  else if (word_len > 5) s_big_count += 2;

  if (s_big_count >= s_big_limit) {
    int n = 1 + ((rand() % 3));  // 1..3
    for (int i = 0; i < n; i++) {
      int row = rand() % (BOARD_ROWS - 1);     // 0..3, avoid submit row
      int col = rand() % BOARD_COLS;
      if (is_submit_tile(row, col)) continue;
      s_board[row][col].status = STATUS_GOLD;
    }
    reset_big_words();
  }
}

// ---------------------------------------------------------------------------
// Drop pipeline with fade animation
// ---------------------------------------------------------------------------

// Each drop is two phases: FADE_OUT animates DELETED tiles 255→0, then
// compact runs (placing new tiles at alpha 0), then FADE_IN animates 0→255.
// After a word drop's fade-in completes, we check for burning-tile fallout
// and may run a second drop cycle for the fire propagation.
typedef enum {
  ANIM_NONE,
  ANIM_FADE_OUT,
  ANIM_FADE_IN,
} AnimPhase;

static AnimPhase s_anim_phase;
static int s_anim_progress;
static int s_drop_word_len;
static bool s_drop_processing_word;  // false once we're on the fire cascade

static void anim_tick(void *ctx);

static void cancel_anim_timer(void) {
  if (s_anim_timer) {
    app_timer_cancel(s_anim_timer);
    s_anim_timer = NULL;
  }
}

static void enter_game_over(void) {
  cancel_anim_timer();
  s_input_locked = false;
  s_anim_phase = ANIM_NONE;
  s_drop_processing_word = false;
  force_all_alpha_full();
  s_screen = SCREEN_GAME_OVER;
  update_game_backlight();
  play_game_over_tone();
  mark_dirty();
}

static void start_fade_out(void) {
  s_anim_phase = ANIM_FADE_OUT;
  s_anim_progress = 0;
  cancel_anim_timer();
  s_anim_timer = app_timer_register(ANIM_FRAME_MS, anim_tick, NULL);
  mark_dirty();
}

static void start_fade_in(void) {
  s_anim_phase = ANIM_FADE_IN;
  s_anim_progress = 0;
  cancel_anim_timer();
  s_anim_timer = app_timer_register(ANIM_FRAME_MS, anim_tick, NULL);
  mark_dirty();
}

static void start_drop_animation(int word_len) {
  s_input_locked = true;
  s_drop_word_len = word_len;
  s_drop_processing_word = true;
  start_fade_out();
}

static void start_fire_drop_animation(void) {
  s_input_locked = true;
  s_drop_word_len = 0;
  s_drop_processing_word = false;
  start_fade_out();
}

static bool any_deleted_tile(void) {
  for (int row = 0; row < BOARD_ROWS; row++) {
    for (int col = 0; col < BOARD_COLS; col++) {
      if (s_board[row][col].status == STATUS_DELETED) return true;
    }
  }
  return false;
}

static void anim_tick(void *ctx) {
  (void)ctx;
  s_anim_timer = NULL;
  s_anim_progress++;

  int frac = (s_anim_progress >= ANIM_STEPS) ? 255 : (255 * s_anim_progress) / ANIM_STEPS;
  if (s_anim_phase == ANIM_FADE_OUT) {
    int alpha = 255 - frac;
    for (int row = 0; row < BOARD_ROWS; row++) {
      for (int col = 0; col < BOARD_COLS; col++) {
        if (s_board[row][col].status == STATUS_DELETED) {
          s_board[row][col].alpha = (uint8_t)alpha;
        }
      }
    }
  } else if (s_anim_phase == ANIM_FADE_IN) {
    int alpha = frac;
    for (int row = 0; row < BOARD_ROWS; row++) {
      for (int col = 0; col < BOARD_COLS; col++) {
        if (s_board[row][col].alpha < 255) {
          s_board[row][col].alpha = (uint8_t)alpha;
        }
      }
    }
  }
  mark_dirty();

  if (s_anim_progress < ANIM_STEPS) {
    s_anim_timer = app_timer_register(ANIM_FRAME_MS, anim_tick, NULL);
    return;
  }

  // Phase complete: transition.
  if (s_anim_phase == ANIM_FADE_OUT) {
    // Clear selection before compact reshuffles tile positions, so the now-stale
    // (row, col) entries in s_selected don't accidentally light up new letters.
    clear_selection();
    compact_columns();
    start_fade_in();
    return;
  }

  // ANIM_FADE_IN complete.
  force_all_alpha_full();

  if (s_drop_processing_word) {
    s_drop_processing_word = false;
    if (!mark_below_burning()) {
      enter_game_over();
      return;
    }
    create_burning_letters(s_drop_word_len);
    create_gold_letters(s_drop_word_len);
    if (any_deleted_tile()) {
      start_fade_out();
      return;
    }
  }

  s_anim_phase = ANIM_NONE;
  s_input_locked = false;
  save_field();
  mark_dirty();
}

static bool complete_drop_immediately(bool process_word, int word_len) {
  if (any_deleted_tile()) {
    compact_columns();
  }
  force_all_alpha_full();

  if (process_word) {
    if (!mark_below_burning()) {
      return false;
    }
    create_burning_letters(word_len);
    create_gold_letters(word_len);
    if (any_deleted_tile()) {
      compact_columns();
      force_all_alpha_full();
    }
  }

  s_anim_phase = ANIM_NONE;
  s_drop_processing_word = false;
  s_input_locked = false;
  save_field();
  mark_dirty();
  return true;
}

static bool finish_active_drop_immediately(void) {
  bool process_word = s_drop_processing_word;
  int word_len = s_drop_word_len;
  cancel_anim_timer();
  return complete_drop_immediately(process_word, word_len);
}

// ---------------------------------------------------------------------------
// Scoring + popup
// ---------------------------------------------------------------------------

static void popup_dismiss(void *ctx);

static void show_score_popup(const char *word, int cost) {
  strncpy(s_popup_word, word, sizeof(s_popup_word) - 1);
  s_popup_word[sizeof(s_popup_word) - 1] = '\0';
  s_popup_cost = cost;
  update_rank_line();
  s_screen = SCREEN_POPUP;
  update_game_backlight();
  if (s_popup_timer) {
    app_timer_cancel(s_popup_timer);
  }
  s_popup_timer = app_timer_register(POPUP_DURATION_MS, popup_dismiss, NULL);
  mark_dirty();
}

static void popup_dismiss(void *ctx) {
  (void)ctx;
  s_popup_timer = NULL;
  s_screen = SCREEN_PLAYING;
  update_game_backlight();
  mark_pressed_deleted();
  start_drop_animation((int)strlen(s_popup_word));
}

static void on_word_validated(bool valid, const char *word, int word_len, int cost_total) {
  (void)word_len;
  if (!valid) {
    play_bad_tone();
    // Reset selection visuals; the press list is cleared to allow new picks.
    clear_selection();
    s_input_locked = false;
    mark_dirty();
    return;
  }

  s_score += cost_total;
  if (s_rank + 1 < RANK_COUNT && s_score >= rank_score_for(s_rank + 1)) {
    s_rank++;
  }
  play_good_tone();
  show_score_popup(word, cost_total);
}

static void submit_word(void) {
  if (s_selected_count < MIN_WORD_LEN) return;

  char word[MAX_WORD_LEN + 1];
  int len = 0;
  int cost = 0;
  int gold_mul = 1;
  for (int i = 0; i < s_selected_count && len < MAX_WORD_LEN; i++) {
    int row = s_selected[i][0];
    int col = s_selected[i][1];
    Tile *t = &s_board[row][col];
    word[len++] = (char)t->letter;
    cost += t->cost;
    if (t->status == STATUS_GOLD) gold_mul++;
  }
  word[len] = '\0';

  s_input_locked = true;

  bool valid = word_is_valid(word, len);
  on_word_validated(valid, word, len, cost * gold_mul);
}

// ---------------------------------------------------------------------------
// Selection logic
// ---------------------------------------------------------------------------

static bool is_neighbor_of_last(int row, int col) {
  if (s_selected_count == 0) return true;
  int lr = s_selected[s_selected_count - 1][0];
  int lc = s_selected[s_selected_count - 1][1];
  int dr = row - lr;
  int dc = col - lc;
  if (dr < 0) dr = -dr;
  if (dc < 0) dc = -dc;
  return dr <= 1 && dc <= 1;
}

static void tap_letter(int row, int col) {
  if (is_submit_tile(row, col)) return;

  int idx;
  if (selection_contains(row, col, &idx)) {
    // Only allow unpressing the most-recent tile.
    if (idx == s_selected_count - 1) {
      s_selected_count--;
      play_unpress_tone();
      mark_dirty();
    }
    return;
  }

  if (!is_neighbor_of_last(row, col)) return;
  if (s_selected_count >= MAX_SELECTION) return;

  s_selected[s_selected_count][0] = row;
  s_selected[s_selected_count][1] = col;
  s_selected_count++;
  play_press_tone();
  mark_dirty();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static GRect tile_rect(int row, int col) {
  return GRect(GRID_X + col * TILE_OUTER + TILE_INSET,
               GRID_Y + row * TILE_OUTER + TILE_INSET,
               TILE_DRAW, TILE_DRAW);
}

static GColor tile_fill_color(int row, int col) {
  if (is_submit_tile(row, col)) return GColorTiffanyBlue;
  Tile *t = &s_board[row][col];
  switch (t->status) {
    case STATUS_BURNING: return GColorDarkCandyAppleRed;
    case STATUS_GOLD: return GColorChromeYellow;
    default: return GColorPastelYellow;
  }
}

static GColor tile_text_color(int row, int col) {
  if (is_submit_tile(row, col)) return GColorBlack;
  Tile *t = &s_board[row][col];
  if (t->status == STATUS_BURNING) return GColorWhite;
  return GColorBlack;
}

static void draw_letter(GContext *ctx, GRect rect, char letter, GColor color) {
  graphics_context_set_text_color(ctx, color);
  char buf[2] = { letter, '\0' };
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GRect text_rect = GRect(rect.origin.x, rect.origin.y - 1, rect.size.w, rect.size.h);
  graphics_draw_text(ctx, buf, font, text_rect,
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void draw_cost_badge(GContext *ctx, GRect rect, uint8_t cost, GColor color) {
  if (cost <= 1) return;
  char buf[4];
  snprintf(buf, sizeof(buf), "%u", cost);
  graphics_context_set_text_color(ctx, color);
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  GRect badge = GRect(rect.origin.x + rect.size.w - 12,
                      rect.origin.y - 2,
                      12, 14);
  graphics_draw_text(ctx, buf, font, badge,
                     GTextOverflowModeWordWrap, GTextAlignmentRight, NULL);
}

static void draw_submit_button(GContext *ctx, GRect rect, bool enabled) {
  graphics_context_set_fill_color(ctx, GColorTiffanyBlue);
  graphics_fill_rect(ctx, rect, TILE_RADIUS, GCornersAll);

  GColor arrow_color = enabled ? GColorBlack : GColorDarkGray;
  graphics_context_set_stroke_color(ctx, arrow_color);
  graphics_context_set_stroke_width(ctx, 3);
  int cx = rect.origin.x + rect.size.w / 2;
  int cy = rect.origin.y + rect.size.h / 2;
  graphics_draw_line(ctx, GPoint(cx - 8, cy), GPoint(cx + 8, cy));
  graphics_draw_line(ctx, GPoint(cx + 8, cy), GPoint(cx + 2, cy - 6));
  graphics_draw_line(ctx, GPoint(cx + 8, cy), GPoint(cx + 2, cy + 6));
  graphics_context_set_stroke_width(ctx, 1);
}

static uint8_t blend_component(uint8_t base, uint8_t tint, uint8_t weight, uint8_t total);

static GColor fade_to_board_color(GColor color, uint8_t alpha) {
  if (alpha >= 255) return color;
  if (alpha == 0) return GColorBlack;
  GColor black = GColorBlack;
  GColor blended;
  blended.a = 3;
  blended.r = blend_component(black.r, color.r, alpha, 255);
  blended.g = blend_component(black.g, color.g, alpha, 255);
  blended.b = blend_component(black.b, color.b, alpha, 255);
  return blended;
}

static void draw_tile(GContext *ctx, int row, int col) {
  GRect rect = tile_rect(row, col);
  if (is_submit_tile(row, col)) {
    draw_submit_button(ctx, rect, s_selected_count >= MIN_WORD_LEN);
    return;
  }
  uint8_t alpha = s_board[row][col].alpha;
  if (alpha == 0) return;

  GColor fill = tile_fill_color(row, col);
  bool selected = selection_contains(row, col, NULL);
  if (selected) fill = COLOR_SELECTED_TILE;
  fill = fade_to_board_color(fill, alpha);

  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_rect(ctx, rect, TILE_RADIUS, GCornersAll);

  GColor text = selected ? GColorBlack : tile_text_color(row, col);
  text = fade_to_board_color(text, alpha);
  draw_letter(ctx, rect, (char)s_board[row][col].letter, text);
  if (!selected) {
    draw_cost_badge(ctx, rect, s_board[row][col].cost,
                    fade_to_board_color(GColorOxfordBlue, alpha));
  }
}

static void draw_board(GContext *ctx) {
  // Darker backdrop; modal tint stays closer to the original saddle-brown.
  graphics_context_set_fill_color(ctx, COLOR_MAIN_BACKGROUND);
  graphics_fill_rect(ctx,
                     GRect(0, 0, SCREEN_W, SCREEN_H),
                     0, GCornerNone);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx,
                     GRect(GRID_X - 2, GRID_Y - 2, GRID_W + 4, GRID_H + 4),
                     6, GCornersAll);

  for (int row = 0; row < BOARD_ROWS; row++) {
    for (int col = 0; col < BOARD_COLS; col++) {
      draw_tile(ctx, row, col);
    }
  }
}

static uint8_t blend_component(uint8_t base, uint8_t tint, uint8_t weight, uint8_t total) {
  return (uint8_t)((base * (total - weight) + tint * weight + total / 2) / total);
}

static void tint_rect(GContext *ctx, GRect rect, GColor tint, uint8_t weight, uint8_t total) {
  GBitmap *fb = graphics_capture_frame_buffer_format(ctx, GBitmapFormat8Bit);
  if (!fb) {
    graphics_context_set_fill_color(ctx, tint);
    graphics_fill_rect(ctx, rect, 0, GCornerNone);
    return;
  }

  int min_y = rect.origin.y < 0 ? 0 : rect.origin.y;
  int max_y = rect.origin.y + rect.size.h;
  if (max_y > SCREEN_H) max_y = SCREEN_H;
  int min_x = rect.origin.x < 0 ? 0 : rect.origin.x;
  int max_x = rect.origin.x + rect.size.w;
  if (max_x > SCREEN_W) max_x = SCREEN_W;

  for (int y = min_y; y < max_y; y++) {
    GBitmapDataRowInfo info = gbitmap_get_data_row_info(fb, y);
    int rmin = min_x < info.min_x ? info.min_x : min_x;
    int rmax = max_x - 1 > info.max_x ? info.max_x : max_x - 1;
    for (int x = rmin; x <= rmax; x++) {
      GColor base = (GColor){ .argb = info.data[x] };
      GColor blended;
      blended.a = 3;
      blended.r = blend_component(base.r, tint.r, weight, total);
      blended.g = blend_component(base.g, tint.g, weight, total);
      blended.b = blend_component(base.b, tint.b, weight, total);
      info.data[x] = blended.argb;
    }
  }

  graphics_release_frame_buffer(ctx, fb);
}

static void draw_text_centered(GContext *ctx, const char *text, GFont font,
                               GRect rect, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, rect,
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter, NULL);
}

static void draw_title_modal(GContext *ctx) {
  GRect modal = GRect(10, 30, SCREEN_W - 20, SCREEN_H - 60);
  tint_rect(ctx, modal, COLOR_MODAL_BACKGROUND, 9, 10);
  // Border ring
  graphics_context_set_stroke_color(ctx, GColorBulgarianRose);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, modal, 10);
  graphics_context_set_stroke_width(ctx, 1);

  const char *title = (s_screen == SCREEN_GAME_OVER) ? "GAME OVER" : "LETTERFALL";
  const char *prompt;
  if (s_screen == SCREEN_GAME_OVER) prompt = "Tap to exit";
  else if (s_title_variant == TITLE_CONTINUED) prompt = "Tap to resume";
  else prompt = "Tap to start";

  GFont title_font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  GFont info_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GFont prompt_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);

  draw_text_centered(ctx, title, title_font,
                     GRect(modal.origin.x, modal.origin.y + 14, modal.size.w, 32),
                     GColorPastelYellow);

  char score_buf[24];
  snprintf(score_buf, sizeof(score_buf), "SCORE: %d", s_score);
  draw_text_centered(ctx, score_buf, prompt_font,
                     GRect(modal.origin.x, modal.origin.y + 56, modal.size.w, 28),
                     GColorPastelYellow);

  draw_text_centered(ctx, "RANK:", info_font,
                     GRect(modal.origin.x + 6, modal.origin.y + 90,
                           modal.size.w - 12, 20),
                     GColorPastelYellow);

  draw_text_centered(ctx, rank_title_for(s_rank), info_font,
                     GRect(modal.origin.x + 6, modal.origin.y + 112,
                           modal.size.w - 12, 24),
                     GColorPastelYellow);

  draw_text_centered(ctx, prompt, prompt_font,
                     GRect(modal.origin.x,
                           modal.origin.y + modal.size.h - 36,
                           modal.size.w, 28),
                     GColorPastelYellow);
}

static void draw_score_popup(GContext *ctx) {
  GRect modal = GRect(10, 76, SCREEN_W - 20, 76);
  tint_rect(ctx, modal, COLOR_SCORE_POPUP, 4, 5);
  graphics_context_set_stroke_color(ctx, GColorOxfordBlue);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, modal, 8);
  graphics_context_set_stroke_width(ctx, 1);

  char line1[40];
  snprintf(line1, sizeof(line1), "%s +%d (%d)", s_popup_word, s_popup_cost, s_score);
  GFont big = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  GFont small = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  draw_text_centered(ctx, line1, big,
                     GRect(modal.origin.x + 4, modal.origin.y + 6,
                           modal.size.w - 8, 30),
                     GColorPastelYellow);
  draw_text_centered(ctx, s_rank_line, small,
                     GRect(modal.origin.x + 4, modal.origin.y + 40,
                           modal.size.w - 8, 24),
                     GColorPastelYellow);
}

static void scene_update_proc(Layer *layer, GContext *ctx) {
  (void)layer;
  draw_board(ctx);
  if (s_screen == SCREEN_TITLE || s_screen == SCREEN_GAME_OVER) {
    draw_title_modal(ctx);
  } else if (s_screen == SCREEN_POPUP) {
    draw_score_popup(ctx);
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static void enter_title(TitleVariant variant) {
  cancel_anim_timer();
  s_anim_phase = ANIM_NONE;
  s_drop_processing_word = false;
  force_all_alpha_full();
  if (s_popup_timer) {
    app_timer_cancel(s_popup_timer);
    s_popup_timer = NULL;
  }
  s_input_locked = false;
  s_title_variant = variant;
  s_screen = SCREEN_TITLE;
  update_game_backlight();
  mark_dirty();
}

static bool any_burning_at_dead_end(void) {
  for (int row = 0; row < BOARD_ROWS; row++) {
    for (int col = 0; col < BOARD_COLS; col++) {
      if (s_board[row][col].status != STATUS_BURNING) continue;
      if ((col < BOARD_COLS - 1 && row == BOARD_ROWS - 1) ||
          (col == BOARD_COLS - 1 && row == BOARD_ROWS - 2)) {
        return true;
      }
    }
  }
  return false;
}

static void start_or_resume_game(void) {
  if (any_burning_at_dead_end()) {
    enter_game_over();
    return;
  }
  s_screen = SCREEN_PLAYING;
  update_game_backlight();
  s_input_locked = false;
  mark_dirty();
}

static void handle_game_over_tap(void) {
  s_score = 0;
  s_rank = 0;
  clear_saved_game();
  start_field();
  reset_small_words();
  reset_big_words();
  s_title_variant = TITLE_NEW;
  s_screen = SCREEN_TITLE;
  update_game_backlight();
  mark_dirty();
}

static void handle_touch_point(GPoint p) {
  if (s_screen == SCREEN_TITLE) {
    start_or_resume_game();
    return;
  }
  if (s_screen == SCREEN_GAME_OVER) {
    handle_game_over_tap();
    return;
  }
  if (s_screen == SCREEN_POPUP) {
    return;
  }
  if (s_screen != SCREEN_PLAYING || s_input_locked) {
    return;
  }

  // Translate pixel point to (row, col).
  if (p.x < GRID_X || p.x >= GRID_X + GRID_W) return;
  if (p.y < GRID_Y || p.y >= GRID_Y + GRID_H) return;
  int col = (p.x - GRID_X) / TILE_OUTER;
  int row = (p.y - GRID_Y) / TILE_OUTER;
  if (col < 0) col = 0;
  if (col >= BOARD_COLS) col = BOARD_COLS - 1;
  if (row < 0) row = 0;
  if (row >= BOARD_ROWS) row = BOARD_ROWS - 1;

  if (is_submit_tile(row, col)) {
    submit_word();
  } else {
    tap_letter(row, col);
  }
}

static void touch_handler(const TouchEvent *event, void *context) {
  (void)context;
  if (!event || event->type != TouchEvent_Liftoff) return;
  handle_touch_point(GPoint(event->x, event->y));
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  if (s_screen == SCREEN_TITLE) {
    save_field();
    window_stack_pop(true);
    return;
  }
  if (s_screen == SCREEN_GAME_OVER) {
    window_stack_pop(true);
    return;
  }
  // From playing or popup → save and return to title.
  if (s_popup_timer) {
    app_timer_cancel(s_popup_timer);
    s_popup_timer = NULL;
    // If the score popup is interrupted, commit the validated word before saving.
    mark_pressed_deleted();
    if (!complete_drop_immediately(true, (int)strlen(s_popup_word))) {
      enter_game_over();
      return;
    }
  } else if (s_anim_phase != ANIM_NONE) {
    if (!finish_active_drop_immediately()) {
      enter_game_over();
      return;
    }
  }
  cancel_anim_timer();
  clear_selection();
  save_field();
  enter_title(TITLE_CONTINUED);
}

static void click_config_provider(void *context) {
  (void)context;
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

// Read a Tuple as a signed integer regardless of its underlying width. Clay can
// send 1/2/4-byte ints depending on the value, and reading the wrong union
// member returns garbage past the actual data.
static int32_t parse_int_tuple(Tuple *t, int32_t fallback) {
  if (!t) return fallback;
  switch (t->type) {
    case TUPLE_INT:
      switch (t->length) {
        case 1: return t->value->int8;
        case 2: return t->value->int16;
        case 4: return t->value->int32;
      }
      return fallback;
    case TUPLE_UINT:
      switch (t->length) {
        case 1: return t->value->uint8;
        case 2: return t->value->uint16;
        case 4: return (int32_t)t->value->uint32;
      }
      return fallback;
    case TUPLE_CSTRING:
      return t->length > 0 ? atoi(t->value->cstring) : fallback;
    default:
      return fallback;
  }
}

static bool parse_bool_tuple(Tuple *t, bool fallback) {
  if (!t) return fallback;
  if (t->type == TUPLE_CSTRING) {
    if (t->length == 0) return fallback;
    const char *v = t->value->cstring;
    if (strcmp(v, "true") == 0 || strcmp(v, "TRUE") == 0) return true;
    if (strcmp(v, "false") == 0 || strcmp(v, "FALSE") == 0) return false;
    return atoi(v) != 0;
  }
  return parse_int_tuple(t, fallback ? 1 : 0) != 0;
}

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  (void)ctx;
  Tuple *t_volume = dict_find(iter, MESSAGE_KEY_SOUND_VOLUME);
  Tuple *t_shake = dict_find(iter, MESSAGE_KEY_SHAKE_RESET);
  Tuple *t_backlight = dict_find(iter, MESSAGE_KEY_BACKLIGHT_COLOR);

  if (t_volume) {
    s_sound_volume = clamp_sound_volume(parse_int_tuple(t_volume, s_sound_volume));
    persist_write_int(PERSIST_KEY_SOUND_VOLUME, s_sound_volume);
  }
  if (t_shake) {
    s_shake_enabled = parse_bool_tuple(t_shake, s_shake_enabled);
    persist_write_int(PERSIST_KEY_SHAKE, s_shake_enabled ? 1 : 0);
  }
  if (t_backlight) {
    s_backlight_color = clamp_backlight_color(
        parse_int_tuple(t_backlight, s_backlight_color));
    persist_write_int(PERSIST_KEY_BACKLIGHT_COLOR, s_backlight_color);
    update_game_backlight();
  }
}

static void inbox_dropped(AppMessageResult reason, void *ctx) {
  (void)ctx;
  APP_LOG(APP_LOG_LEVEL_ERROR, "Settings inbox dropped: %d", (int)reason);
}

// ---------------------------------------------------------------------------
// Accel: shake to shuffle
// ---------------------------------------------------------------------------

static void shuffle_non_burning(void) {
  for (int row = 0; row < BOARD_ROWS; row++) {
    for (int col = 0; col < BOARD_COLS; col++) {
      if (is_submit_tile(row, col)) continue;
      if (s_board[row][col].status == STATUS_BURNING) continue;
      s_board[row][col].status = STATUS_DEFAULT;
      random_letter(row, col);
    }
  }
  clear_selection();
}

static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
  (void)axis;
  (void)direction;
  if (!s_shake_enabled) return;
  if (s_screen != SCREEN_PLAYING || s_input_locked) return;

  time_t now = time(NULL);
  if (now - s_last_shake < (SHAKE_DEBOUNCE_MS / 1000) + 1) return;
  s_last_shake = now;

  play_press_tone();
  shuffle_non_burning();

  // Mark below burning so the burnings drop, then run a drop animation.
  s_input_locked = true;
  if (!mark_below_burning()) {
    enter_game_over();
    return;
  }
  // Force at least one new burning tile next iteration.
  s_small_count = s_small_limit;
  if (any_deleted_tile()) {
    start_fire_drop_animation();
  } else {
    s_input_locked = false;
    mark_dirty();
  }
}

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_scene_layer = layer_create(bounds);
  layer_set_update_proc(s_scene_layer, scene_update_proc);
  layer_add_child(root, s_scene_layer);

  touch_service_subscribe(touch_handler, NULL);
  accel_tap_service_subscribe(accel_tap_handler);
}

static void window_unload(Window *window) {
  (void)window;
  cancel_anim_timer();
  if (s_popup_timer) {
    app_timer_cancel(s_popup_timer);
    s_popup_timer = NULL;
  }
  accel_tap_service_unsubscribe();
  touch_service_unsubscribe();
  if (s_scene_layer) {
    layer_destroy(s_scene_layer);
    s_scene_layer = NULL;
  }
}

// ---------------------------------------------------------------------------
// init / deinit
// ---------------------------------------------------------------------------

static void init(void) {
  srand((unsigned int)time(NULL));

  s_score = persist_exists(PERSIST_KEY_SCORE) ? persist_read_int(PERSIST_KEY_SCORE) : 0;
  s_rank = persist_exists(PERSIST_KEY_RANK) ? persist_read_int(PERSIST_KEY_RANK) : 0;
  s_shake_enabled = persist_exists(PERSIST_KEY_SHAKE) ? persist_read_int(PERSIST_KEY_SHAKE) : 0;
  s_sound_volume = persist_exists(PERSIST_KEY_SOUND_VOLUME)
                       ? clamp_sound_volume(persist_read_int(PERSIST_KEY_SOUND_VOLUME))
                       : DEFAULT_SOUND_VOLUME;
  s_backlight_color = persist_exists(PERSIST_KEY_BACKLIGHT_COLOR)
                          ? clamp_backlight_color(
                                persist_read_int(PERSIST_KEY_BACKLIGHT_COLOR))
                          : DEFAULT_BACKLIGHT_COLOR;

  reset_small_words();
  reset_big_words();

  if (load_field()) {
    s_title_variant = TITLE_CONTINUED;
  } else {
    start_field();
    s_title_variant = TITLE_NEW;
  }
  s_screen = SCREEN_TITLE;
  update_game_backlight();

  s_window = window_create();
  window_set_background_color(s_window, COLOR_MAIN_BACKGROUND);
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load,
    .unload = window_unload,
  });
  window_stack_push(s_window, true);

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_open(128, 64);
}

static void deinit(void) {
  app_message_deregister_callbacks();
  light_enable(false);
  light_set_system_color();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
