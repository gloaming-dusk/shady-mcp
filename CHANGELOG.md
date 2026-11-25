# Changelog

## 0.2.1 (2026-11-25)

- A region `screenshot` asks Shady to upscale it, so with a Shady that can
  (0.5.3 or later) small text comes back up to four times larger instead of
  at the screen's own size.
- `launch` learns the command's exit status: a command that is not there
  (127), cannot run (126) or fails before it has a window is an error at
  once, instead of a wait for a window that will not come. One that
  finishes well without a window says so after a second.
- shady-mcp stopped by SIGTERM, SIGINT or SIGHUP leaves no screenshot
  directory behind.
- With no Shady socket but a Wayland display in `XDG_RUNTIME_DIR`, the
  error says Shady's IPC may be off, and why it might be.
- The test keeps the user's config out of its headless Shady.
- The tests run against Shady 0.5.3.

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
