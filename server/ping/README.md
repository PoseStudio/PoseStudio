# The install-ping endpoint (`https://www.posestudio.io/ping`)

The desktop app sends one small, signed request each time it starts (see
[`src/core/installping.h`](../../src/core/installping.h)). This folder is the server side that
receives it: the PHP handler, the MySQL schema, and a script that sends a correctly signed test
ping. The client and server are kept in one repository so the contract can't drift.

## What the client sends

```
POST /ping HTTP/1.1
Content-Type: application/json
User-Agent: PoseStudio/0.3.12 (Windows 11 Version 24H2; x86_64)
X-PoseStudio-Signature: <lowercase hex HMAC-SHA256 over the raw body, keyed with the shared secret>

{"arch":"x86_64","install":"c26adb1e-5641-4dc1-a95b-96984236253e","kind":"installer","os":"windows","os_version":"Windows 11 Version 24H2","qt":"6.10.3","ts":1788994219,"version":"0.3.12"}
```

- `install` is a random UUID the app generates once and keeps in its preferences. It is the only
  identity the server ever sees; it is not tied to a person, machine, or account.
- The signature key is the `POSESTUDIO_PING_KEY` GitHub Actions secret the release build is
  compiled with. Local builds and forks have no key and never ping.
- Every response is `204 No Content`, whether or not the ping was accepted.

## Deploy

1. **Tables**: run [`schema.sql`](schema.sql) once against the site's database:
   `mysql -u <user> -p <database> < schema.sql`
2. **Key**: in `../includes/posestudio_inc.php` (outside the web root, next to `dbConnect()` and
   `getUserIP()`), add the same value that is stored as the `POSESTUDIO_PING_KEY` repository secret:
   ```php
   define('POSESTUDIO_PING_KEY', 'paste-the-hex-secret-here');
   ```
   Generate a fresh one with `openssl rand -hex 32`. Keep it hex: it travels through a CMake compile
   definition on the client side. If the constant is missing, the handler rejects everything
   (`no_key_configured`) rather than counting unsigned pings.
3. **Handler**: make [`ping.php`](ping.php) the script that serves `/ping` (replace the current
   one's contents). It includes `../includes/posestudio_inc.php` exactly as the existing script does.
4. **Proxy check**: if the site sits behind Cloudflare or another proxy, `getUserIP()` must return
   the real client address (`CF-Connecting-IP` / `X-Forwarded-For`), or every visitor hashes to the
   proxy's address and the per-address quota bites real users.

## Test it

From this folder, with the key you configured:

```powershell
.\send-test-ping.ps1 -Key <key>                     # -> one new row in installs, one in ping_log
.\send-test-ping.ps1 -Key <key> -Install <that id>  # -> launches becomes 2 on the same row
.\send-test-ping.ps1 -Key <key> -Tamper             # -> nothing counted; a bad_signature row in ping_rejects
```

A plain browser visit to `/ping` adds a `method` row to `ping_rejects` and nothing else. That is
the "people hitting the endpoint directly" case: filed, never counted.

To ping from a development build of the app instead, configure it with the same key and build.
The key is the build's only switch: any build that has one pings, Debug or Release, and any build without
one never does. The setting lives in the CMake cache, so IDE builds pick it up too.

```powershell
cmake -B build -DPOSESTUDIO_PING_KEY=<key>
cmake --build build --config Release
build\Release\PoseStudio.exe      # a few seconds after the window shows, a row arrives
```

The app logs what it did (`[ping] delivered, HTTP 204`, or the reason it skipped) on stderr.
Besides a missing key, three things keep a launch from pinging: `POSESTUDIO_NO_PING` in the
environment, the switch in Preferences → General, and a run that a script or a probe drives
(`POSESTUDIO_IK_SCRIPT`, `POSESTUDIO_IK_BENCH`, `POSESTUDIO_MANUAL_SHOT`,
`POSESTUDIO_DUMP_SKELETON` — `src/core/runmode.h`), which is nobody's launch.
Reset with `cmake -B build -DPOSESTUDIO_PING_KEY=` when you no longer want local launches counted.

**If you build through VS Code's CMake Tools**, put the key in the workspace's git-ignored
`.vscode/settings.json` instead of relying on the cache — the extension wipes and regenerates the
cache whenever the kit changes, which silently produces a keyless build (the app then logs
`[ping] no signing key in this build; skipped`):

```json
"cmake.configureArgs": [ "-DPOSESTUDIO_PING_KEY=<key>" ]
```

## Troubleshooting

Nothing shows up and you can't tell why? The endpoint is deliberately silent, so switch on debug
mode for a moment: add `define('POSESTUDIO_PING_DEBUG', true);` next to the key in the include.
Every response then says what happened — `400 rejected: <reason>`, `200 accepted: <install id>`,
or `500 error: <message>` (typically a missing table: run `schema.sql`) — and PHP errors are
displayed instead of logged. A rejection that could not even be FILED (the `ping_rejects` insert
failed) reports that database error too, so an empty set of tables always has an explanation.
`send-test-ping.ps1` prints that body. Remove the define afterwards.

If all three tables stay empty after the app has pinged, the request reached the handler (it
answers 204) but the database writes are failing: almost always the tables were created in a
different database than the one `dbConnect()` opens, or that user lacks INSERT on the new tables.
Debug mode names the database in its error text (`Table 'dbname.installs' doesn't exist`).

Without debug mode, look in two places: `ping_rejects` (every ignored request, with its reason)
and the web server's error log (`/var/log/apache2/error.log` on Ubuntu), where the handler writes
lines starting with `ping:` and PHP reports any fatal error.

Two things that look like failures but aren't: a browser visit shows a blank page (that is the
204), and the app's own development builds never ping unless built with a key — the app then
logs `[ping] no signing key in this build; skipped` (see "Test it").

