#!/usr/bin/env bash
# Nightly PostgreSQL backup (NFR "Operability": nightly backups stored off the server).
#
#   backup.sh schedule   run every day at PONI_BACKUP_TIME_UTC (the container's default)
#   backup.sh once       run one backup now and exit; non-zero if it failed
#
# Each run writes /backups/poni-<UTC time>.dump (pg_dump custom format),
# checks that the archive is readable, uploads it to the Cloud Storage bucket
# PONI_BACKUP_BUCKET if one is set, and removes local dumps older than
# PONI_BACKUP_KEEP_DAYS. The upload runs as the VM's service account, through
# the metadata server: there is no key file. How long the bucket keeps
# objects is a lifecycle rule on the bucket, not something this script does.
#
# The connection comes from the usual PG* variables (PGHOST, PGUSER,
# PGPASSWORD, PGDATABASE). Restore: see deploy/README.md.
set -euo pipefail

dir="${PONI_BACKUP_DIR:-/backups}"
at="${PONI_BACKUP_TIME_UTC:-08:30}"
keep_days="${PONI_BACKUP_KEEP_DAYS:-14}"
bucket="${PONI_BACKUP_BUCKET:-}"

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }

upload() {
  local file="$1" token
  # The token stays in a variable; it is never written to the log.
  token="$(curl -fsS --max-time 10 -H 'Metadata-Flavor: Google' \
    'http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token' \
    | sed -n 's/.*"access_token" *: *"\([^"]*\)".*/\1/p')"
  [[ -n "${token}" ]] || { log "upload failed: no access token from the metadata server"; return 1; }
  curl -fsS --max-time 3600 -X POST -T "${file}" \
    -H "Authorization: Bearer ${token}" -H 'Content-Type: application/octet-stream' \
    "https://storage.googleapis.com/upload/storage/v1/b/${bucket}/o?uploadType=media&name=$(basename "${file}")" \
    >/dev/null
}

# Every step is checked by hand: inside `if ! run_backup`, bash ignores `set -e`.
run_backup() {
  local stamp file
  stamp="$(date -u +%Y%m%dT%H%M%SZ)"
  file="${dir}/poni-${stamp}.dump"
  mkdir -p "${dir}" || return 1

  # Written under a temporary name: a dump that failed halfway never looks finished.
  pg_dump --format=custom --no-owner --file "${file}.partial" || { log "pg_dump failed"; return 1; }
  pg_restore --list "${file}.partial" >/dev/null || { log "the dump is not readable"; return 1; }
  mv "${file}.partial" "${file}" || return 1
  log "backup written: ${file} ($(du -h "${file}" | cut -f1))"

  if [[ -n "${bucket}" ]]; then
    upload "${file}" || { log "upload to gs://${bucket} failed; the local copy is kept"; return 1; }
    log "backup uploaded to gs://${bucket}/$(basename "${file}")"
  else
    log "PONI_BACKUP_BUCKET is not set: the backup exists only on this server"
  fi

  # Local copies are a convenience; the bucket is the backup.
  find "${dir}" -maxdepth 1 -name 'poni-*.dump' -mtime "+${keep_days}" -print -delete | sed 's/^/removed old local backup: /'
  rm -f "${dir}"/poni-*.dump.partial
  date -u +%Y-%m-%dT%H:%M:%SZ > "${dir}/last-success"
}

# Seconds from now until the next HH:MM in UTC.
seconds_until() {
  local now target
  now="$(date -u +%s)"
  target="$(date -u -d "today ${at}" +%s)"
  (( target > now )) || target="$(date -u -d "tomorrow ${at}" +%s)"
  echo $(( target - now ))
}

case "${1:-schedule}" in
  once)
    run_backup
    ;;
  schedule)
    [[ "${at}" =~ ^([01][0-9]|2[0-3]):[0-5][0-9]$ ]] || { log "PONI_BACKUP_TIME_UTC must be HH:MM, got '${at}'"; exit 2; }
    log "backups run daily at ${at} UTC; local copies are kept ${keep_days} days"
    while true; do
      sleep "$(seconds_until)"
      # A failed night is logged and the schedule goes on.
      if ! run_backup; then log "BACKUP FAILED"; fi
      sleep 60  # step past the minute so one night never runs twice
    done
    ;;
  *)
    echo "usage: backup.sh schedule|once" >&2
    exit 2
    ;;
esac
