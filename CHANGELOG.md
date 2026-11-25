# Changelog

## 0.2.0 (2026-11-25)

- `screenshot` zooms: `x`, `y`, `width`, `height` capture a region of the
  latest screenshot closer, at up to the output's own resolution, to read
  small text. `output` picks a monitor.
- `launch` starts a program by its shell command, waits for a new window
  and names it.
- `click`, `move`, `drag`, `scroll`, `type`, `key` and `launch` take
  `screenshot: true` and answer with a screenshot of what they did.
- Tools carry MCP annotations (read-only, destructive, open-world hints)
  and titles. `drag`'s points are documented one by one, `click` checks
  `count` is 1 to 3, and buttons include `side` and `extra`.
- `wait` takes `window` and `scope` and says what kept changing; a window
  screenshot settles on that window. With Shady 0.4.1 a wait ignores bars
  and wallpapers.
- Connected to a Shady older than 0.4.0, tools say so instead of answering
  "unknown command", and `key` no longer reports a key that went nowhere
  as missing from the keymap.

## 0.1.0 (2026-11-12)

The first release: an MCP server on stdio for computer use on Shady 0.4.0,
with `screenshot`, `click`, `move`, `drag`, `scroll`, `type`, `key`, `wait`,
`windows`, `focus_window` and `shady_ipc`. Coordinates are pixels of the
latest screenshot.
