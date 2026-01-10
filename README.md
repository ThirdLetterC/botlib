# BOTLIB - Telegram C bot framework

Botlib is a small C framework for building Telegram bots. It provides a threaded event loop that delivers every matching update to your callback together with convenience helpers for SQLite, JSON, and dynamic strings.

## Features

- Telegram Bot API helpers to send, edit, and download messages from a per-update worker thread.
- `BotRequest` parsing: split argv/argc, message type, mentions, file metadata (voice OGG), and targeting info.
- SQLite helpers and a ready-to-use key/value schema (`TB_CREATE_KV_STORE`) plus convenience wrappers like `kvSet`/`kvGet`.
- JSON selection with `json_select` (Parson) and SDS dynamic strings baked in.
- HTTP GET wrappers on top of libcurl.
- Minimal dependencies: `libcurl`, `libsqlite3`, `pthread`, and a C23-capable compiler.

## Quick start

1. Install dependencies (Debian/Ubuntu): `sudo apt install build-essential libcurl4-openssl-dev libsqlite3-dev just` (or equivalent packages on your distro).
2. Drop your Telegram bot token in `apikey.txt` (one line), or pass it via `--apikey <token>` when running.
3. Edit `mybot.c` if you want to tweak triggers, responses, or database setup; the example already seeds the key/value store schema.
4. Build: `just build` (release-style) or `just build-sanitize` (ASan/UBSan/leak). If you prefer Zig, `zig build` produces `zig-out/bin/mybot` and accepts `-Dsanitize=true`.
5. Run the bot from the repo root: `./mybot --debug` for verbose output, or add `--dbfile <path>` to pick a different SQLite location.
6. Add the bot to your chat/channel and promote it to administrator so it can read group messages; private chats work without admin rights.

The default database file is `mybot.sqlite` in the working directory.

## Runtime options

- `--apikey <token>`: Telegram Bot API key (overrides `apikey.txt`).
- `--dbfile <file>`: Path to the SQLite database (default: `./mybot.sqlite`).
- `--debug`: Increases debug level each time it is passed and implies verbose logging of Telegram responses.
- `--verbose`: Print basic request handling traces without full debug output.

## API hints

- Entry point: `startBot(create_sql, argc, argv, flags, handleRequest, cron, triggers);`
- Your request handler receives `sqlite3 *dbhandle` and `BotRequest *br`; free the request with `freeBotRequest()` if you allocate your own.
- Respond with `botSendMessage`, `botSendMessageAndGetInfo`, or `botEditMessageText`; download voice messages with `botGetFile`.
- Use `kvSet`/`kvGet`/`kvDel` for the bundled key/value store or the `sql*` helpers for raw queries.
- `json_select` provides a simple formatted selector for navigating JSON replies.
- See `mybot.c` for a complete, runnable example of triggers, storage, and message editing.

## Notes

- The SQLite and JSON helpers are derived from the Stonky project; check that codebase for deeper examples.
- Additional documentation for the Telegram helper surface is planned; the example bot exercises the core flows today.
