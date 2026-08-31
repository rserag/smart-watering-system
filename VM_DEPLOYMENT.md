# Padval VM deployment

The production-specific stack is defined by `compose.vm.yml`. It keeps PostgreSQL, the backend, and the frontend on the private Compose network and publishes only the HTTP gateway on `127.0.0.1:8008`.

Host Nginx terminates public TLS using the existing `sibex-wildcard` certificate. The site template is `deploy/nginx-host-watering.sibex.zip.conf`.

## Current VM status

Verified on 2026-08-27:

- Immutable deployment control is at `/opt/watering-system`; the protected application environment
  and pre-CI recovery stack remain at `/home/c/watering-system`.
- All four containers are healthy.
- The only host binding is `127.0.0.1:8008`.
- PostgreSQL persists in `watering-system_watering_db`.
- Docker logs are limited to three 10 MB files per service.
- Backend health, frontend delivery, database access, valid device WebSocket authentication, invalid device-token rejection, and unauthenticated dashboard rejection passed.
- `/etc/nginx/sites-available/watering.sibex.zip` is enabled and validates.
- `watering.sibex.zip` resolves through a proxied Cloudflare A record and is publicly healthy.
- The stack runs in `AUTH_MODE=google`; the authorization redirect and exact production callback URL were verified.
- A root-only daily logical backup and reusable isolated restore drill are installed on the VM.

The production callback is `https://watering.sibex.zip/auth/google/callback`. Never expose the OAuth values or other contents of the VM environment file.

## Continuous deployment

GitHub Actions builds the backend, frontend, and gateway on GitHub-hosted runners. Images are
published to public GHCR packages with a `sha-<commit>` tag and build-provenance attestation.
Production uses the immutable image digests produced by that build rather than rebuilding source
on the VM.

The `production` GitHub environment is restricted to `main`, requires approval, and contains only
the deployment SSH key and trusted host-key entry. Application and database secrets remain solely
in `/home/c/watering-system/.env` on the VM.

The restricted `watering-deploy` SSH identity can invoke only the root-owned deployment wrapper.
The wrapper validates the Git revision and all three GHCR digests, serializes deployments, pulls
only the watering images, starts the existing `watering-system` Compose project, and checks both
the backend and frontend through `127.0.0.1:8008`.

The root-owned deployment control files are:

```text
/opt/watering-system/compose.yml
/opt/watering-system/state/current.env
/opt/watering-system/state/previous.env
/opt/watering-system/releases/<git-revision>.env
/usr/local/sbin/watering-deploy
/usr/local/sbin/watering-deploy-ssh
```

To release normally, merge a pull request into `main` and approve the pending production job.
The workflow does not enable the public Nginx site and does not modify TLS configuration.

## Manual inspection

On the VM, inspect the current immutable release without displaying either environment file:

```sh
sudo docker compose \
  --project-name watering-system \
  --env-file /home/c/watering-system/.env \
  --env-file /opt/watering-system/state/current.env \
  --file /opt/watering-system/compose.yml \
  ps
curl --fail http://127.0.0.1:8008/health
```

The remote `.env` must be mode 600. Do not display its values. Public cutover requires `AUTH_MODE=google`, non-empty Google OAuth values, an allowed-email list, and this registered callback:

```text
https://watering.sibex.zip/auth/google/callback
```

Telegram is delivered directly by the ESP32 and does not require credentials on
the VM. `/health` reports `telegramDelivery: device-direct`; the dashboard shows
the non-secret configuration and delivery state reported by the controller.

Do not publicly enable the host Nginx site while `AUTH_MODE=development` is active.

## Rollback

Run the **Roll back production** workflow from GitHub Actions. It uses the same protected
environment and activates the previous healthy set of image digests. A failed ordinary deployment
automatically attempts this rollback before reporting failure.

For disaster recovery before the first immutable release, the deployment wrapper can restore the
original locally built stack from `/home/c/watering-system/compose.vm.yml` without rebuilding it.

Do not add `--volumes` to any Compose recovery command; `watering-system_watering_db` contains the
persistent application database.

## External uptime monitoring

`.github/workflows/uptime.yml` probes `https://watering.sibex.zip` every ten minutes from a GitHub-hosted runner. It verifies the homepage, application and database health, Google authentication mode, the Google authorization redirect host, and the exact production callback URL.

The probe retries three times before declaring an incident. A failure opens and assigns a single GitHub issue to the repository owner; repeated failures reuse that issue. The next successful probe comments on and closes the incident automatically. The workflow can also be run manually from GitHub Actions.
