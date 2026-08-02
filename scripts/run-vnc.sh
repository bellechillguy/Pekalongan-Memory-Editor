#!/bin/sh
set -eu

export DISPLAY="${DISPLAY:-:99}"
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
vnc_password="${PEKALONGAN_VNC_PASSWORD:-}"
if [ -z "$vnc_password" ]; then
    vnc_password="$(od -An -N4 -tx1 /dev/urandom | tr -d ' \n')"
fi
vnc_password_file="/tmp/pekalongan-vnc.pass"

display_number="${DISPLAY#:}"
display_number="${display_number%%.*}"
socket_path="/tmp/.X11-unix/X${display_number}"

Xvfb "$DISPLAY" \
    -screen 0 "${PEKALONGAN_SCREEN:-1280x720x24}" \
    -ac \
    +extension GLX \
    +render \
    -noreset \
    </dev/null >/tmp/pekalongan-xvfb.log 2>&1 &

attempt=0
while [ ! -S "$socket_path" ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        echo "Error: Xvfb tidak siap setelah 10 detik." >&2
        exit 1
    fi
    sleep 0.1
done

x11vnc -storepasswd "$vnc_password" "$vnc_password_file" \
    </dev/null >/tmp/pekalongan-vnc-password.log 2>&1

x11vnc \
    -display "$DISPLAY" \
    -forever \
    -shared \
    -rfbauth "$vnc_password_file" \
    -rfbport 5900 \
    -listen 0.0.0.0 \
    -quiet \
    </dev/null >/tmp/pekalongan-x11vnc.log 2>&1 &

echo "Display virtual aktif pada $DISPLAY."
echo "Buka vnc://localhost:5900 dari macOS."
echo "Password VNC: $vnc_password"

if [ "${PEKALONGAN_FRONTEND:-cli}" = "gui" ]; then
    echo "Frontend: GUI Tkinter"
    exec python3 ./gui/memory_editor_gui.py --editor ./build/memory_editor
fi

echo "Setelah terhubung, launch game di prompt."
exec ./build/memory_editor
