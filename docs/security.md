# Securing the runtime HTTP API

The runtime exposes a REST surface (`/v0/*`) including control actions
(`POST /v0/call`, `POST /v0/mode`, `POST /v0/parameters`). By default it listens
on loopback only, without authentication, and it **must be authenticated before
any non-localhost exposure** (e.g. the bioreactor target under systemd).

Unauthenticated loopback is not private to you: any local process can call the
control endpoints. Turn auth on wherever another user shares the host.

## A browser on the host

A web page open in a browser on the runtime's host can send requests to
`127.0.0.1`. A browser sends a `text/plain` or bodyless `POST` as a CORS
"simple" request, with no preflight, so until #540 was fixed such a page could
clear the e-stop latch, change the mode and call devices, and with the old default
`cors_allowed_origins: ["*"]` read every response (#540). A page can also
re-point its own name at `127.0.0.1` (DNS rebinding) and send requests the
browser treats as same-origin.

The runtime now refuses these before routing:

- **Origin.** A `POST` to `/v0/` that carries an `Origin` header is refused
  with `403` unless that origin is in `http.cors_allowed_origins`. A browser
  sends `Origin` on every cross-origin `POST`; `curl`, scripts and the
  workbench's proxy send none and are not affected.
- **Content type.** A `POST` with a body must say `Content-Type:
  application/json`, or it is refused with `415`, so no request with a body
  can avoid a preflight.
- **Host.** With auth off, the `Host` header must be `localhost`, `127.0.0.1`,
  `::1` or the bind address, or the request is refused with `403`. That stops
  a rebound page. `http.allow_insecure_bind` turns this check off, because the
  names a network client uses can't be known in advance.

`http.cors_allowed_origins` defaults to none. List the origins of any browser
UI that calls the runtime directly, or `*` to allow any. The workbench reaches
the runtime through its own server, so it needs no entry.

With auth on and `http.auth_exempt_loopback: true`, a rebound page's requests
come from loopback and skip the token. The origin and content-type checks still
refuse its `POST`s, but it can read `GET` responses. Leave
`auth_exempt_loopback` off where a browser shares the host.

## Defaults

| Setting | Default | Effect |
| --- | --- | --- |
| `http.bind` | `127.0.0.1` | loopback only — not reachable from the network |
| `http.auth_enabled` | `false` | no token required |
| `http.auth_exempt_loopback` | `false` | with auth on, requests from this host need the token too |
| `http.cors_allowed_origins` | none | no browser origin may call the API; `*` allows any |
| `http.tls_cert_path` / `http.tls_key_path` | unset | plain HTTP |

With the defaults the API is reachable only from the same host, so local
development needs no configuration.

## Safety gate

The runtime **refuses to start** when `http.bind` is non-loopback and
`http.auth_enabled` is `false` — you cannot accidentally expose an
unauthenticated API to the network. Override only for a deliberately-trusted
network with `http.allow_insecure_bind: true` (not recommended).

`--check-config <file>` reports the same error without starting the runtime.

## Authentication (Bearer token)

<!-- validate: runtime-config fragment -->

```yaml
http:
  bind: 0.0.0.0
  auth_enabled: true
  auth_token: "<a long random secret>"   # or set the ANOLIS_API_TOKEN env var
```

Prefer the **`ANOLIS_API_TOKEN`** environment variable over putting the secret
in the YAML file. Generate a token with e.g. `openssl rand -hex 32`.

Clients send it as a Bearer header:

```bash
curl -H "Authorization: Bearer $ANOLIS_API_TOKEN" https://host:8080/v0/state
```

The token is compared in constant time and is never written to the logs.

With auth on, every client sends the token, including one on the same host.
Local scripts can read it from root-only storage and pass the header on stdin,
which keeps it off the command line (`ps` shows a process's arguments):

```bash
sudo sed -n 's/^ANOLIS_API_TOKEN=/Authorization: Bearer /p' /etc/anolis/runtime.env \
  | curl -s -H @- http://127.0.0.1:8080/v0/runtime/status
```

`install.sh`'s health check sends the token the same way, to
`http://localhost:<port>`, and curl tries `[::1]:<port>` before
`127.0.0.1:<port>`. The runtime listens on IPv4 only, so a local process that
holds `[::1]:<port>` at any time, or `127.0.0.1:<port>` before the runtime
starts, receives the token from the probe.

### The loopback exemption

`http.auth_exempt_loopback: true` lets any request from the runtime's own host
(`127.*`, `::1`) skip the token. It decides by source address alone, so it
trusts:

- every process on the host, under any user account;
- every local service that forwards requests from other machines, such as a
  reverse proxy or the workbench's Operate proxy. Whoever can reach such a
  service reaches the full runtime API, control endpoints included, without the
  token.

It is off by default. Up to 0.1.47 it defaulted to `true`, which left a host
running a forwarding service open to that service's clients. A workbench on the
device needs anolis-workbench 0.18.0 or later, which sends the token; upgrade it
before the runtime, or its Operate proxy gets `401`. Turn it on only on
a host where no local service forwards network requests to the runtime and every
local user is trusted to operate the hardware. The runtime logs a warning at
startup whenever it is on with auth enabled.

## TLS

A Bearer token sent over plain HTTP is sniffable on the network, so serve over
TLS (or terminate TLS at a reverse proxy) whenever the API is network-exposed.
The runtime starts an HTTPS server when both paths are set:

<!-- validate: runtime-config fragment -->

```yaml
http:
  tls_cert_path: /etc/anolis/tls/cert.pem
  tls_key_path: /etc/anolis/tls/key.pem
```

Generate a self-signed certificate for testing / a trusted LAN:

```bash
sudo mkdir -p /etc/anolis/tls
sudo openssl req -x509 -newkey rsa:4096 -nodes \
  -keyout /etc/anolis/tls/key.pem -out /etc/anolis/tls/cert.pem \
  -days 365 -subj "/CN=anolis-runtime"
sudo chown root:anolis /etc/anolis/tls/key.pem
sudo chmod 640 /etc/anolis/tls/key.pem
```

The runtime reads the key itself, as its service user, so the key must be
readable by that user. The commands above are for the `anolis` user and group
that `install.sh` creates.

For a public hostname, use a CA-issued certificate (e.g. Let's Encrypt) instead.

## systemd deployment

Keep the secret out of the unit file and the config; pass it via the
environment (`EnvironmentFile`, mode `600`):

```ini
# /etc/anolis/anolis.env   (chmod 600, root-owned)
ANOLIS_API_TOKEN=<your-token>
```

```ini
# /etc/systemd/system/anolis-runtime.service
[Unit]
Description=Anolis runtime
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/opt/anolis/bin/anolis-runtime --config /etc/anolis/anolis-runtime.yaml
EnvironmentFile=/etc/anolis/anolis.env
DynamicUser=yes
StateDirectory=anolis
WorkingDirectory=/var/lib/anolis
NoNewPrivileges=yes
ProtectSystem=strict
ReadOnlyPaths=/etc/anolis
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

This is a minimal unit for running the runtime by hand. Without
`runtime.data_dir`, the runtime keeps its data in `anolis-data` beside the
config file, which this unit mounts read-only, so set `runtime.data_dir:
/var/lib/anolis` in the config; without a writable data directory the run
registry fails at startup and `/v0/runs` answers `503`. `install.sh` generates its
own (`User=anolis`, see [system surfaces](system-surfaces.md)). With
`DynamicUser=yes` the runtime and its provider children have no stable user, so a
provider that opens a device node needs a real user that the host's device groups
include.

## Reverse proxy (alternative)

You may instead keep the runtime on `127.0.0.1` and front it with a reverse
proxy (nginx/Caddy) that terminates TLS and performs auth/SSO. In that setup
leave `http.bind: 127.0.0.1` so only the proxy can reach the runtime.

## Observability

`install.sh --with-observability` installs InfluxDB and Grafana on the device.
Each opens one port:

| Port | Service | Bind | Authentication | Who needs it |
| --- | --- | --- | --- | --- |
| 8086 | InfluxDB | `127.0.0.1`, always | API token. The runtime writes with a write-scoped token, and Grafana and anolis-telemetry-export read with a read-scoped one; the admin token is in root's influx CLI config and, as `INFLUXDB_ADMIN_TOKEN`, in `/etc/anolis/observability.env` (mode 0600, root). | The runtime and Grafana, plus anolis-telemetry-export when installed with `--with-telemetry-export`, all on the device. |
| 3000 | Grafana | `127.0.0.1`; `0.0.0.0` with `--with-grafana-network` | Anonymous **Viewer**: anyone who reaches the port sees the dashboards and the data behind them without logging in. Editing needs the `admin` login, whose first-boot password is in `/etc/anolis/observability.env`. | The operator, from a browser. |

By default reach Grafana over an ssh tunnel, which the install summary prints:
`ssh -L 3000:127.0.0.1:3000 <user>@<host>`, then open `http://localhost:3000`.
`--with-grafana-network` opens Grafana, and only Grafana, to the network; every
host that can reach the device can then read the dashboards. A later
`--with-observability` install without the flag binds it to `127.0.0.1` again,
and its summary says so. If that install's observability step does not finish,
the bind is left as it was, and the summary says the Grafana bind is unchanged.

The install configures no host firewall.
