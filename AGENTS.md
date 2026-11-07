# Notes for coding agents

Dune Dynasty fork (C, Allegro 5), built on Windows with MSYS2 UCRT64.

## Branch

Work and commit on `master` only.  Don't create or switch branches unless asked.

## Building

```sh
export PATH=/d/development/msys64/ucrt64/bin:$PATH
ninja                      # from the repo root; output is dist/dunedynasty.exe
```

## Running the game for tests: always headless

Every time the game is started for a test, a check or a debug run, pass
`--headless --windowed`.  Headless hides the window (created off-screen,
frameless, never taking the focus) and mutes the sound, so the user can keep
playing or working while tests run.  Never start the game without `--headless`
unless the user asks to see it.

- Regression suite: `.\scripts\regression-tests.ps1` (headless by default).
  `-Only <name>` runs matching tests, `-Slow` adds the slow ones, `-Build`
  builds first.  Don't pass `-Visible`.  It force-closes any running
  `dunedynasty.exe` before each test, so first check the user isn't playing
  (`Get-Process dunedynasty`; one started without `--headless` is theirs).
- If the build fails with "cannot open output file ... Permission denied", the
  user has the game open.  Don't close it; tell them, and build when it's closed.
- A single test by hand:

  ```sh
  timeout 200 ./dist/dunedynasty.exe --headless --windowed --house atreides --mission 5 --test-worm
  ```

  Each `--test-*` switch plays the scenario by itself, logs to a `*_test.log`
  in the repo root, and quits.  Always wrap the run in `timeout`, so that a
  hang cannot block.
- Screenshots (F12, or `VideoA5_CaptureScreenshot()` in a test) still work
  headless; they go to `%APPDATA%\Dune Dynasty`.
- If you add a temporary debugging hook to the code, remove it and rebuild
  before finishing.
- The tracked `*_test.log` files get rewritten by test runs; restore them with
  `git checkout -- <file>` rather than committing them.
