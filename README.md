# Letterfall

A word game for Pebble smartwatches (Emery platform). Tap adjacent letter tiles on a 5×5 grid to spell words, score points, and survive the falling fire.

## Gameplay

- Tap adjacent tiles (including diagonals) to build a word of at least 3 letters.
- Tap the arrow tile (bottom-right) to submit. A valid word scores points and removes those tiles; new tiles fall in from the top.
- Tap the last selected tile again to deselect it.
- Press Back to pause and return to the title screen. Your game is saved automatically.

**Burning tiles** (red) appear after you play too many short words. They fall one row each turn and cause game over if they reach the bottom.

**Gold tiles** (yellow) appear after you play enough long words. Each gold tile in your word adds a score multiplier.

**Letter costs:** Common letters (E, T, A, …) are worth 1 point each. Rarer letters (B, V, K, X, J, Q, Z) are worth 2–3 points. Gold tiles multiply the total cost of your word.

**Rank:** Your cumulative score unlocks titles from *Illiterate* all the way to *Absolute genius*.

## Settings

Open the Pebble app on your phone and tap the settings gear next to Letterfall.

| Setting | Default | Description |
|---|---|---|
| Sound Volume | 20 | 0 = silent, 100 = full volume |
| Reset Board on Shake | Off | Shake the watch to reshuffle non-burning tiles and advance fire one step |

## Building

Requires the [Rebble SDK](https://developer.rebble.io/developer.pebble.com/sdk/index.html) and Node.js.

```bash
npm install
npm run build          # build only
npm start              # build and launch in the Emery emulator
npm run clean          # clean build artifacts
```

The compiled `.pbw` is written to `build/letterfall-pebble.pbw`.

## Word list

Words are validated on-device via binary search against a sorted list of SHA-256 hash prefixes stored in `resources/data/hashes.bin`. No plaintext word list is shipped with the app.

The vocabulary is the public-domain [ENABLE](https://raw.githubusercontent.com/dolph/dictionary/master/enable1.txt) word list (~173k words of 3+ letters). To regenerate or swap in another list (one word per line):

```bash
python3 tools/build_hashes.py path/to/wordlist.txt
```

Each word costs 4 bytes; keep `hashes.bin` under the 1 MB per-platform appstore limit (~250k words). The SDK's 256 KB resource warning is outdated and can be ignored.

## Platform

Targets **Emery** (Pebble Time 2) — color display (200×228), hardware speaker, and touch input.
