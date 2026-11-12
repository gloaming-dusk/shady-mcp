# shady-mcp

Computer use on [Shady](https://github.com/gloaming-dusk/shady), the 3D
Wayland compositor, for AI agents: a [Model Context
Protocol](https://modelcontextprotocol.io) server that lets an agent see the
screen and work it with the keyboard and pointer, as a person does.

It is a small C program with no dependencies. It speaks MCP on stdin and
stdout and turns each tool call into a request on Shady's IPC socket.

| Tool | What it does |
|---|---|
| `screenshot` | The screen (or one `window`) as a PNG image, after it stops changing, scaled to 1280×800 at most |
| `click` | Click at a point: `button`, `count` 2 for a double click |
| `move` | Move the pointer, to hover |
| `drag` | Press, move over `duration` ms, release |
| `scroll` | Scroll by wheel clicks, at a point |
| `type` | Type text into the focused window: any characters, Hangul and emoji included |
| `key` | A key or shortcut: `Return`, `ctrl+l`, `Super+Return`; `action` `down`/`up` to hold |
| `wait` | Until nothing new has been drawn for `idle` ms |
| `windows` | The windows, their ids and where they are |
| `focus_window` | Focus a window, switching to its workspace |
| `shady_ipc` | Any other [IPC command](https://github.com/gloaming-dusk/shady/blob/main/docs/IPC_API.md): workspaces, `window.close`, `lua`, ... |

**Coordinates are pixels of the latest screenshot**, so an agent clicks
where it looked, whatever the output's size and scale, and in a window
screenshot from that window's corner. Before any screenshot they are
Shady's layout coordinates.

Shady 0.4.0 or later is needed: keys and text reach windows, and the
pointer and screenshot commands this uses are there.

## Install

With Nix:

```sh
nix profile install github:gloaming-dusk/shady-mcp
```

From a checkout:

```sh
meson setup build && ninja -C build && sudo ninja -C build install
```

## Use it

**Claude Code**, from inside the Shady session:

```sh
claude mcp add shady -- shady-mcp
```

Then ask for something on the desktop: "open a terminal and run uname",
"find the settings window and turn on dark mode".

**Any MCP client**: run `shady-mcp` as a stdio server:

```json
{ "mcpServers": { "shady": { "command": "shady-mcp" } } }
```

It finds Shady by `--socket PATH`, else `$SHADY_SOCKET`, else
`$XDG_RUNTIME_DIR/shady-$WAYLAND_DISPLAY.sock`, else the only Shady
running for the user. Started outside the session, it still finds the one
Shady there is.

### A desktop of its own

An agent can have a Shady of its own, headless, that is not your screen:

```sh
export XDG_RUNTIME_DIR=$(mktemp -d) WLR_BACKENDS=headless WLR_RENDERER=pixman
shady -s foot &
claude mcp add shady-headless -e XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR -- shady-mcp
```

## Safety

The agent gets what your keyboard and mouse have: it can type into any
window, the lock screen's password field included, and `shady_ipc` can run
Lua inside the compositor. Give it a desktop you would let it use, or a
headless one.

## Test

```sh
nix develop
meson setup build && ninja -C build
python3 -I tests/test-mcp.py
```

The test starts a headless Shady with a foot terminal, types a line into it
over MCP and checks that the shell read it, and that screenshots, clicks
and drags map coordinates right.

## License

Apache-2.0.
