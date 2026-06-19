# fcitx5-pinboard

A lightweight Fcitx5 clipboard history addon that adds **pinning** on top of the
built-in clipboard, rendered with the native candidate window — no Qt, no extra
process, no D-Bus.

## Features

- Clipboard text history with automatic de-duplication
- **Pin / Unpin** entries (kept above regular history, marked with 📌)
- Delete entries
- Number keys `1`–`9` to paste, `Esc` to dismiss
- Follows the built-in **clipboard** addon's configuration (trigger key and
  entry limit) — no separate settings to maintain
- Text history persisted across restarts

## How it works

A single native Fcitx5 addon (`.so`) running inside the Fcitx5 process. The UI
uses Fcitx5's `CandidateList`; pin/delete are `CandidateAction`s. Clipboard
monitoring reuses the built-in `clipboard` module. The trigger key is consumed in
the `PreInputMethod` phase so it does not collide with the built-in clipboard
panel.

## Configuration

There is nothing to configure here. It mirrors the built-in **Clipboard** addon:

- **Trigger key** — Clipboard addon's *Trigger Key* (default `Control+;`)
- **History / pin limit** — Clipboard addon's *Number of entries* (default 5)

Change them in `fcitx5-config-qt` → Addons → Clipboard.

## Install

### From AUR

```sh
yay -S fcitx5-pinboard
```

### From source

```sh
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

Then restart Fcitx5: `fcitx5 -r -d`.

## Usage

In any input field press the trigger key (`Control+;` by default):

| Key       | Action                         |
| --------- | ------------------------------ |
| `1`–`9`   | Paste the numbered entry       |
| `Enter`   | Paste the highlighted entry    |
| `p`       | Pin / unpin the highlighted    |
| `d`       | Delete the highlighted         |
| `Esc`     | Close                          |

## License

LGPL-2.1-or-later. See [LICENSE](LICENSE).
