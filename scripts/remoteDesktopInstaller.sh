#!/usr/bin/env bash
set -Eeuo pipefail

# Installs the same dual-VNC layout used on pidatvmon:
#   port 5900: WayVNC, sharing the physical Wayland desktop
#   port 5901: TigerVNC, providing a separate 1920x1080 XFCE desktop
#
# Usage on the target Raspberry Pi while logged in as the desktop user:
#   sudo bash install-dual-vnc.sh

if [[ ${EUID} -ne 0 ]]; then
    echo "Run this script with sudo: sudo bash $0" >&2
    exit 1
fi

TARGET_USER=${SUDO_USER:-}
if [[ -z ${TARGET_USER} || ${TARGET_USER} == root ]]; then
    echo "Could not detect the normal user behind sudo." >&2
    echo "Log in as the desktop user, then run: sudo bash $0" >&2
    exit 1
fi

if ! id "${TARGET_USER}" >/dev/null 2>&1; then
    echo "User '${TARGET_USER}' does not exist." >&2
    exit 1
fi

TARGET_HOME=$(getent passwd "${TARGET_USER}" | cut -d: -f6)
if [[ -z ${TARGET_HOME} || ! -d ${TARGET_HOME} ]]; then
    echo "Cannot find the home directory for '${TARGET_USER}'." >&2
    exit 1
fi

if [[ ! -r /etc/os-release ]]; then
    echo "Cannot identify this operating system." >&2
    exit 1
fi

. /etc/os-release
if [[ ${ID:-} != raspbian && ${ID:-} != debian ]]; then
    echo "Warning: this was designed for Raspberry Pi OS/Debian 12 Bookworm." >&2
fi

echo "Installing WayVNC, TigerVNC, and XFCE..."
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y \
    wayvnc \
    tigervnc-common \
    tigervnc-standalone-server \
    tigervnc-tools \
    xfce4 \
    xfce4-goodies \
    dbus-x11 \
    xauth \
    acl

# The Raspberry Pi OS wayvnc package supplies its service, vnc system user,
# TLS keys, PAM authentication, display attachment helper, and port 5900 config.
if [[ ! -f /etc/wayvnc/config ]]; then
    install -d -m 0755 /etc/wayvnc
    cat >/etc/wayvnc/config <<'EOF'
use_relative_paths=true
address=::
enable_auth=true
enable_pam=true
private_key_file=tls_key.pem
certificate_file=tls_cert.pem
rsa_private_key_file=rsa_key.pem
EOF
fi

if [[ ! -f /etc/wayvnc/tls_key.pem || ! -f /etc/wayvnc/tls_cert.pem ]]; then
    echo "Generating WayVNC TLS keys..."
    systemctl start wayvnc-generate-keys.service
fi

# An isolated X11/DBus session avoids inheriting variables from the physical
# Wayland desktop and makes XFCE reliable under TigerVNC.
install -d -m 0755 /usr/local/bin /usr/share/xsessions
cat >/usr/local/bin/startxfce4-vnc <<'EOF'
#!/bin/sh
unset DBUS_SESSION_BUS_ADDRESS
unset WAYLAND_DISPLAY
export GDK_BACKEND=x11
export XDG_SESSION_TYPE=x11
exec dbus-run-session -- startxfce4
EOF
chmod 0755 /usr/local/bin/startxfce4-vnc

cat >/usr/share/xsessions/xfce-vnc.desktop <<'EOF'
[Desktop Entry]
Name=XFCE for VNC
Comment=XFCE in an isolated D-Bus X11 session
Exec=/usr/local/bin/startxfce4-vnc
Type=Application
DesktopNames=XFCE
EOF
chmod 0644 /usr/share/xsessions/xfce-vnc.desktop

install -d -m 0700 -o "${TARGET_USER}" -g "${TARGET_USER}" "${TARGET_HOME}/.vnc"
cat >"${TARGET_HOME}/.vnc/config" <<'EOF'
session=xfce-vnc
geometry=1920x1080
depth=24
localhost=no
alwaysshared
securitytypes=VncAuth,TLSVnc
EOF
chown "${TARGET_USER}:${TARGET_USER}" "${TARGET_HOME}/.vnc/config"
chmod 0600 "${TARGET_HOME}/.vnc/config"

echo
echo "Choose the TigerVNC password for port 5901 (6-8 characters)."
while true; do
    read -r -s -p "VNC password: " VNC_PASSWORD
    echo
    read -r -s -p "Confirm password: " VNC_PASSWORD_CONFIRM
    echo

    if [[ ${VNC_PASSWORD} != "${VNC_PASSWORD_CONFIRM}" ]]; then
        echo "Passwords do not match; try again."
        continue
    fi
    if (( ${#VNC_PASSWORD} < 6 || ${#VNC_PASSWORD} > 8 )); then
        echo "TigerVNC passwords must contain 6-8 characters; try again."
        continue
    fi
    break
done

printf '%s\n' "${VNC_PASSWORD}" | runuser -u "${TARGET_USER}" -- \
    tigervncpasswd -f >"${TARGET_HOME}/.vnc/passwd"
chown "${TARGET_USER}:${TARGET_USER}" "${TARGET_HOME}/.vnc/passwd"
chmod 0600 "${TARGET_HOME}/.vnc/passwd"
unset VNC_PASSWORD VNC_PASSWORD_CONFIRM

install -d -m 0755 /etc/tigervnc
touch /etc/tigervnc/vncserver.users
if grep -q '^:1=' /etc/tigervnc/vncserver.users; then
    sed -i "s/^:1=.*/:1=${TARGET_USER}/" /etc/tigervnc/vncserver.users
else
    printf ':1=%s\n' "${TARGET_USER}" >>/etc/tigervnc/vncserver.users
fi
chmod 0644 /etc/tigervnc/vncserver.users

systemctl daemon-reload

# RealVNC may be preinstalled on Raspberry Pi OS. Keep the package, but disable
# its services so it cannot compete with WayVNC for port 5900.
systemctl disable --now vncserver-x11-serviced.service 2>/dev/null || true
systemctl disable --now vncserver-virtuald.service 2>/dev/null || true

systemctl enable wayvnc.service wayvnc-control.service
systemctl enable "tigervncserver@:1.service"

echo "Starting TigerVNC..."
systemctl restart "tigervncserver@:1.service"

# WayVNC needs an active wlroots-compatible Wayland session to attach to.
# Restarting it here is safe; it will wait if the physical desktop is not ready.
echo "Starting WayVNC..."
systemctl restart wayvnc.service

echo
echo "Installed versions:"
dpkg-query -W -f='  ${binary:Package} ${Version}\n' \
    wayvnc tigervnc-standalone-server 2>/dev/null || true

echo
echo "Listening VNC ports:"
ss -lnt 2>/dev/null | grep -E ':(5900|5901)[[:space:]]' || \
    echo "  Services may still be starting; check with: systemctl status wayvnc 'tigervncserver@:1'"

echo
echo "Done. Connect to:"
echo "  <pi-address>:5900  physical Wayland desktop (Linux/PAM login)"
echo "  <pi-address>:5901  separate 1920x1080 XFCE desktop (VNC password)"
echo
echo "Security note: both ports are exposed to the local network. Do not forward"
echo "ports 5900 or 5901 to the public internet; use a VPN or SSH tunnel instead."
