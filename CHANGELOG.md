# Changelog

## Unreleased

- Connected to a Shady older than 0.4.0, tools say so instead of answering
  "unknown command", and `key` no longer reports a key that went nowhere
  as missing from the keymap.

## 0.1.0 (2026-11-12)

The first release: an MCP server on stdio for computer use on Shady 0.4.0,
with `screenshot`, `click`, `move`, `drag`, `scroll`, `type`, `key`, `wait`,
`windows`, `focus_window` and `shady_ipc`. Coordinates are pixels of the
latest screenshot.
