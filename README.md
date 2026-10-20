# achivmentsd

A small offline proxy for RetroAchievements, written in C on OpenWrt's `uloop`.
RetroArch talks to it instead of retroachievements.org. While the server is
reachable, requests pass through untouched and successful responses are cached.
While it is not, the daemon answers from the cache and queues unlocks and
leaderboard entries, then replays them, backdated, once the server is back.

## Build

Requires CMake ≥ 3.20, a C11 compiler, `git` (used to apply a patch to
uclient), and the system `libsqlite3` with its headers. libubox, uclient,
ustream-ssl, mbedTLS 3.6 and json-c are downloaded at configure time, pinned by
hash, and linked statically. LTO is used whenever the toolchain supports it.

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure   # integration test needs python3
```

Static Linux binaries with zig as the cross compiler. SQLite comes from the
target's sysroot, which `CMAKE_FIND_ROOT_PATH` points at:

```sh
cmake -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/zig.cmake \
      -DZIG_TARGET=aarch64-linux-musl -DCMAKE_FIND_ROOT_PATH=/path/to/sysroot \
      -DACHD_STATIC=ON
cmake --build build-aarch64
```

`arm-linux-musleabihf` and `x86_64-linux-musl` work the same way. Without
SQLite, a static binary is about 700 KB; a stock static `libsqlite3.a` adds about
750 KB more. To build against libraries already installed (OpenWrt SDK), pass
`-DACHD_SYSTEM_DEPS=ON`. In that mode the TLS backend is loaded through
`libustream-ssl.so`.

## Run

```sh
achivmentsd -c /etc/achivmentsd.json -S
```

| Option | Meaning |
|---|---|
| `-c FILE` | JSON config; `/etc/achivmentsd.json` by default, ignored if missing |
| `-s DIR` | state directory |
| `-l HOST` / `-p PORT` | listen address and port |
| `-u URL` | upstream base URL |
| `-S` | log to syslog instead of stderr |
| `-v` | debug logging |

The daemon stays in the foreground, which suits procd and other init systems.
It never polls the network. Each emulator request is tried upstream first, and
the result decides whether the daemon is online. Queued awards and background
refresh wait for a request to succeed. `SIGUSR1` marks the upstream reachable
and replays the queue at once, which makes it useful from a network-up hook.
`SIGHUP` refreshes cached data (see below).

Point RetroArch at it in `retroarch.cfg`:

```
cheevos_enable = "true"
cheevos_custom_host = "127.0.0.1:8080"
```

### Config

```json
{
  "proxy_host": "127.0.0.1",
  "proxy_port": 8080,
  "upstream_host": "https://retroachievements.org",
  "state_dir": "/var/lib/achivmentsd",
  "ca_file": "/etc/ssl/certs/ca-certificates.crt",
  "insecure_tls": false,
  "retroarch_cfg": "/storage/.config/retroarch/retroarch.cfg"
}
```

All keys are optional.

- **`ca_file`**: when unset, the daemon uses the first common CA bundle path that
  exists. Without a CA bundle, HTTPS requests fail. They only succeed without one
  if `insecure_tls` is set.
- **`retroarch_cfg`**: lets replays use `cheevos_token` before the emulator has
  logged in through the proxy.
- **Remote clients**: requests not from loopback are refused, whatever address
  the daemon listens on.

## Behaviour

| Request | Upstream reachable | Unreachable |
|---|---|---|
| `login`, `login2` | relayed; cached on success | cached response |
| `gameid`, `patch`, `achievementsets`, others | relayed; cached on success | cached response; a `gameid` miss returns `GameID: 0` |
| `startsession` | relayed; its unlock lists are cached | built from cached unlocks plus queued awards |
| `unlocks` | relayed and cached | built the same way |
| `ping`, `postactivity` | relayed | `{"Success":true}` |
| `awardachievement`, `submitlbentry` | relayed | queued; success returned |

Hardcore requests get no special handling. The `h` value is queued and replayed
as sent, and the server decides what counts. Relayed requests keep the
emulator's User-Agent. Replays append `achivmentsd/0.1`, and requests the daemon
makes for itself send only `achivmentsd/0.1`. That tag lets RetroAchievements
apply its policy for offline-submission clients.

**Replay.**
- Queued entries go out in order, with the current API token.
- `o` is set to the emulator's own offset plus the time spent in the queue,
  capped at 14 days, and `v` is recomputed to match.
- If the clock was earlier than the build date at queue time (devices without
  an RTC), the entry replays without a backdate.
- Entries the server rejects for good stay in the `awards` table, marked failed,
  with the server's error.
- After a 429, background work pauses for at least 10 minutes.

**Refresh.** On `SIGHUP`, achievement definitions older than a day are
re-fetched for games started in the last 7 days, along with their unlocks. While
offline, the fetches wait until a request reaches upstream.

### State

Everything lives in `<state_dir>/achivmentsd.db`:

| Table | Contents |
|---|---|
| `cache` | RA responses, keyed by action, scope (game ID or ROM hash), user and hardcore flag, with the game and the request without its token, so refresh can resend it |
| `achievements` | achievement → game, learned from `patch` and `achievementsets` |
| `played` | game → last session start |
| `awards` | queued submissions in order (`status` 0 = pending, 1 = failed, with `last_error`) |

To see what is waiting: `sqlite3 achivmentsd.db 'SELECT * FROM awards'`.

## Known limitations

- **Blocking DNS and connect.** uclient resolves names and opens connections
  synchronously. While offline, every emulator request pays for a failed
  attempt. With no interface up, that fails at once. On a link with no route
  out, it costs up to 3 s for connect, or the resolver's timeout for DNS.
- **Patched uclient.** uclient always sends request bodies chunked, and
  RetroAchievements' backend drops chunked bodies. The build applies
  `cmake/patches/uclient-content-length.patch`, which sends the body as-is when
  `Content-Length` is set. A system uclient (`ACHD_SYSTEM_DEPS=ON`) needs the
  same patch.
- **No image caching.** Badge images load straight from RetroAchievements' media
  host, so they do not show while offline.
- **No ROM hashing.** Games must have been launched online at least once before
  they work offline.