## What gets rejected, and why

| Reason (`ping_rejects.reason`) | Trigger |
|---|---|
| `method` | anything but POST: browser visits, crawlers, HEAD probes |
| `content_type` | body not `application/json` (a form post, for instance) |
| `empty_body`, `too_large` | no body, or over 1 KB |
| `no_key_configured` | the server has no `POSESTUDIO_PING_KEY`; fail closed |
| `no_signature`, `bad_signature` | header missing / malformed, or the HMAC doesn't verify |
| `bad_json`, `bad_field:<name>` | body isn't the documented shape (a hand-crafted or tampered request; official builds always pass) |
| `unknown_version` | only when `PING_ALLOWED_VERSIONS` in `ping.php` is non-empty |
| `stale_ts` | client clock more than 6 h from the server's: replay protection with room for wrong clocks |
| `ip_new_install_limit` | more than 25 NEW install ids from one address in a day (a leaked key generating ids) |
| `install_rate_limit` | more than 200 pings from one install id in a day (a runaway script) |

The quotas and windows are the `PING_*` constants at the top of `ping.php`.

## Reading the numbers

```sql
-- Active installs: seen in the last 30 days
SELECT COUNT(*) FROM installs WHERE last_seen >= NOW() - INTERVAL 30 DAY;

-- Confirmed active installs: pinged on at least two different days (filters one-off forgeries
-- and install-then-uninstall noise)
SELECT COUNT(*) FROM installs WHERE last_seen >= NOW() - INTERVAL 30 DAY AND ping_days >= 2;

-- Version breakdown of active installs
SELECT version, COUNT(*) AS installs FROM installs
WHERE last_seen >= NOW() - INTERVAL 30 DAY GROUP BY version ORDER BY installs DESC;

-- Platform breakdown
SELECT os, arch, kind, COUNT(*) AS installs FROM installs
WHERE last_seen >= NOW() - INTERVAL 30 DAY GROUP BY os, arch, kind ORDER BY installs DESC;

-- New installs per day, last 30 days
SELECT DATE(first_seen) AS day, COUNT(*) AS new_installs FROM installs
WHERE first_seen >= NOW() - INTERVAL 30 DAY GROUP BY day ORDER BY day;

-- Launches per day (all installs), last 30 days
SELECT DATE(received_at) AS day, COUNT(*) AS launches, COUNT(DISTINCT install_id) AS installs
FROM ping_log WHERE received_at >= NOW() - INTERVAL 30 DAY GROUP BY day ORDER BY day;

-- Is anyone poking the endpoint? Rejections by reason, last 7 days
SELECT reason, COUNT(*) AS hits FROM ping_rejects
WHERE received_at >= NOW() - INTERVAL 7 DAY GROUP BY reason ORDER BY hits DESC;
```

## Maintenance

- **Pruning** (a cron job or an occasional run; `installs` is the permanent table):
  ```sql
  DELETE FROM ping_log     WHERE received_at < NOW() - INTERVAL 90 DAY;
  DELETE FROM ping_rejects WHERE received_at < NOW() - INTERVAL 30 DAY;
  ```
- **Rotating the key**: change the GitHub secret and the PHP constant together, then cut a
  release. Builds compiled with the old key keep signing with it and are rejected from then on;
  that is the intended way to retire a version from the count.
- **Privacy**: the tables hold a random install id, version and platform strings, timestamps, and
  a daily-rotating keyed hash of the address. No raw addresses, names, paths, or usage data,
  matching what the app discloses in Preferences → General and in the installation guide.
