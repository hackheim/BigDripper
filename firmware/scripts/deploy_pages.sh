#!/bin/sh
# Publishes the web UI (web/) to Cloudflare, as a Worker serving static
# files (what Cloudflare Pages has become), where it talks to the board over
# Bluetooth (Chrome on Android or desktop). The same files the firmware
# embeds, minus the docs.
#
# Needs a Cloudflare login once: npx wrangler@4 login
#
# Run from firmware/: sh scripts/deploy_pages.sh
# The Worker is "bigdripper"; set CF_NAME to use another.
set -e
name=${CF_NAME:-bigdripper}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

cp web/*.html web/*.css web/*.js web/*.wasm "$out"/
# Always revalidated, as on the board, so a phone picks up a new deploy
# straight away instead of mixing old and new files.
printf '/*\n  Cache-Control: no-cache\n' > "$out/_headers"

npx --yes wrangler@4 deploy --name "$name" --assets "$out" --compatibility-date 2026-10-01
