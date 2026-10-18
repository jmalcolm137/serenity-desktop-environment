#!/usr/bin/env bash
# Report the GPU/output capabilities R6's remaining rendering work depends on:
# DRM connectors, mode, bits-per-channel range, colour spaces, HDR metadata and
# adaptive-sync support. Run it on the target machine before planning hardware
# validation; it is read-only and does not take DRM master.
set -euo pipefail

ALL=0
case "${1:-}" in
    --all) ALL=1 ;;
    --help|-h)
        echo "usage: $0 [--all]"
        echo "  --all  include disconnected DRM connectors and their properties"
        exit 0
        ;;
    "") ;;
    *) echo "error: unknown option '$1' (try --help)" >&2; exit 2 ;;
esac

echo "SDE rendering capability report"
echo "host=$(hostname) kernel=$(uname -r) date=$(date -Iseconds)"

echo
echo "GPUs:"
if command -v lspci >/dev/null 2>&1; then
    lspci -nnk 2>/dev/null | grep -A4 -E 'VGA compatible|3D controller|Display controller' || echo "  (lspci found no display controllers)"
else
    echo "  lspci not installed"
fi

echo
echo "DRM connectors:"
for card in /sys/class/drm/card[0-9]*-*; do
    [[ -e "$card/status" ]] || continue
    status="$(cat "$card/status" 2>/dev/null || echo '?')"
    [[ "$status" == "connected" || "$ALL" -eq 1 ]] || continue
    echo "  $(basename "$card"): status=$status enabled=$(cat "$card/enabled" 2>/dev/null || echo '?')"
    if [[ -s "$card/modes" ]]; then
        echo "    modes: $(head -5 "$card/modes" | tr '\n' ' ')"
    fi
    if [[ -e "$card/edid" ]] && command -v edid-decode >/dev/null 2>&1; then
        edid-decode "$card/edid" 2>/dev/null | grep -E 'Display Product Name|Manufacturer|Model|Bits per primary color channel|Supported color formats|HDR Static Metadata|Luminance|Color Characteristics' | sed 's/^/    /' | head -12 || true
    fi
done

echo
echo "DRM properties (adaptive sync, HDR, bpc, colour space):"
if command -v modetest >/dev/null 2>&1; then
    tmp="$(mktemp "${TMPDIR:-/tmp}/sde-drm-props.XXXXXX")"
    trap 'rm -f "$tmp"' EXIT
    modetest -c >"$tmp" 2>/dev/null || true
    awk -v all="$ALL" '
        /^[0-9]+[ \t]+[0-9]+[ \t]+(connected|disconnected)/ {
            skip = (all == 0 && $0 ~ /disconnected/)
            if (!skip) {
                print "\n  " $0
                capture = 0
            }
            next
        }
        skip { next }
        /^[ \t]+[0-9]+ (vrr_capable|HDR_OUTPUT_METADATA|max bpc|Colorspace|Broadcast RGB):/ {
            print "    " $0
            capture = 4
            next
        }
        capture > 0 { print "    " $0; capture-- }
    ' "$tmp"
else
    echo "  modetest not installed (libdrm-tests)"
fi

echo
echo "Compositor rendering:"
echo "  wlroots scene direct scanout: enabled by default; set WLR_SCENE_DISABLE_DIRECT_SCANOUT=1 to disable"
echo "  renderer: [Output] Renderer in WindowServer.ini, or SDE_RENDERER=auto|gles2|vulkan|pixman"
echo "  scanout telemetry: SDE_PERF_STATS=<file> exposes scanout_samples= and direct_scanouts="
echo "  direct scanout becomes measurable when a fullscreen client content buffer covers the output"
