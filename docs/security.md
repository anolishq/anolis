# Securing the runtime HTTP API

The runtime exposes a REST surface (`/v0/*`) including control actions
(`POST /v0/call`, `POST /v0/mode`, `POST /v0/parameters`). It is **safe for
localhost/dev by default** and **must be authenticated before any non-localhost
exposure** (e.g. the bioreactor target under systemd).

## Defaults

| Setting | Default | Effect |
| --- | --- | --- |
| `http.bind` | `127.0.0.1` | loopback only — not reachable from the network |
| `http.auth_enabled` | `false` | no token required |
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

```yaml
http:
  bind: 0.0.0.0
  auth_enabled: true
  auth_token: "<a long random secret>"   # or set the ANOLIS_API_TOKEN env var
  auth_exempt_loopback: true             # local tools skip auth (default)
```

Prefer the **`ANOLIS_API_TOKEN`** environment variable over putting the secret
in the YAML file. Generate a token with e.g. `openssl rand -hex 32`.

Clients send it as a Bearer header:

```bash
curl -H "Authorization: Bearer $ANOLIS_API_TOKEN" https://host:8080/v0/state
```

The token is compared in constant time and is never written to the logs.

## TLS

A Bearer token sent over plain HTTP is sniffable on the network, so serve over
TLS (or terminate TLS at a reverse proxy) whenever the API is network-exposed.
The runtime starts an HTTPS server when both paths are set:

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
sudo chmod 600 /etc/anolis/tls/key.pem
```

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
ExecStart=/usr/local/bin/anolis-runtime --config /etc/anolis/anolis-runtime.yaml
EnvironmentFile=/etc/anolis/anolis.env
DynamicUser=yes
NoNewPrivileges=yes
ProtectSystem=strict
ReadOnlyPaths=/etc/anolis
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

## Reverse proxy (alternative)

You may instead keep the runtime on `127.0.0.1` and front it with a reverse
proxy (nginx/Caddy) that terminates TLS and performs auth/SSO. In that setup
leave `http.bind: 127.0.0.1` so only the proxy can reach the runtime.
